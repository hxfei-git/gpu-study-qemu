/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU 双进程队列回归：A57 Host 经 vfio-user 驱动远端设备与 RV64 CP 固件。
 *
 * 进程 1 是 qemu-system-riscv64 -machine gpgpu-cp，运行设备模型和真实 CP
 * 固件；进程 2 是 A57 virt，测试代码在其中扮演驱动：配置 Outbound、IH 与
 * KIQ，经管理队列映射 HIQ 和用户队列，再提交 AQL 包并核对完成发布。
 *
 * 地址约定：Host RAM 中的对象在设备协议中写 GPU 物理地址
 * GPA(host) = VRAM 容量 + Host IOVA；未启用 IOMMU 时 IOVA 即客体物理地址。
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "qobject/qdict.h"
#include "gpgpu-test.h"

/* Host RAM 中的测试对象布局，均位于 virt 默认 RAM 内。 */
#define QUEUE_HOST 0x41000000ULL
#define QUEUE_MQD(slot) (QUEUE_HOST + (slot) * 0x100)
#define QUEUE_RING(slot) (QUEUE_HOST + 0x1000 + (slot) * 0x1000)
#define QUEUE_WPTR(slot) (QUEUE_HOST + 0x6000 + (slot) * 16)
#define QUEUE_RPTR(slot) (QUEUE_WPTR(slot) + 8)
#define QUEUE_IH (QUEUE_HOST + 0x8000)
#define QUEUE_SIGNAL(task) (QUEUE_HOST + 0x9000 + (task) * 32)
#define QUEUE_MAILBOX(task) (QUEUE_HOST + 0xa000 + (task) * 8)
#define QUEUE_ARGS(task) (QUEUE_HOST + 0xb000 + (task) * 8)
#define QUEUE_RESULT (QUEUE_HOST + 0xc000)
/* Ring 容量：管理 Ring 8 个 32 字节槽，AQL Ring 4 个 64 字节槽。 */
#define QUEUE_RING_BYTES 256

/* VRAM 中的 Kernel 描述对象、代码和输出数组。 */
#define KERNEL_DESC 0x10000U
#define KERNEL_CODE 0x11000U
#define KERNEL_OUT 0x12000U

#define GPA(host) (GPGPU_TEST_VRAM_SIZE + (host))
#define AQL_DISPATCH_1D ((1U << 16) | GPGPU_AQL_HEADER)

typedef struct CPTest {
	QTestState *remote;
	char *directory;
	char *socket;
	char *log;
	QTestState *host;
	QGenericPCIBus bus;
	QPCIDevice *dev;
	QPCIBar regs;
	QPCIBar vram;
	QPCIBar doorbell;
	bool gpuvm;
	uint64_t sequence;
} CPTest;

static void save_device(QPCIDevice *dev, int devfn, void *opaque)
{
	*(QPCIDevice **)opaque = dev;
}

/*
 * cp_start() - 启动两个 QEMU 进程并映射设备 BAR
 *
 * 1. 远端进程创建 gpgpu-cp 机器，经 QMP 建立 vfio-user 服务端；
 * 2. A57 Host 以 vfio-user-pci 连接该 socket；
 * 3. 找到设备、打开 Bus Master，映射 BAR0、BAR2、BAR4。
 */
static void cp_start(CPTest *s, bool gpuvm)
{
	const char *logs = g_getenv("QTEST_GPGPU_REMOTE_LOG_DIR");
	static unsigned int run;
	g_autofree char *quoted_log = NULL;
	g_autofree char *remote_args = NULL;
	g_autofree char *device = NULL;
	g_autofree char *quoted_device = NULL;

	g_assert_nonnull(g_getenv("QTEST_GPGPU_REMOTE_BINARY"));
	s->gpuvm = gpuvm;

	/* 1. 远端设备进程。 */
	s->directory = g_dir_make_tmp("gpgpu-qtest-XXXXXX", NULL);
	g_assert_nonnull(s->directory);
	s->socket = g_build_filename(s->directory, "gpu.sock", NULL);
	if (logs)
		g_assert_cmpint(g_mkdir_with_parents(logs, 0700), ==, 0);
	s->log = g_strdup_printf("%s/device-%u.log", logs ? logs : s->directory, ++run);
	quoted_log = g_shell_quote(s->log);
	remote_args = g_strdup_printf("-machine gpgpu-cp,auto-shutdown=off -accel tcg "
				      "-nodefaults -serial none -global gpgpu.gpuvm=%s 2>%s",
				      gpuvm ? "on" : "off", quoted_log);
	s->remote = qtest_init_ext("QTEST_GPGPU_REMOTE_BINARY", remote_args, NULL, true);
	qtest_qmp_assert_success(s->remote,
				 "{ 'execute': 'object-add', 'arguments': {"
				 "'qom-type': 'x-vfio-user-server', 'id': 'vfu0', 'device': 'gpu0',"
				 "'socket': { 'type': 'unix', 'path': %s } } }",
				 s->socket);

	/* 2. A57 Host 进程。 */
	device = g_strdup_printf("{\"driver\":\"vfio-user-pci\",\"bus\":\"pcie.0\","
				 "\"x-dma-via-socket\":true,"
				 "\"socket\":{\"type\":\"unix\",\"path\":\"%s\"}}",
				 s->socket);
	quoted_device = g_shell_quote(device);
	s->host = qtest_initf("-machine virt,gic-version=3 -cpu cortex-a57 -m 512M "
			      "-nodefaults -device %s",
			      quoted_device);

	/* 3. 设备 BAR。 */
	qpci_init_generic(&s->bus, s->host, NULL, false);
	qpci_device_foreach(&s->bus.bus, GPGPU_VENDOR_ID, GPGPU_DEVICE_ID, save_device,
			    &s->dev);
	g_assert_nonnull(s->dev);
	qpci_device_enable(s->dev);
	s->regs = qpci_iomap(s->dev, 0, NULL);
	s->vram = qpci_iomap(s->dev, 2, NULL);
	s->doorbell = qpci_iomap(s->dev, 4, NULL);
}

static void cp_stop(CPTest *s)
{
	qpci_iounmap(s->dev, s->regs);
	qpci_iounmap(s->dev, s->vram);
	qpci_iounmap(s->dev, s->doorbell);
	g_free(s->dev);
	qtest_quit(s->host);
	qtest_quit(s->remote);
	g_unlink(s->socket);
	if (!g_getenv("QTEST_GPGPU_REMOTE_LOG_DIR"))
		g_unlink(s->log);
	g_rmdir(s->directory);
	g_free(s->log);
	g_free(s->socket);
	g_free(s->directory);
}

static uint32_t rd(CPTest *s, uint64_t reg)
{
	return qpci_io_readl(s->dev, s->regs, reg);
}

static void wr(CPTest *s, uint64_t reg, uint32_t value)
{
	qpci_io_writel(s->dev, s->regs, reg, value);
}

static void wr64(CPTest *s, uint64_t reg, uint64_t value)
{
	wr(s, reg, (uint32_t)value);
	wr(s, reg + 4, value >> 32);
}

static uint32_t hqd_read(CPTest *s, unsigned int slot, unsigned int field)
{
	wr(s, GPGPU_REG_HQD_SELECT, slot);
	return rd(s, GPGPU_REG_HQD_WINDOW + field);
}

/* 轮询 Host RAM 中的 32 位值，10 秒内未达到预期则断言失败。 */
static void wait_host32(CPTest *s, uint64_t address, uint32_t expected)
{
	int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
	uint32_t value;

	do {
		value = qtest_readl(s->host, address);
		if (value == expected)
			return;
		g_usleep(1000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_cmphex(value, ==, expected);
}

static void wait_queue_error(CPTest *s, unsigned int slot, uint32_t expected)
{
	int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
	uint32_t value;

	do {
		value = hqd_read(s, slot, GPGPU_HQD_ERROR);
		if (value == expected)
			return;
		g_usleep(1000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_cmpuint(value, ==, expected);
}

/*
 * queue_mqd() - 在 Host RAM 中准备一条队列的 MQD、进度和空 Ring
 * @ring/@rptr/@wptr: 写入 MQD 的地址；GPUVM 用户队列为 GPUVA，否则为 GPU 物理地址。
 */
static void queue_mqd_at(CPTest *s, unsigned int slot, uint64_t ring, uint64_t rptr,
			 uint64_t wptr)
{
	GpgpuMqd mqd = {
		.ring_base = cpu_to_le64(ring),
		.rptr_report_addr = cpu_to_le64(rptr),
		.wptr_poll_addr = cpu_to_le64(wptr),
		.pq_control = cpu_to_le32(QUEUE_RING_BYTES),
		.doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | slot * 8),
		.vmid = cpu_to_le32(slot >= 2 ? slot - 1 : 0),
		.aql_control = cpu_to_le32(slot >= 2),
	};

	qtest_memwrite(s->host, QUEUE_MQD(slot), &mqd, sizeof(mqd));
	qtest_writel(s->host, QUEUE_WPTR(slot), 0);
	qtest_writel(s->host, QUEUE_RPTR(slot), 0);
	if (slot >= 2) {
		for (unsigned int i = 0; i < QUEUE_RING_BYTES / 64; i++)
			qtest_writel(s->host, QUEUE_RING(slot) + i * 64, GPGPU_AQL_INVALID);
	}
}

static void queue_mqd(CPTest *s, unsigned int slot)
{
	queue_mqd_at(s, slot, GPA(QUEUE_RING(slot)), GPA(QUEUE_RPTR(slot)),
		     GPA(QUEUE_WPTR(slot)));
}

/*
 * cp_boot() - 按驱动的冷启动顺序启动 CP 固件
 *
 * 1. 保持 CP 复位时写入固件和复位向量；
 * 2. 使能 Outbound，使 Host RAM 可经 GPU 物理地址访问；
 * 3. 配置并使能 IH Ring；
 * 4. 经 Host 选槽窗口写入 KIQ（槽 0）并激活；
 * 5. 使能设备并解除 CP 复位。
 */
static void cp_boot(CPTest *s, bool gpuvm, uint32_t ih_bytes)
{
	const char *path = g_getenv("QTEST_GPGPU_FIRMWARE");
	g_autofree char *image = NULL;
	gsize size;

	g_assert_nonnull(path);
	g_assert_true(g_file_get_contents(path, &image, &size, NULL));
	g_assert_cmpuint(size, <, GPGPU_FIRMWARE_STACK_BASE - GPGPU_FIRMWARE_BASE);
	cp_start(s, gpuvm);
	qtest_memset(s->host, QUEUE_HOST, 0, 0x10000);

	/* 1. 固件。 */
	g_assert_cmphex(rd(s, GPGPU_REG_CP_CTRL), ==, GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD);
	qpci_memwrite(s->dev, s->vram, GPGPU_FIRMWARE_BASE, image, size);
	wr(s, GPGPU_REG_CP_RESET_VECTOR, GPGPU_FIRMWARE_BASE);

	/* 2. Outbound 使用复位后的固定布局，只需打开。 */
	wr(s, GPGPU_REG_OUTBOUND_CONTROL, GPGPU_OUTBOUND_ENABLE);
	g_assert_cmphex(rd(s, GPGPU_REG_OUTBOUND_STATUS), ==, GPGPU_OUTBOUND_ACK);

	/* 3. IH Ring。 */
	wr64(s, GPGPU_REG_IH_BASE_LO, GPA(QUEUE_IH));
	wr(s, GPGPU_REG_IH_SIZE, ih_bytes);
	wr(s, GPGPU_REG_IH_CTRL, GPGPU_IH_ENABLE);

	/* 4. KIQ。 */
	queue_mqd(s, 0);
	wr(s, GPGPU_REG_HQD_SELECT, 0);
	wr64(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_LO, GPA(QUEUE_MQD(0)));
	wr64(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_RING_LO, GPA(QUEUE_RING(0)));
	wr64(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_RPTR_ADDR_LO, GPA(QUEUE_RPTR(0)));
	wr64(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_WPTR_ADDR_LO, GPA(QUEUE_WPTR(0)));
	wr(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_CONTROL, QUEUE_RING_BYTES);
	wr(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_DOORBELL, GPGPU_DOORBELL_ENABLE);
	wr(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ACTIVE, 1);
	g_assert_cmpuint(rd(s, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ACTIVE), ==, 1);
	g_assert_cmphex(rd(s, GPGPU_REG_ERROR_STATUS), ==, 0);

	/* 5. 启动。 */
	wr(s, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	wr(s, GPGPU_REG_CP_CTRL, 0);
}

/*
 * queue_manage() - 在管理 Ring 中提交一对命令并返回结果码
 *
 * 1. 结果区写入初值；2. 写管理命令和相邻的 WRITE_COMPLETION；
 * 3. 一次发布 W；4. 等 R 越过两条命令，核对完成序号后读结果码。
 */
static uint32_t queue_manage(CPTest *s, unsigned int source, uint32_t opcode, uint64_t mqd)
{
	uint32_t wptr = qtest_readl(s->host, QUEUE_WPTR(source));
	unsigned int slots = QUEUE_RING_BYTES / sizeof(GpgpuManagementPacket);
	GpgpuManagementPacket command = {
		.opcode = cpu_to_le32(opcode),
		.queue_kind = cpu_to_le32(source == 0 ? GPGPU_QUEUE_HIQ : GPGPU_QUEUE_USER_AQL),
		.address = cpu_to_le64(mqd),
	};
	GpgpuManagementPacket completion = {
		.opcode = cpu_to_le32(GPGPU_WRITE_COMPLETION),
		.address = cpu_to_le64(GPA(QUEUE_RESULT)),
		.value = cpu_to_le64(++s->sequence),
	};

	qtest_writeq(s->host, QUEUE_RESULT, 0);
	qtest_writel(s->host, QUEUE_RESULT + 8, GPGPU_MANAGEMENT_PENDING);
	qtest_memwrite(s->host, QUEUE_RING(source) + (wptr % slots) * 32, &command,
		       sizeof(command));
	qtest_memwrite(s->host, QUEUE_RING(source) + ((wptr + 1) % slots) * 32, &completion,
		       sizeof(completion));
	qtest_writel(s->host, QUEUE_WPTR(source), wptr + 2);
	qpci_io_writeq(s->dev, s->doorbell, source * 8, wptr + 2);
	wait_host32(s, QUEUE_RPTR(source), wptr + 2);
	g_assert_cmphex(qtest_readq(s->host, QUEUE_RESULT), ==, s->sequence);
	return qtest_readl(s->host, QUEUE_RESULT + 8);
}

static void queue_map(CPTest *s, unsigned int slot)
{
	g_assert_cmpuint(queue_manage(s, slot == 1 ? 0 : 1, GPGPU_MAP_QUEUES,
				      GPA(QUEUE_MQD(slot))),
			 ==, GPGPU_MANAGEMENT_OK);
}

/*
 * kernel_load() - 在 VRAM 写入一个两参数 Kernel
 *
 * 参数块为 { 目标地址, 值 }，Kernel 执行 *(u32 *)args[0] = args[1]：
 *   lw t0, 0(a0); lw t1, 4(a0); sw t1, 0(t0); ebreak
 */
static void kernel_load(CPTest *s, uint32_t code_entry)
{
	const uint32_t code[] = { 0x00052283, 0x00452303, 0x0062a023, 0x00100073 };
	GpgpuKernelDescriptor desc = {
		.code_entry = cpu_to_le32(code_entry),
		.code_size = cpu_to_le32(sizeof(code)),
		.kernarg_size = cpu_to_le32(8),
		.kernel_abi_version = cpu_to_le32(GPGPU_KERNEL_ABI_VERSION),
	};

	qpci_memwrite(s->dev, s->vram, KERNEL_DESC, &desc, sizeof(desc));
	for (unsigned int i = 0; i < ARRAY_SIZE(code); i++)
		qpci_io_writel(s->dev, s->vram, KERNEL_CODE + 4 * i, code[i]);
}

/*
 * aql_submit() - 填写一个 1×1×1 派发包并发布
 *
 * 1. 准备 Signal（值 1、事件编号、通知槽）与参数；
 * 2. 以 INVALID Header 写入包体；3. 最后写有效 Header，推进 W 并敲 Doorbell。
 * 各地址参数直接写入协议：GPUVM 开启时传 GPUVA，关闭时传 GPU 物理地址。
 */
static void aql_submit(CPTest *s, unsigned int slot, uint32_t number, unsigned int task,
		       uint64_t desc, uint64_t args, uint64_t signal, uint64_t mailbox,
		       uint32_t out)
{
	GpgpuSignal sig = {
		.value = cpu_to_le64(1),
		.event_id = cpu_to_le32(task + 1),
		.event_mailbox_ptr = cpu_to_le64(mailbox),
	};
	GpgpuAqlPacket packet = {
		.full_header = cpu_to_le32(GPGPU_AQL_INVALID),
		.workgroup_size_x = cpu_to_le16(1),
		.workgroup_size_y = cpu_to_le16(1),
		.workgroup_size_z = cpu_to_le16(1),
		.grid_size_x = cpu_to_le32(1),
		.grid_size_y = cpu_to_le32(1),
		.grid_size_z = cpu_to_le32(1),
		.kernel_object = cpu_to_le64(desc),
		.kernarg_address = cpu_to_le64(args),
		.completion_signal = cpu_to_le64(signal),
	};
	uint64_t at = QUEUE_RING(slot) + (number % (QUEUE_RING_BYTES / 64)) * 64;

	/* 1. Signal 与参数。 */
	qtest_memwrite(s->host, QUEUE_SIGNAL(task), &sig, sizeof(sig));
	qtest_writeq(s->host, QUEUE_MAILBOX(task), 0);
	qtest_writel(s->host, QUEUE_ARGS(task), out);
	qtest_writel(s->host, QUEUE_ARGS(task) + 4, 0xaabb0000 + task);

	/* 2. 包体。 */
	qtest_memwrite(s->host, at, &packet, sizeof(packet));

	/* 3. 发布。 */
	qtest_writel(s->host, at, AQL_DISPATCH_1D);
	qtest_writel(s->host, QUEUE_WPTR(slot), number + 1);
	qpci_io_writeq(s->dev, s->doorbell, slot * 8, number + 1);
}

/* GPUVM 关闭时的提交：所有地址都是 GPU 物理地址。 */
static void aql_submit_phys(CPTest *s, unsigned int slot, uint32_t number, unsigned int task)
{
	aql_submit(s, slot, number, task, KERNEL_DESC, GPA(QUEUE_ARGS(task)),
		   GPA(QUEUE_SIGNAL(task)), GPA(QUEUE_MAILBOX(task)), KERNEL_OUT + 4 * task);
}

/* 成功收尾的可见结果：Signal 归零、通知槽为事件编号、Header 回到 INVALID。 */
static void assert_retired(CPTest *s, unsigned int slot, uint32_t number, unsigned int task)
{
	g_assert_cmphex(qtest_readq(s->host, QUEUE_SIGNAL(task)), ==, 0);
	g_assert_cmphex(qtest_readq(s->host, QUEUE_MAILBOX(task)), ==, task + 1);
	g_assert_cmphex(qtest_readl(s->host, QUEUE_RING(slot) +
						     (number % (QUEUE_RING_BYTES / 64)) * 64),
			==, GPGPU_AQL_INVALID);
}

/* 管理队列：映射、查重、名额、卸载顺序及卸载后保存的消费进度。 */
static void test_management(void)
{
	CPTest s = { 0 };

	cp_boot(&s, false, 4096);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_FIRMWARE), ==, GPGPU_PROTOCOL_VERSION);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);

	/* 1. 仍有用户队列时不能卸载 HIQ。 */
	g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(1))), ==,
			 GPGPU_MANAGEMENT_RECOVERY_REQUIRED);
	/* 2. 重复 MQD 与名额耗尽。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_DUPLICATE);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(4))), ==,
			 GPGPU_MANAGEMENT_NO_SLOT);
	/* 3. 正常卸载后再次卸载返回未映射。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_NOT_MAPPED);
	/* 4. 按用户队列 → HIQ 的顺序全部卸载。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(3))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(1))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(qtest_readq(s.host, QUEUE_MQD(1) + offsetof(GpgpuMqd, saved_rptr)), ==,
			 4);
	cp_stop(&s);
}

/*
 * GPUVM 关闭时两条用户队列各执行任务，核对数据、Signal、通知槽、IH 记录、
 * INVALID Header 和卸载保存的 R。
 */
static void test_dispatch(void)
{
	CPTest s = { 0 };

	cp_boot(&s, false, 4096);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);
	kernel_load(&s, KERNEL_CODE);

	aql_submit_phys(&s, 2, 0, 0);
	aql_submit_phys(&s, 3, 0, 1);
	wait_host32(&s, QUEUE_RPTR(2), 1);
	wait_host32(&s, QUEUE_RPTR(3), 1);
	aql_submit_phys(&s, 2, 1, 2);
	wait_host32(&s, QUEUE_RPTR(2), 2);

	for (unsigned int task = 0; task < 3; task++)
		g_assert_cmphex(qpci_io_readl(s.dev, s.vram, KERNEL_OUT + 4 * task), ==,
				0xaabb0000 + task);
	assert_retired(&s, 2, 0, 0);
	assert_retired(&s, 3, 0, 1);
	assert_retired(&s, 2, 1, 2);

	/* 每个成功任务产生一条 SIGNAL_NOTIFY 记录。 */
	g_assert_cmpuint(rd(&s, GPGPU_REG_IH_WPTR), ==, 3);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, type)), ==,
			 GPGPU_IH_SIGNAL_NOTIFY);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, data)), ==, 1);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, mqd_addr)), ==,
			GPA(QUEUE_MQD(2)));

	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(qtest_readq(s.host, QUEUE_MQD(2) + offsetof(GpgpuMqd, saved_rptr)), ==,
			 2);
	cp_stop(&s);
}

/*
 * 错误范围：非法包只停止所属队列，不写成功 Signal、不推进 R；另一条用户
 * 队列继续执行；故障队列卸载后保存首次错误，带错误的 MQD 不能直接重装。
 */
static void test_fault_scope(void)
{
	CPTest s = { 0 };
	uint64_t at = QUEUE_RING(2);

	cp_boot(&s, false, 4096);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);
	kernel_load(&s, KERNEL_CODE);

	/* 1. 槽 2：工作组尺寸为 0 的非法包。 */
	aql_submit_phys(&s, 2, 0, 0);
	qtest_writew(s.host, at + offsetof(GpgpuAqlPacket, workgroup_size_x), 0);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_PACKET);
	g_assert_cmpuint(hqd_read(&s, 2, GPGPU_HQD_ACTIVE), ==, 0);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);

	/* 2. 槽 3 不受影响。 */
	aql_submit_phys(&s, 3, 0, 1);
	wait_host32(&s, QUEUE_RPTR(3), 1);
	assert_retired(&s, 3, 0, 1);

	/* 3. IH 中依次是 QUEUE_ERROR 与 SIGNAL_NOTIFY。 */
	g_assert_cmpuint(rd(&s, GPGPU_REG_IH_WPTR), ==, 2);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, type)), ==,
			 GPGPU_IH_QUEUE_ERROR);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, data)), ==,
			 GPGPU_QUEUE_ERROR_PACKET);

	/* 4. 故障队列可直接卸载，MQD 保存首次错误，重装被拒绝。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_MQD(2) + offsetof(GpgpuMqd, saved_error)), ==,
			 GPGPU_QUEUE_ERROR_PACKET);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_INVALID);
	cp_stop(&s);
}

/* GPUVM 测试的 GPUVA 布局与 VRAM 页表根。 */
#define VM_ROOT 0x20000U
#define VM_RING 0x00100000U
#define VM_PROGRESS 0x00101000U
#define VM_ARGS 0x00102000U
#define VM_SIGNAL 0x00103000U
#define VM_DESC 0x00400000U
#define VM_CODE 0x00401000U
#define VM_DATA 0x00402000U

/* 用户页的映射目标；VRAM 页直接写 VRAM 偏移，Host 页写 GPA。 */
typedef struct VmMapping {
	uint32_t va;
	uint64_t target;
	uint64_t flags;
} VmMapping;

/*
 * vm_leaf() - 返回 GPUVA 叶子项所在的 VRAM 偏移，按需创建中间表
 *
 * 三级 4 KiB 页表：L0 按 VA[31:30] 有 4 项，L1 按 VA[29:21]、L2 按 VA[20:12]
 * 各 512 项。中间项只保存下一级表页地址和 VALID 位。
 */
static uint32_t vm_leaf(CPTest *s, uint32_t va, uint32_t *next_table)
{
	const unsigned int shifts[] = { 30, 21 };
	uint32_t table = VM_ROOT;

	for (unsigned int level = 0; level < 2; level++) {
		uint32_t index = (va >> shifts[level]) & (level ? 511 : 3);
		uint32_t entry_at = table + index * 8;
		uint64_t entry = qpci_io_readq(s->dev, s->vram, entry_at);

		if (!(entry & GPGPU_VM_VALID)) {
			entry = *next_table | GPGPU_VM_VALID;
			*next_table += GPGPU_VM_PAGE_SIZE;
			qpci_io_writeq(s->dev, s->vram, entry_at, entry);
		}
		table = entry & GPGPU_VM_ADDR_MASK;
	}
	return table + ((va >> 12) & 511) * 8;
}

/*
 * vm_prepare() - 建立 VMID 1 的地址空间并映射用户队列（槽 2）
 * @code_flags: 代码页权限，用于构造取指权限故障。
 *
 * 1. 清空表页区并写入各页映射；2. 写根、同步失效并启用上下文；
 * 3. 以 GPUVA 准备 MQD，经 HIQ 映射。
 */
static void vm_prepare(CPTest *s, uint64_t code_flags)
{
	const uint64_t rw = GPGPU_VM_READ | GPGPU_VM_WRITE;
	const VmMapping maps[] = {
		{ VM_RING, GPA(QUEUE_RING(2)), rw },
		{ VM_PROGRESS, GPA(QUEUE_HOST + 0x6000), rw },
		{ VM_ARGS, GPA(QUEUE_HOST + 0xb000), GPGPU_VM_READ },
		{ VM_SIGNAL, GPA(QUEUE_HOST + 0x9000), rw },
		{ GPGPU_VM_NOTIFY_VA, GPA(QUEUE_HOST + 0xa000), rw },
		{ VM_DESC, KERNEL_DESC, GPGPU_VM_READ },
		{ VM_CODE, KERNEL_CODE, code_flags },
		{ VM_DATA, KERNEL_OUT, rw },
	};
	uint32_t next_table = VM_ROOT + GPGPU_VM_PAGE_SIZE;

	cp_boot(s, true, 4096);
	queue_mqd(s, 1);
	queue_map(s, 1);
	kernel_load(s, VM_CODE);

	/* 1. 页表。 */
	for (uint32_t off = 0; off < 8 * GPGPU_VM_PAGE_SIZE; off += 8)
		qpci_io_writeq(s->dev, s->vram, VM_ROOT + off, 0);
	for (unsigned int i = 0; i < ARRAY_SIZE(maps); i++)
		qpci_io_writeq(s->dev, s->vram, vm_leaf(s, maps[i].va, &next_table),
			       maps[i].target | maps[i].flags | GPGPU_VM_VALID);

	/* 2. 上下文。 */
	wr(s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ROOT_LO, VM_ROOT);
	wr(s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
	wr(s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE, 1);
	g_assert_cmphex(rd(s, GPGPU_REG_ERROR_STATUS), ==, 0);

	/* 3. 用户队列。 */
	queue_mqd_at(s, 2, VM_RING, VM_PROGRESS + 40, VM_PROGRESS + 32);
	queue_map(s, 2);
}

static void vm_submit(CPTest *s)
{
	aql_submit(s, 2, 0, 0, VM_DESC, VM_ARGS, VM_SIGNAL, GPGPU_VM_NOTIFY_VA, VM_DATA);
}

/* 全部访问经 GPUVA 翻译：Kernel 写入 VM_DATA，落到 VRAM KERNEL_OUT。 */
static void test_gpuvm_translation(void)
{
	CPTest s = { 0 };

	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	vm_submit(&s);
	wait_host32(&s, QUEUE_RPTR(2), 1);
	g_assert_cmphex(qpci_io_readl(s.dev, s.vram, KERNEL_OUT), ==, 0xaabb0000);
	assert_retired(&s, 2, 0, 0);

	/* 队列存活时拒绝失效；卸载后允许。 */
	wr(&s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ERROR);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	wr(&s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
	cp_stop(&s);
}

/* 代码页缺少 EXEC：执行器失败，源槽记录取指权限故障，Signal 保持 1。 */
static void test_gpuvm_exec_fault(void)
{
	const uint64_t fault = GPGPU_REG_VM_FAULT(2);
	CPTest s = { 0 };

	vm_prepare(&s, GPGPU_VM_READ);
	vm_submit(&s);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_EXECUTOR);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_REASON), ==, GPGPU_VM_FAULT_PERMISSION);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_VMID), ==, 1);
	g_assert_cmphex(rd(&s, fault + GPGPU_VM_FAULT_ADDR_LO), ==, VM_CODE);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_ACCESS), ==, GPGPU_VM_EXEC);
	g_assert_cmphex(rd(&s, fault + GPGPU_VM_FAULT_PHASE), ==, GPGPU_VM_PHASE_FETCH);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	cp_stop(&s);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	qtest_add_func("/gpgpu/cp/management", test_management);
	qtest_add_func("/gpgpu/cp/dispatch", test_dispatch);
	qtest_add_func("/gpgpu/cp/fault-scope", test_fault_scope);
	qtest_add_func("/gpgpu/cp/gpuvm-translation", test_gpuvm_translation);
	qtest_add_func("/gpgpu/cp/gpuvm-exec-fault", test_gpuvm_exec_fault);
	return g_test_run();
}
