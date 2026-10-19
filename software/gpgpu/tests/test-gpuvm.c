/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "internal.h"
#include "queue_helpers.h"
#include "vector_add.h"

#define CHECK(x)                                                                                   \
	do {                                                                                       \
		if (!(x)) {                                                                        \
			fprintf(stderr, "GPUVM 检查失败 %s:%d: %s errno=%d\n", __FILE__, __LINE__, \
				#x, errno);                                                        \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define OK(x) CHECK((x) == GPGPU_KMT_SUCCESS)
#define N 2051U
#define BYTES (N * sizeof(float))
#define RW (GPGPU_VM_READ | GPGPU_VM_WRITE)

/* 所有对象在建队列前准备，代码与输入/输出分别使用不同页权限。 */
typedef struct VmFixture {
	GpgpuKmtContext *context;
	GpgpuKmtMemory ring;
	GpgpuKmtMemory progress;
	GpgpuKmtMemory signal;
	GpgpuKmtMemory args;
	GpgpuKmtMemory staging;
	GpgpuKmtMemory code;
	GpgpuKmtMemory desc;
	GpgpuKmtMemory x;
	GpgpuKmtMemory y;
	GpgpuKmtMemory z;
	GpgpuKmtQueue queue;
	GpgpuKmtEvent event;
	float seed;
} VmFixture;

/* 先固定全部映射，再上传代码、描述和跨页数组。 */
static void prepare(VmFixture *f, float seed, uint32_t code_perms, uint32_t out_perms)
{
	GpgpuKernelDescriptor desc;
	float *values;
	uint32_t args[4];

	memset(f, 0, sizeof(*f));
	f->seed = seed;
	OK(gpgpuKmtOpen("/dev/dri/renderD128", &f->context));
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 4096, 64, &f->ring));
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 16, 8, &f->progress));
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 24, 8, &f->signal));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_HOST, 16, 8, GPGPU_VM_READ, 0, &f->args));
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 16384, 8, &f->staging));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_VRAM, sizeof(vector_add_code), 4,
				 code_perms, 0x00401000, &f->code));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_VRAM, 16, 8, GPGPU_VM_READ, 0x00400000,
				 &f->desc));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_VRAM, BYTES, 4, GPGPU_VM_READ, 0x00500000,
				 &f->x));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_VRAM, BYTES, 4, GPGPU_VM_READ, 0x00600000,
				 &f->y));
	OK(gpgpuKmtAllocMemoryEx(f->context, GPGPU_MEMORY_VRAM, BYTES, 4, out_perms, 0x00700000,
				 &f->z));
	CHECK(!f->x.vram_offset && !f->args.host_address);
	memcpy(f->staging.cpu, vector_add_code, sizeof(vector_add_code));
	OK(gpgpuKmtCopyMemory(&f->staging, 0, &f->code, 0, sizeof(vector_add_code),
			      GPGPU_COPY_H2D));
	desc = (GpgpuKernelDescriptor) {
		.code_entry = htole32(f->code.gpu_va),
		.code_size = htole32(sizeof(vector_add_code)),
		.kernarg_size = htole32(16),
	};
	memcpy(f->staging.cpu, &desc, sizeof(desc));
	OK(gpgpuKmtCopyMemory(&f->staging, 0, &f->desc, 0, sizeof(desc), GPGPU_COPY_H2D));
	values = f->staging.cpu;
	for (unsigned int i = 0; i < N; i++)
		values[i] = seed + i;
	OK(gpgpuKmtCopyMemory(&f->staging, 0, &f->x, 0, BYTES, GPGPU_COPY_H2D));
	for (unsigned int i = 0; i < N; i++)
		values[i] = i * 2;
	OK(gpgpuKmtCopyMemory(&f->staging, 0, &f->y, 0, BYTES, GPGPU_COPY_H2D));
	args[0] = htole32(f->x.gpu_va);
	args[1] = htole32(f->y.gpu_va);
	args[2] = htole32(f->z.gpu_va);
	args[3] = htole32(N);
	memcpy(f->args.cpu, args, sizeof(args));
}

/* 发布一次向量计算；事件只在已经预映射的通知页中分配槽。 */
static void submit(VmFixture *f)
{
	GpgpuAqlPacket packet;
	GpgpuSignal *signal = f->signal.cpu;
	uint32_t number;

	OK(gpgpuKmtCreateQueue(f->context, &f->ring, 0, 4096, &f->progress, 0, 8, &f->queue));
	OK(gpgpuKmtCreateEvent(&f->queue, &f->event));
	*signal = (GpgpuSignal) { .value = htole64(1),
				  .event_id = htole32(f->event.id),
				  .event_mailbox_ptr = htole64(f->event.mailbox_address) };
	packet = (GpgpuAqlPacket) {
		.full_header = htole32((1U << 16) | GPGPU_AQL_HEADER),
		.workgroup_size_x = htole16(64),
		.workgroup_size_y = htole16(1),
		.workgroup_size_z = htole16(1),
		.grid_size_x = htole32((N + 63) / 64 * 64),
		.grid_size_y = htole32(1),
		.grid_size_z = htole32(1),
		.kernel_object = htole64(f->desc.gpu_va),
		.kernarg_address = htole64(f->args.gpu_va),
		.completion_signal = htole64(f->signal.gpu_va),
	};
	OK(queueReserve(&f->queue, &number));
	queuePublish(&f->queue, number, &packet);
}

static void verify(VmFixture *f, GpgpuKmtMemory *out)
{
	float *values = f->staging.cpu;

	OK(queueWaitSignal(f->signal.cpu, &f->event, 10000));
	OK(queueDrain(&f->queue, 10000));
	OK(gpgpuKmtCopyMemory(&f->staging, 0, out, 0, BYTES, GPGPU_COPY_D2H));
	for (unsigned int i = 0; i < N; i++)
		CHECK(values[i] == f->seed + 3 * i);
}

/* 正常与故障队列都先卸载，再销毁事件；对象由最后关闭统一清理。 */
static void unload(VmFixture *f)
{
	OK(gpgpuKmtDestroyQueue(&f->queue));
	OK(gpgpuKmtDestroyEvent(&f->event));
}

static void test_objects(void)
{
	VmFixture f;
	struct gpgpu_vm_info info;
	GpgpuKmtMemory refused;

	prepare(&f, 7, GPGPU_VM_READ | GPGPU_VM_EXEC, RW);
	OK(gpgpuKmtGetVmInfo(f.context, &info));
	CHECK(info.address_bits == 32 && info.page_size == 4096 && info.range_count == 3);
	CHECK(info.bindings == 1 && info.queues_per_vm == 1);
	CHECK(gpgpuKmtAllocMemoryEx(f.context, GPGPU_MEMORY_HOST, 4096, 4096, RW, GPGPU_VM_CTRL_VA,
				    &refused) == GPGPU_KMT_INVALID_PARAMETER);
	submit(&f);
	CHECK(gpgpuKmtFreeMemory(&f.x) == GPGPU_KMT_UNAVAILABLE);
	CHECK(gpgpuKmtAllocMemory(f.context, GPGPU_MEMORY_HOST, 4096, 8, &refused) ==
	      GPGPU_KMT_UNAVAILABLE);
	verify(&f, &f.z);
	OK(gpgpuKmtClose(f.context));
	puts("PASS gpuvm-group-1");
}

/* 两个真实进程先同时持有地址空间；父进程验证第三个 VM 不能偷取编号。 */
static void isolation_child(int ready, int go, unsigned int index)
{
	VmFixture f;
	char byte;

	prepare(&f, 100 + index, GPGPU_VM_READ | GPGPU_VM_EXEC, RW);
	CHECK(write(ready, "R", 1) == 1);
	CHECK(read(go, &byte, 1) == 1);
	submit(&f);
	verify(&f, &f.z);
	OK(gpgpuKmtClose(f.context));
	_exit(0);
}

static void test_isolation(void)
{
	int ready[2], go[2], status;
	pid_t children[2];
	char byte;
	GpgpuKmtContext *c;
	GpgpuKmtMemory m;

	CHECK(pipe(ready) == 0 && pipe(go) == 0);
	for (unsigned int i = 0; i < 2; i++) {
		children[i] = fork();
		CHECK(children[i] >= 0);
		if (!children[i])
			isolation_child(ready[1], go[0], i);
	}
	for (unsigned int i = 0; i < 2; i++)
		CHECK(read(ready[0], &byte, 1) == 1);
	OK(gpgpuKmtOpen("/dev/dri/renderD128", &c));
	CHECK(gpgpuKmtAllocMemory(c, GPGPU_MEMORY_HOST, 4096, 8, &m) == GPGPU_KMT_OUT_OF_RESOURCES);
	OK(gpgpuKmtClose(c));
	CHECK(write(go[1], "GG", 2) == 2);
	for (unsigned int i = 0; i < 2; i++) {
		CHECK(waitpid(children[i], &status, 0) == children[i]);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	close(ready[0]);
	close(ready[1]);
	close(go[0]);
	close(go[1]);
	puts("PASS gpuvm-group-2");
}

/* 权限和缺页均终止源队列；完成前故障必须保留 Signal=1。 */
static void test_faults(void)
{
	for (unsigned int mode = 0; mode < 3; mode++) {
		VmFixture f;
		struct gpgpu_vm_fault fault;

		prepare(&f, 9, mode == 0 ? GPGPU_VM_READ : GPGPU_VM_READ | GPGPU_VM_EXEC,
			mode == 1 ? GPGPU_VM_READ : RW);
		if (mode == 2)
			((uint32_t *)f.args.cpu)[0] = htole32(0x00800000);
		submit(&f);
		CHECK(gpgpuKmtWaitEvent(&f.event, 10000) == GPGPU_KMT_WAIT_FAILURE);
		OK(gpgpuKmtGetVmFault(&f.queue, &fault));
		CHECK(fault.vmid >= 1 && fault.vmid <= 2);
		CHECK(fault.reason ==
		      (mode == 2 ? GPGPU_VM_FAULT_UNMAPPED : GPGPU_VM_FAULT_PERMISSION));
		CHECK(fault.access ==
		      (mode == 0	 ? GPGPU_VM_EXEC :
			       mode == 1 ? GPGPU_VM_WRITE :
					   GPGPU_VM_READ));
		CHECK(le64toh(((GpgpuSignal *)f.signal.cpu)->value) == 1);
		OK(gpgpuKmtClose(f.context));
	}
	puts("PASS gpuvm-group-3");
}

/* 保留旧 BO，使同一 GPUVA 必须切换到不同物理存储，检查旧 TLB 不再生效。 */
static void test_remap(void)
{
	VmFixture f;
	GpgpuKmtMemory replacement;
	struct gpgpu_vm_map unmap;
	uint64_t address;

	prepare(&f, 19, GPGPU_VM_READ | GPGPU_VM_EXEC, RW);
	submit(&f);
	verify(&f, &f.z);
	unload(&f);
	address = f.z.gpu_va;
	unmap = (struct gpgpu_vm_map) { .version = GPGPU_DRM_VERSION, .address = address };
	CHECK(ioctl(f.context->fd, GPGPU_DRM_UNMAP, &unmap) == 0);
	f.z.gpu_va = 0;
	OK(gpgpuKmtAllocMemoryEx(f.context, GPGPU_MEMORY_VRAM, BYTES, 4, RW, address,
				 &replacement));
	f.seed = 91;
	for (unsigned int i = 0; i < N; i++)
		((float *)f.staging.cpu)[i] = f.seed + i;
	OK(gpgpuKmtCopyMemory(&f.staging, 0, &f.x, 0, BYTES, GPGPU_COPY_H2D));
	*(uint32_t *)f.progress.cpu = 0;
	*(uint32_t *)((char *)f.progress.cpu + 8) = 0;
	submit(&f);
	verify(&f, &replacement);
	OK(gpgpuKmtCopyMemory(&f.staging, 0, &f.z, 0, BYTES, GPGPU_COPY_D2H));
	CHECK(((float *)f.staging.cpu)[N - 1] == 19 + 3 * (N - 1));
	OK(gpgpuKmtClose(f.context));
	puts("PASS gpuvm-group-4");
}

/* 同一 DRM 文件内并发创建和关闭句柄，覆盖 GEM 回调先于创建返回的窗口。 */
typedef struct HandleRace {
	int fd;		   /* 参与竞争的文件描述符。 */
	unsigned int stop; /* 创建线程发布的结束标记。 */
} HandleRace;

static void *close_handles(void *opaque)
{
	HandleRace *race = opaque;

	while (!__atomic_load_n(&race->stop, __ATOMIC_ACQUIRE)) {
		for (unsigned int handle = 1; handle <= 8; handle++) {
			struct gpgpu_gem_close release = { .handle = handle };
			int ret = ioctl(race->fd, GPGPU_DRM_GEM_CLOSE, &release);

			CHECK(!ret || errno == EINVAL || errno == ENOENT);
		}
	}
	return NULL;
}

static void test_handle_race(void)
{
	HandleRace race = { .fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC) };
	pthread_t closer;

	CHECK(race.fd >= 0);
	CHECK(pthread_create(&closer, NULL, close_handles, &race) == 0);
	for (unsigned int i = 0; i < 256; i++) {
		struct gpgpu_bo_create bo = { .version = GPGPU_DRM_VERSION,
					      .kind = GPGPU_MEMORY_HOST,
					      .size = 4096,
					      .alignment = 4096 };
		int ret = ioctl(race.fd, GPGPU_DRM_BO_CREATE, &bo);

		CHECK(!ret || errno == ENOENT);
		if (!ret) {
			struct gpgpu_gem_close release = { .handle = bo.handle };

			ret = ioctl(race.fd, GPGPU_DRM_GEM_CLOSE, &release);
			CHECK(!ret || errno == EINVAL || errno == ENOENT);
		}
	}
	__atomic_store_n(&race.stop, 1, __ATOMIC_RELEASE);
	CHECK(pthread_join(closer, NULL) == 0);
	close(race.fd);
}

/* 验证 GEM 句柄关闭、VMA 引用、跨文件 mmap 授权与显存复用清零。 */
static void test_lifetime(void)
{
	int a = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	int b = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gpgpu_bo_create bo = { .version = GPGPU_DRM_VERSION,
				      .kind = GPGPU_MEMORY_HOST,
				      .size = 4096,
				      .alignment = 4096 };
	struct gpgpu_gem_close release;
	struct gpgpu_vm_map map;
	void *cpu;

	CHECK(a >= 0 && b >= 0);
	CHECK(ioctl(a, GPGPU_DRM_BO_CREATE, &bo) == 0);
	CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, b, bo.mmap_offset) ==
	      MAP_FAILED);
	cpu = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, a, bo.mmap_offset);
	CHECK(cpu != MAP_FAILED);
	for (unsigned int i = 0; i < 4096; i++)
		CHECK(((unsigned char *)cpu)[i] == 0);
	memset(cpu, 0x5a, 4096);
	map = (struct gpgpu_vm_map) { .version = GPGPU_DRM_VERSION,
				      .handle = bo.handle,
				      .address = 0x01000000,
				      .permissions = RW };
	CHECK(ioctl(a, GPGPU_DRM_MAP, &map) == 0);
	release = (struct gpgpu_gem_close) { .handle = bo.handle };
	CHECK(ioctl(a, GPGPU_DRM_GEM_CLOSE, &release) == 0);
	map.handle = map.permissions = 0;
	CHECK(ioctl(a, GPGPU_DRM_UNMAP, &map) == 0);
	close(a);
	CHECK(((unsigned char *)cpu)[4095] == 0x5a);
	CHECK(munmap(cpu, 4096) == 0);
	close(b);
	test_handle_race();
	for (unsigned int round = 0; round < 6; round++) {
		VmFixture f;

		prepare(&f, round, GPGPU_VM_READ | GPGPU_VM_EXEC, RW);
		OK(gpgpuKmtCopyMemory(&f.staging, 0, &f.z, 0, BYTES, GPGPU_COPY_D2H));
		for (unsigned int i = 0; i < BYTES; i++)
			CHECK(((unsigned char *)f.staging.cpu)[i] == 0);
		submit(&f);
		verify(&f, &f.z);
		OK(gpgpuKmtClose(f.context));
	}
	puts("PASS gpuvm-group-5");
}

/* 独立测试构建覆写管理结果或失效状态，正常模块不包含注入逻辑。 */
static void test_unknown(const char *mode)
{
	VmFixture f;
	GpgpuKmtStatus status;

	if (!strcmp(mode, "invalidate")) {
		GpgpuKmtContext *c;
		GpgpuKmtMemory memory;

		OK(gpgpuKmtOpen("/dev/dri/renderD128", &c));
		CHECK(gpgpuKmtAllocMemory(c, GPGPU_MEMORY_HOST, 4096, 8, &memory) !=
		      GPGPU_KMT_SUCCESS);
		CHECK(gpgpuKmtClose(c) == GPGPU_KMT_UNAVAILABLE);
		/* 退出后驱动保留未知失效的 VM，下一进程验证没有复用。 */
		return;
	}
	prepare(&f, 1, GPGPU_VM_READ | GPGPU_VM_EXEC, RW);
	status = gpgpuKmtCreateQueue(f.context, &f.ring, 0, 4096, &f.progress, 0, 8, &f.queue);
	if (!strcmp(mode, "map")) {
		CHECK(status != GPGPU_KMT_SUCCESS);
	} else {
		OK(status);
		CHECK(gpgpuKmtDestroyQueue(&f.queue) != GPGPU_KMT_SUCCESS);
	}
	CHECK(gpgpuKmtFreeMemory(&f.x) == GPGPU_KMT_UNAVAILABLE);
	CHECK(gpgpuKmtFreeMemory(&f.ring) == GPGPU_KMT_UNAVAILABLE);
	CHECK(gpgpuKmtClose(f.context) != GPGPU_KMT_SUCCESS);
}

/* 异常进程结束后，旧 VMID 必须仍占用；只剩一个可用绑定。 */
static void test_retained(void)
{
	int a = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	int b = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gpgpu_vm_info info = { .version = GPGPU_DRM_VERSION };
	struct gpgpu_bo_create bo = { .version = GPGPU_DRM_VERSION,
				      .kind = GPGPU_MEMORY_HOST,
				      .size = 4096,
				      .alignment = 4096 };
	struct gpgpu_vm_map map = { .version = GPGPU_DRM_VERSION,
				    .address = 0x10000,
				    .permissions = RW };

	CHECK(a >= 0 && b >= 0);
	CHECK(ioctl(a, GPGPU_DRM_INFO, &info) == 0 && info.bindings == 1);
	CHECK(ioctl(a, GPGPU_DRM_BO_CREATE, &bo) == 0);
	map.handle = bo.handle;
	/* 失效注入只针对 VMID 1；另一个上下文必须仍可映射。 */
	CHECK(ioctl(a, GPGPU_DRM_MAP, &map) == 0);
	CHECK(ioctl(b, GPGPU_DRM_BO_CREATE, &bo) == 0);
	map.handle = bo.handle;
	CHECK(ioctl(b, GPGPU_DRM_MAP, &map) < 0 && errno == ENOSPC);
	close(b);
	close(a);
	puts("GPUVM retained resources: PASS");
}

int main(int argc, char **argv)

{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2) {
		if (!strcmp(argv[1], "retained"))
			test_retained();
		else
			test_unknown(argv[1]);
		return 0;
	}
	test_objects();
	test_isolation();
	test_faults();
	test_remap();
	test_lifetime();
	puts("GPUVM tests: 5/5 PASS");
	return 0;
}
