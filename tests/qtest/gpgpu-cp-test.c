/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU 双进程队列回归：A57 Host 经 vfio-user 驱动远端设备与 RV64 CP 固件。
 *
 * 进程 1 是 qemu-system-riscv64 -machine gpgpu-cp，运行设备模型和真实 CP
 * 固件；进程 2 是 A57 virt，测试代码在其中扮演驱动：配置 Outbound、IH 与
 * KIQ，经管理队列映射 HIQ 和用户队列，再提交 AQL 包并核对完成发布。
 *
 * 地址约定：Host RAM 中的对象在设备协议中写 GPU 物理地址
 * GPA(host) = GPGPU_OUTBOUND_BASE + Host IOVA；未启用 IOMMU 时 IOVA 即客体
 * 物理地址。
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "gpgpu/gpgpu_tensor_abi.h"
#include "libqtest.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "qobject/qdict.h"
#include "gpgpu-test.h"

/*
 * Host RAM 中的测试对象布局，均位于 virt 默认 RAM 内。
 * 槽号 0～3 依次是 KIQ、HIQ 和两个用户队列；每个槽有 0x100 字节的
 * MQD 位置、一页 Ring，以及 16 字节进度区（W 在前、R 在后）。
 * task 是测试内的任务编号，每个任务独占 Signal、通知槽和
 * ARGS_BYTES 字节参数块。QUEUE_RESULT 是管理请求的 16 字节结果区，
 * 测试串行提交管理命令，所以全部请求复用这一份。
 */
/* 测试 Kernel 的参数块：目标地址与待写值各占一个 8 字节槽。 */
#define ARGS_BYTES 16
#define QUEUE_HOST 0x41000000ULL
#define QUEUE_MQD(slot) (QUEUE_HOST + (slot) * 0x100)
#define QUEUE_RING(slot) (QUEUE_HOST + 0x1000 + (slot) * 0x1000)
#define QUEUE_WPTR(slot) (QUEUE_HOST + 0x6000 + (slot) * 16)
#define QUEUE_RPTR(slot) (QUEUE_WPTR(slot) + 8)
#define QUEUE_IH (QUEUE_HOST + 0x8000)
#define QUEUE_SIGNAL(task) (QUEUE_HOST + 0x9000 + (task) * 32)
#define QUEUE_MAILBOX(task) (QUEUE_HOST + 0xa000 + (task) * 8)
#define QUEUE_ARGS(task) (QUEUE_HOST + 0xb000 + (task) * ARGS_BYTES)
#define QUEUE_RESULT (QUEUE_HOST + 0xc000)
/* Ring 容量：管理 Ring 8 个 32 字节槽，AQL Ring 4 个 64 字节槽。 */
#define QUEUE_RING_BYTES 256

/* VRAM 中的 Kernel 描述对象、代码和输出数组；任务 n 写 KERNEL_OUT + 4n。 */
#define KERNEL_DESC 0x10000U
#define KERNEL_CODE 0x11000U
#define KERNEL_OUT 0x12000U

/* Host 地址换成设备协议中的 GPU 物理地址：窗口固定从 2 GiB 开始。 */
#define GPA(host) (GPGPU_OUTBOUND_BASE + (host))
/* 有效的一维派发 Header：低 16 位为包类型等字段，setup 写维度数 1。 */
#define AQL_DISPATCH_1D ((1U << 16) | GPGPU_AQL_HEADER)

/*
 * 一次测试的双进程环境：remote 是设备进程，host 是 A57 客体；
 * regs、vram、doorbell 依次映射 BAR0、BAR2、BAR4。sequence 是最近一次
 * 管理请求使用的完成序号，从 1 开始递增。
 */
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
    /* 启动时是否允许张量指令使用安全的宿主 SIMD。 */
    bool tensor_native;
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
 *
 * 必须先建好服务端 socket，第 2 步的 Host 进程才能连上；设备日志在
 * 设置 QTEST_GPGPU_REMOTE_LOG_DIR 时保留，便于失败后查看固件输出。
 */
static void cp_start(CPTest *s, bool gpuvm, bool shared_fetch)
{
	const char *logs = g_getenv("QTEST_GPGPU_REMOTE_LOG_DIR");
	static unsigned int run;
	g_autofree char *quoted_log = NULL;
	g_autofree char *remote_args = NULL;
	g_autofree char *device = NULL;
	g_autofree char *quoted_device = NULL;

	g_assert_nonnull(g_getenv("QTEST_GPGPU_REMOTE_BINARY"));
	s->gpuvm = gpuvm;

	/*
	 * 1. 在临时目录中启动 gpgpu-cp 远端进程，再用 QMP 的 object-add 创建
	 *    vfio-user 服务端，把设备 gpu0 导出到 gpu.sock。设备的 stderr 写入
     *    日志单独保存；属性通过 -global 配置。
	 */
	s->directory = g_dir_make_tmp("gpgpu-qtest-XXXXXX", NULL);
	g_assert_nonnull(s->directory);
	s->socket = g_build_filename(s->directory, "gpu.sock", NULL);
	if (logs)
		g_assert_cmpint(g_mkdir_with_parents(logs, 0700), ==, 0);
	s->log = g_strdup_printf("%s/device-%u.log", logs ? logs : s->directory, ++run);
	quoted_log = g_shell_quote(s->log);
    remote_args = g_strdup_printf("-machine gpgpu-cp,auto-shutdown=off "
                                  "-accel tcg -nodefaults -serial none "
                                  "-global gpgpu.gpuvm=%s "
                                  "-global gpgpu.shared_fetch=%s "
                                  "-global gpgpu.tensor_native=%s 2>%s",
                                  gpuvm ? "on" : "off",
                                  shared_fetch ? "on" : "off",
                                  s->tensor_native ? "on" : "off", quoted_log);
	s->remote = qtest_init_ext("QTEST_GPGPU_REMOTE_BINARY", remote_args, NULL, true);
	qtest_qmp_assert_success(s->remote,
				 "{ 'execute': 'object-add', 'arguments': {"
				 "'qom-type': 'x-vfio-user-server', 'id': 'vfu0', 'device': 'gpu0',"
				 "'socket': { 'type': 'unix', 'path': %s } } }",
				 s->socket);

	/*
	 * 2. 启动 A57 virt 客体，用 vfio-user-pci 连接第 1 步的 socket。
	 *    x-dma-via-socket 让设备对 Host RAM 的 DMA 经 socket 转发。
	 */
	device = g_strdup_printf("{\"driver\":\"vfio-user-pci\",\"bus\":\"pcie.0\","
				 "\"x-dma-via-socket\":true,"
				 "\"socket\":{\"type\":\"unix\",\"path\":\"%s\"}}",
				 s->socket);
	quoted_device = g_shell_quote(device);
	s->host = qtest_initf("-machine virt,gic-version=3 -cpu cortex-a57 -m 512M "
			      "-nodefaults -device %s",
			      quoted_device);

	/*
	 * 3. 在客体 PCIe 总线上按厂商号、设备号找到设备，打开 Memory 与
	 *    Bus Master，再映射寄存器、VRAM 和 Doorbell 三个 BAR，供后续读写。
	 */
	qpci_init_generic(&s->bus, s->host, NULL, false);
	qpci_device_foreach(&s->bus.bus, GPGPU_VENDOR_ID, GPGPU_DEVICE_ID, save_device,
			    &s->dev);
	g_assert_nonnull(s->dev);
	qpci_device_enable(s->dev);
	s->regs = qpci_iomap(s->dev, 0, NULL);
    s->vram = gpgpu_test_map_vram(s->dev);
	s->doorbell = qpci_iomap(s->dev, 4, NULL);
}

/* 关闭两个进程并删除临时文件；设置了日志目录时保留设备日志。 */
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

/* BAR0 寄存器读写：设备只接受对齐的 32 位访问，64 位分两次读写。 */
static uint32_t rd(CPTest *s, uint64_t reg)
{
    return gpgpu_test_readl(s->dev, s->regs, reg);
}

static uint64_t rd64(CPTest *s, uint64_t reg)
{
	return rd(s, reg) | (uint64_t)rd(s, reg + 4) << 32;
}

static void wr(CPTest *s, uint64_t reg, uint32_t value)
{
    gpgpu_test_writel(s->dev, s->regs, reg, value);
}

static void wr64(CPTest *s, uint64_t reg, uint64_t value)
{
	wr(s, reg, (uint32_t)value);
	wr(s, reg + 4, value >> 32);
}

/* 以 Host 身份发出一条 dcache 命令；设备同步执行，返回 STATUS。 */
static uint32_t dcache_cmd(CPTest *s, uint32_t cmd, uint64_t addr, uint32_t length)
{
	wr64(s, GPGPU_REG_DCACHE_ADDR_LO, addr);
	wr(s, GPGPU_REG_DCACHE_LENGTH, length);
	wr(s, GPGPU_REG_DCACHE_CMD, cmd);
	return rd(s, GPGPU_REG_DCACHE_STATUS);
}

/* 先写 HQD_SELECT 选中槽位，再经 Host 选槽窗口读取该槽的一个 HQD 字段。 */
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

/* 轮询 slot 的 HQD ERROR，10 秒内未等到 CP 记下预期错误号则断言失败。 */
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
 * queue_mqd_at() - 在 Host RAM 中准备一条队列的 MQD、进度和空 Ring
 * @ring/@rptr/@wptr: 写入 MQD 的地址；GPUVM 用户队列为 GPUVA，否则为 GPU 物理地址。
 *
 * 槽 0、1 是管理队列，VMID 为 0、aql_control 为 0；槽 2、3 是用户 AQL
 * 队列，VMID 依次为 1、2。Doorbell 偏移按槽号乘 8，与提交时敲的
 * BAR4 地址一致。W、R 清零；用户 Ring 的每个槽先写 INVALID Header，
 * CP 读到 INVALID 就当作尚未发布，不会把旧内容当成包执行。
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

/* GPUVM 关闭时的 MQD：Ring 与进度地址都用 GPU 物理地址。 */
static void queue_mqd(CPTest *s, unsigned int slot)
{
	queue_mqd_at(s, slot, GPA(QUEUE_RING(slot)), GPA(QUEUE_RPTR(slot)),
		     GPA(QUEUE_WPTR(slot)));
}

/*
 * cp_boot() - 按驱动的冷启动顺序启动 CP 固件
 * @ih_bytes: IH Ring 字节容量。
 * @shared_fetch: 在设备启动时选择同轮取指与译码共享。
 *
 * 1. 保持 CP 复位时写入固件和复位向量；
 * 2. 使能 Outbound，使 Host RAM 可经 GPU 物理地址访问；
 * 3. 配置并使能 IH Ring；
 * 4. 经 Host 选槽窗口写入 KIQ（槽 0）并激活；
 * 5. 使能设备并解除 CP 复位。
 *
 * 第 2～4 步都要求 CP 仍保持复位：Outbound 只在设备引擎全部停止时接受
 * 配置，HQD 在 CP 运行后对 Host 只读。KIQ 由 Host 直接装载，HIQ 和
 * 用户队列之后再经管理命令映射。
 */
static void cp_boot(CPTest *s, bool gpuvm, uint32_t ih_bytes, bool shared_fetch)
{
	const char *path = g_getenv("QTEST_GPGPU_FIRMWARE");
	g_autofree char *image = NULL;
	gsize size;

	g_assert_nonnull(path);
	g_assert_true(g_file_get_contents(path, &image, &size, NULL));
	g_assert_cmpuint(size, <, GPGPU_FIRMWARE_STACK_BASE - GPGPU_FIRMWARE_BASE);
    cp_start(s, gpuvm, shared_fetch);
	qtest_memset(s->host, QUEUE_HOST, 0, 0x10000);

	/*
	 * 1. 复位后 CP_CTRL 应为“已连接 CP 且保持复位”。把固件镜像经 BAR2
	 *    写到 VRAM 末尾的固件区，复位向量指向它的入口。
	 */
	g_assert_cmphex(rd(s, GPGPU_REG_CP_CTRL), ==, GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD);
    gpgpu_test_memwrite(s->dev, s->vram, GPGPU_FIRMWARE_BASE, image, size);
	wr(s, GPGPU_REG_CP_RESET_VECTOR, GPGPU_FIRMWARE_BASE);

	/*
	 * 2. Outbound 布局在复位后固定为
	 *    “GPU 物理地址 = GPGPU_OUTBOUND_BASE + Host IOVA”，只需打开；
	 *    状态为 ACK 表示设备接受了这次配置。此后 CP 才能经
	 *    GPA() 换算出的地址访问 Host RAM。
	 */
	wr(s, GPGPU_REG_OUTBOUND_CONTROL, GPGPU_OUTBOUND_ENABLE);
	g_assert_cmphex(rd(s, GPGPU_REG_OUTBOUND_STATUS), ==, GPGPU_OUTBOUND_ACK);

	/*
	 * 3. IH Ring 放在 Host RAM 的 QUEUE_IH。CP 每完成一个任务或
	 *    记录一次队列错误都会写一条记录，测试据此核对通知顺序。
	 */
	wr64(s, GPGPU_REG_IH_BASE_LO, GPA(QUEUE_IH));
	wr(s, GPGPU_REG_IH_SIZE, ih_bytes);
	wr(s, GPGPU_REG_IH_CTRL, GPGPU_IH_ENABLE);

	/*
	 * 4. KIQ 是 CP 启动时唯一要求已激活的队列。先在 Host RAM 准备 MQD，
	 *    再把 MQD、Ring、R/W 地址、容量和 Doorbell 写进槽 0 的 HQD 并置
	 *    ACTIVE。读回 ACTIVE 为 1 且没有全局错误，说明这些写入都被接受。
	 */
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

	/*
	 * 5. 使能设备后清除 RESET_HOLD，CP 从复位向量开始执行。固件先检查
	 *    KIQ 已激活、其他槽为空，再进入调度循环。
	 */
	wr(s, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	wr(s, GPGPU_REG_CP_CTRL, 0);
}

/*
 * queue_manage() - 在管理 Ring 中提交一对命令并返回结果码
 * @source: 0 经 KIQ 管理 HIQ，1 经 HIQ 管理用户队列；决定 queue_kind。
 *
 * 1. 结果区写入初值：完成标记 0、结果码 PENDING；
 * 2. 在 W、W+1 两个槽写管理命令和相邻的 WRITE_COMPLETION，后者让 CP 把
 *    结果码和新序号写回 QUEUE_RESULT；
 * 3. 一次把 W 推进 2 并敲 Doorbell，两条命令同时对 CP 可见；
 * 4. 等 R 越过两条命令，核对完成标记等于本次序号后读结果码。
 *
 * 测试只在上一请求完成后才提交下一请求，结果区可以直接复用。
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
    gpgpu_test_writeq(s->dev, s->doorbell, source * 8, wptr + 2);
	wait_host32(s, QUEUE_RPTR(source), wptr + 2);
	g_assert_cmphex(qtest_readq(s->host, QUEUE_RESULT), ==, s->sequence);
	return qtest_readl(s->host, QUEUE_RESULT + 8);
}

/* 映射槽 slot 的 MQD 并要求成功：HIQ 经 KIQ 映射，用户队列经 HIQ 映射。 */
static void queue_map(CPTest *s, unsigned int slot)
{
	g_assert_cmpuint(queue_manage(s, slot == 1 ? 0 : 1, GPGPU_MAP_QUEUES,
				      GPA(QUEUE_MQD(slot))),
			 ==, GPGPU_MANAGEMENT_OK);
}

/*
 * kernel_load() - 在 VRAM 写入一个两参数 Kernel
 * @code_entry: 描述对象中的代码入口；GPUVM 关闭时为 VRAM 偏移，
 *              开启时为 GPUVA。
 *
 * 参数块为两个 8 字节槽 { 目标地址, 值 }，Kernel 执行
 * *(u32 *)args[0] = args[1]：
 *   ld t0, 0(a0); ld t1, 8(a0); sw t1, 0(t0); ebreak
 *
 * a0 是 RV64 计算执行器传给线程的参数块地址。代码总放在 VRAM 的
 * KERNEL_CODE，描述对象放在 KERNEL_DESC，参数长度固定为 ARGS_BYTES。
 */
static void kernel_load(CPTest *s, uint64_t code_entry)
{
	const uint32_t code[] = { 0x00053283, 0x00853303, 0x0062a023, 0x00100073 };
	GpgpuKernelDescriptor desc = {
		.code_entry = cpu_to_le64(code_entry),
		.code_size = cpu_to_le32(sizeof(code)),
		.kernarg_size = cpu_to_le32(ARGS_BYTES),
		.kernel_abi_version = cpu_to_le32(GPGPU_KERNEL_ABI_VERSION),
	};

    gpgpu_test_memwrite(s->dev, s->vram, KERNEL_DESC, &desc, sizeof(desc));
	for (unsigned int i = 0; i < ARRAY_SIZE(code); i++)
                gpgpu_test_writel(s->dev, s->vram, KERNEL_CODE + 4 * i,
                                  code[i]);
}

/*
 * aql_submit() - 填写一个单工作组的一维派发包并发布
 * @number: 包编号；物理槽为 number 对 Ring 槽数取模。
 * @task: 测试任务编号，选择 Signal、通知槽和参数块，事件编号为 task + 1。
 * @out: 参数块中的目标地址；默认 Kernel 写入 0xaabb0000 + task。
 * @lanes: 工作组与网格的 X 维线程数，发布前填好。
 *
 * 1. 准备 Signal（值 1、事件编号、通知槽）与参数；
 * 2. 以 INVALID Header 写入包体；3. 最后写有效 Header，推进 W 并敲 Doorbell。
 * 各地址参数直接写入协议：GPUVM 开启时传 GPUVA，关闭时传 GPU 物理地址。
 *
 * 测试是单生产者，直接把 W 写成 number + 1，不经过原子预留。
 */
static void aql_submit(CPTest *s, unsigned int slot, uint32_t number,
                       unsigned int task, uint64_t desc, uint64_t args,
                       uint64_t signal, uint64_t mailbox, uint64_t out,
                       unsigned int lanes)
{
	GpgpuSignal sig = {
		.value = cpu_to_le64(1),
		.event_id = cpu_to_le32(task + 1),
		.event_mailbox_ptr = cpu_to_le64(mailbox),
	};
	GpgpuAqlPacket packet = {
		.full_header = cpu_to_le32(GPGPU_AQL_INVALID),
        .workgroup_size_x = cpu_to_le16(lanes),
		.workgroup_size_y = cpu_to_le16(1),
		.workgroup_size_z = cpu_to_le16(1),
        .grid_size_x = cpu_to_le32(lanes),
		.grid_size_y = cpu_to_le32(1),
		.grid_size_z = cpu_to_le32(1),
		.kernel_object = cpu_to_le64(desc),
		.kernarg_address = cpu_to_le64(args),
		.completion_signal = cpu_to_le64(signal),
	};
	uint64_t at = QUEUE_RING(slot) + (number % (QUEUE_RING_BYTES / 64)) * 64;

	/*
	 * 1. Signal 置 1，表示任务尚未完成，并写入事件编号与通知槽地址；
	 *    通知槽清零，完成后 CP 会在其中写入事件编号。参数块依次是
	 *    输出地址和待写值，待写值带上 task，便于区分各任务的结果。
	 */
	qtest_memwrite(s->host, QUEUE_SIGNAL(task), &sig, sizeof(sig));
	qtest_writeq(s->host, QUEUE_MAILBOX(task), 0);
	qtest_writeq(s->host, QUEUE_ARGS(task), out);
	qtest_writeq(s->host, QUEUE_ARGS(task) + 8, 0xaabb0000 + task);

	/*
	 * 2. 整包以 INVALID Header 写入。此时 CP 即使读到这个槽，也会当作
	 *    尚未发布，不会读到写了一半的包体。
	 */
	qtest_memwrite(s->host, at, &packet, sizeof(packet));

	/*
	 * 3. 包体写完后单独改写首个 32 位字（Header 与 setup）发布这个包，
	 *    再推进 W 并向本槽 Doorbell 写入新的 W，通知 CP 检查这条队列。
	 */
	qtest_writel(s->host, at, AQL_DISPATCH_1D);
	qtest_writel(s->host, QUEUE_WPTR(slot), number + 1);
    gpgpu_test_writeq(s->dev, s->doorbell, slot * 8, number + 1);
}

/* GPUVM 关闭时的提交：所有地址都是 GPU 物理地址。 */
static void aql_submit_phys(CPTest *s, unsigned int slot, uint32_t number, unsigned int task)
{
    aql_submit(s, slot, number, task, KERNEL_DESC, GPA(QUEUE_ARGS(task)),
               GPA(QUEUE_SIGNAL(task)), GPA(QUEUE_MAILBOX(task)),
               KERNEL_OUT + 4 * task, 1);
}

/*
 * 成功收尾的可见结果：Signal 归零、通知槽为事件编号、Header 回到 INVALID。
 * CP 先完成这三项写入再推进 R，所以调用前先等到 R 越过该包即可检查。
 */
static void assert_retired(CPTest *s, unsigned int slot, uint32_t number, unsigned int task)
{
	g_assert_cmphex(qtest_readq(s->host, QUEUE_SIGNAL(task)), ==, 0);
	g_assert_cmphex(qtest_readq(s->host, QUEUE_MAILBOX(task)), ==, task + 1);
	g_assert_cmphex(qtest_readl(s->host, QUEUE_RING(slot) +
						     (number % (QUEUE_RING_BYTES / 64)) * 64),
			==, GPGPU_AQL_INVALID);
}

/*
 * test_management() - 管理队列的映射、查重、名额与卸载规则
 *
 * 准备：GPUVM 关闭，经 KIQ 映射 HIQ（槽 1），再经 HIQ 映射槽 2、3 两条
 * 用户队列，用满全设备两个用户队列名额。HIQ 映射成功后，固件已完成
 * 协议版本握手，VM_FIRMWARE 应读到当前协议版本。
 *
 * 期望：用户队列存活时卸载 HIQ 返回 RECOVERY_REQUIRED；重复 MQD 返回
 * DUPLICATE；名额耗尽返回 NO_SLOT；卸载已卸载的 MQD 返回 NOT_MAPPED；
 * 按“用户队列 → HIQ”的顺序可以全部卸载，HIQ 的 MQD 保存最终 R。
 */
static void test_management(void)
{
	CPTest s = { 0 };

    cp_boot(&s, false, 4096, false);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_FIRMWARE), ==, GPGPU_PROTOCOL_VERSION);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);

	/*
	 * 1. 槽 2、3 仍占用时经 KIQ 卸载 HIQ：CP 不会自动连带卸载用户队列，
	 *    返回 RECOVERY_REQUIRED，HIQ 保持映射。
	 */
	g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(1))), ==,
			 GPGPU_MANAGEMENT_RECOVERY_REQUIRED);

	/*
	 * 2. 再次映射槽 2 的 MQD：CP 按完整 MQD 地址查重，返回 DUPLICATE。
	 *    映射新 MQD（位置 4）：两个用户槽都已占用，CP 在读 MQD 之前就
	 *    返回 NO_SLOT，所以这份 MQD 不需要填写内容。
	 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_DUPLICATE);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(4))), ==,
			 GPGPU_MANAGEMENT_NO_SLOT);

	/*
	 * 3. 槽 2 没有提交过任务，R == W，可以正常卸载；再次卸载时已找
	 *    不到这个 MQD 的绑定，返回 NOT_MAPPED，CP 不保存卸载历史。
	 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_NOT_MAPPED);

	/* 4. 先卸载剩下的用户队列（槽 3），之后 HIQ 才能卸载。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(3))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(1))), ==,
			 GPGPU_MANAGEMENT_OK);

	/*
	 * HIQ 上的请求依次是：映射槽 2、映射槽 3、重复映射、名额耗尽、
	 * 卸载槽 2、再次卸载槽 2、卸载槽 3，共 7 对
	 * “命令 + WRITE_COMPLETION”，消费 14 个包。
	 * 卸载 HIQ 时 CP 把 R = 14 零扩展写入 saved_rptr。
	 */
	g_assert_cmpuint(qtest_readq(s.host, QUEUE_MQD(1) + offsetof(GpgpuMqd, saved_rptr)), ==,
			14);
	cp_stop(&s);
}

/*
 * test_dispatch() - GPUVM 关闭时两条用户队列的正常派发与收尾
 *
 * 准备：映射 HIQ 与槽 2、3，在 VRAM 装入写值 Kernel。先向槽 2、3
 * 各提交一个任务（task 0、1），等两者都消费后，再向槽 2 提交
 * 第二个任务（task 2）。
 *
 * 期望：三个任务的输出都写到 VRAM；没有页表给出缓存属性，CP 与执行器的
 * 数据访问都不进入 dcache，命中与未命中计数不变；每个任务的 Signal
 * 归零、通知槽为事件编号、Header 恢复 INVALID；IH 中有 3 条
 * SIGNAL_NOTIFY；卸载槽 2 后 saved_rptr 为 2，即这条队列消费的包数。
 */
static void test_dispatch(void)
{
	CPTest s = { 0 };
	uint32_t hits, misses;

    cp_boot(&s, false, 4096, false);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);
	kernel_load(&s, KERNEL_CODE);
	hits = rd(&s, GPGPU_REG_DCACHE_HITS);
	misses = rd(&s, GPGPU_REG_DCACHE_MISSES);

	aql_submit_phys(&s, 2, 0, 0);
	aql_submit_phys(&s, 3, 0, 1);
	wait_host32(&s, QUEUE_RPTR(2), 1);
	wait_host32(&s, QUEUE_RPTR(3), 1);
	aql_submit_phys(&s, 2, 1, 2);
	wait_host32(&s, QUEUE_RPTR(2), 2);

	for (unsigned int task = 0; task < 3; task++)
                g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram,
                                                KERNEL_OUT + 4 * task),
                                ==, 0xaabb0000 + task);
	g_assert_cmpuint(rd(&s, GPGPU_REG_DCACHE_HITS), ==, hits);
	g_assert_cmpuint(rd(&s, GPGPU_REG_DCACHE_MISSES), ==, misses);
	assert_retired(&s, 2, 0, 0);
	assert_retired(&s, 3, 0, 1);
	assert_retired(&s, 2, 1, 2);

	/*
	 * 每个成功任务产生一条 SIGNAL_NOTIFY 记录。槽 2 的 task 0 最先提交，
	 * CP 的轮流调度也从槽 2 开始，所以第一条记录来自它：数据为事件
	 * 编号 1，mqd_addr 为槽 2 的 MQD 物理地址。
	 */
	g_assert_cmpuint(rd(&s, GPGPU_REG_IH_WPTR), ==, 3);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, type)), ==,
			 GPGPU_IH_SIGNAL_NOTIFY);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, data)), ==, 1);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, mqd_addr)), ==,
			GPA(QUEUE_MQD(2)));

	/* 槽 2 已排空（R == W == 2），正常卸载后 saved_rptr 保存 R = 2。 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(qtest_readq(s.host, QUEUE_MQD(2) + offsetof(GpgpuMqd, saved_rptr)), ==,
			 2);

	cp_stop(&s);
}

/*
 * test_shared_fetch_lanes() - 共享取指的 Lane 顺序与退出
 *
 * 四个 Lane 在同一轮分别执行：0 写 x=base、1 读 x、
 * 2 写 x=base+2、3 读 x。偶数 Lane 随后先 EBREAK，
 * 奇数 Lane 再把读值写到 out[lane_id]。
 * 按 PC 分组执行会破坏这两个观测值；若任一 Lane 的
 * EBREAK 结束整个 Warp，奇数 Lane 也会来不及写结果。
 *
 * 每个模式连续派发两次。
 * 前次收尾后同址改写 ADDI，第二次须用新代码。
 * 再把奇数路径 LW 换成非法指令：
 * Lane1 失败前 Lane0 的写入保留，Lane2 不得写入，
 * 失败任务不得发布成功。shared_fetch 是启动属性，
 * 两个模式各使用独立的双进程环境。
 */
static void test_shared_fetch_lanes(void)
{
    enum {
        /* 单个工作组内的活跃线程数。 */
        ORDER_LANES = 4,
        /* 可替换 ADDI 在下面代码数组中的指令下标。 */
        ORDER_VALUE_INSN = 2,
        /* 奇数 Lane 的 LW 在下面代码数组中的指令下标。 */
        ORDER_LOAD_INSN = 7,
    };
    const uint32_t values[] = { 7, 11 };
    const uint32_t clear[ORDER_LANES] = { 0 };
    const uint32_t code[] = {
        0x00053283, /* ld t0,0(a0)：参数中的共享字地址。 */
        0xcc002373, /* csrr t1,THREAD_ID_X。 */
        0x00030393, /* addi t2,t1,base：发布前填入 12 位立即数。 */
        0x00137e13, /* andi t3,t1,1。 */
        0x000e1663, /* bne t3,zero,+12：奇数 Lane 跳到 LW。 */
        0x0072a023, /* sw t2,0(t0)：偶数 Lane 改写共享字。 */
        0x00100073, /* ebreak：只退出当前偶数 Lane。 */
        0x0002ae83, /* lw t4,0(t0)：奇数 Lane 读取共享字。 */
        0x00231f13, /* slli t5,t1,2。 */
        0x005f0f33, /* add t5,t5,t0。 */
        0x01df2023, /* sw t4,0(t5)：out[lane_id] 保存观测值。 */
        0x00100073, /* ebreak：奇数 Lane 完成。 */
    };
    const unsigned int fault_task = ARRAY_SIZE(values);

    for (unsigned int mode = 0; mode < 2; mode++) {
        CPTest s = { 0 };

        cp_boot(&s, false, 4096, mode != 0);
        queue_mqd(&s, 1);
        queue_map(&s, 1);
        queue_mqd(&s, 2);
        queue_map(&s, 2);
        kernel_load(&s, KERNEL_CODE);
        gpgpu_test_writel(s.dev, s.vram,
                         KERNEL_DESC +
                         offsetof(GpgpuKernelDescriptor, code_size),
                         sizeof(code));
        for (unsigned int i = 0; i < ARRAY_SIZE(code); i++)
            gpgpu_test_writel(s.dev, s.vram,
                             KERNEL_CODE + i * sizeof(code[0]), code[i]);

        for (unsigned int task = 0; task < ARRAY_SIZE(values); task++) {
            uint32_t value = values[task];

            /* 前次已收尾；BAR 写完成后发布新包。 */
            gpgpu_test_writel(s.dev, s.vram,
                             KERNEL_CODE +
                             ORDER_VALUE_INSN * sizeof(code[0]),
                             code[ORDER_VALUE_INSN] | (value << 20));
            gpgpu_test_memwrite(s.dev, s.vram, KERNEL_OUT, clear,
                               sizeof(clear));
            aql_submit(&s, 2, task, task, KERNEL_DESC,
                       GPA(QUEUE_ARGS(task)), GPA(QUEUE_SIGNAL(task)),
                       GPA(QUEUE_MAILBOX(task)), KERNEL_OUT, ORDER_LANES);
            wait_host32(&s, QUEUE_RPTR(2), task + 1);
            assert_retired(&s, 2, task, task);
            g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram, KERNEL_OUT),
                             ==, value + 2);
            g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram,
                                             KERNEL_OUT + sizeof(value)),
                             ==, value);
            g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram,
                                             KERNEL_OUT + 3 * sizeof(value)),
                             ==, value + 2);
        }

        /* Lane0 先写，Lane1 随后报告非法指令。 */
        gpgpu_test_writel(s.dev, s.vram,
                         KERNEL_CODE + ORDER_VALUE_INSN * sizeof(code[0]),
                         code[ORDER_VALUE_INSN] | (values[0] << 20));
        gpgpu_test_writel(s.dev, s.vram,
                         KERNEL_CODE + ORDER_LOAD_INSN * sizeof(code[0]), 0);
        gpgpu_test_memwrite(s.dev, s.vram, KERNEL_OUT, clear, sizeof(clear));
        aql_submit(&s, 2, fault_task, fault_task, KERNEL_DESC,
                   GPA(QUEUE_ARGS(fault_task)), GPA(QUEUE_SIGNAL(fault_task)),
                   GPA(QUEUE_MAILBOX(fault_task)), KERNEL_OUT, ORDER_LANES);
        wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_EXECUTOR);
        g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram, KERNEL_OUT),
                         ==, values[0]);
        g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram,
                                         KERNEL_OUT + sizeof(clear[0])), ==, 0);
        g_assert_cmpuint(gpgpu_test_readl(s.dev, s.vram,
                                         KERNEL_OUT + 3 * sizeof(clear[0])),
                         ==, 0);
        g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(fault_task)), ==, 1);
        g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, fault_task);
        g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES,
                                     GPA(QUEUE_MQD(2))),
                         ==, GPGPU_MANAGEMENT_OK);
        cp_stop(&s);
    }
}

/*
 * test_tensor_k_tail() - W8 的 K 分块与行列尾部
 *
 * M、N、K 均比一条指令的块宽多 1，覆盖完整矩阵。
 * 首次只执行 K=0 的块，输出不加偏置。
 * 第二次从 TILE_K 继续，读取部分和后在末块加偏置。
 * 输入为 1，权重按列交替为 -2/3，
 * scale=0.5、bias=0.25，期望值都能精确表示为 FP32。
 * 分别关闭/开启 native 优化，核对同一合同。
 */
static void test_tensor_k_tail(void)
{
    enum {
        /* 测试行数，覆盖 M 尾块。 */
        TENSOR_ROWS = GPGPU_TENSOR_TILE_M + 1,
        /* 测试列数，覆盖 N 尾块。 */
        TENSOR_COLS = GPGPU_TENSOR_TILE_N + 1,
        /* 测试归约宽度，覆盖 K 尾块。 */
        TENSOR_K = GPGPU_TENSOR_TILE_K + 1,
        /* 一维工作项数，从行列数及共享块宽推导。 */
        TENSOR_BLOCKS = DIV_ROUND_UP(TENSOR_ROWS, GPGPU_TENSOR_TILE_M) *
                        DIV_ROUND_UP(TENSOR_COLS, GPGPU_TENSOR_TILE_N),
        /* 下面代码数组中设置 K 起点的 ADDI 下标。 */
        TENSOR_K_INSN = 2,
    };
    const uint64_t x = KERNEL_OUT;
    const uint64_t weights = x + TENSOR_ROWS * TENSOR_K * sizeof(uint32_t);
    const uint64_t scales = QEMU_ALIGN_UP(weights + TENSOR_COLS * TENSOR_K,
                                         sizeof(uint32_t));
    const uint64_t bias = scales + TENSOR_COLS * sizeof(uint32_t);
    const uint64_t output = bias + TENSOR_COLS * sizeof(uint32_t);
    const uint64_t command = QEMU_ALIGN_UP(output + TENSOR_ROWS * TENSOR_COLS *
                                         sizeof(uint32_t),
                                         GPGPU_TENSOR_SLOT_BYTES);
    const uint32_t code[] = {
        0x00053283, /* ld t0,0(a0)：通用命令 GPU 地址。 */
        0xcc0023f3, /* csrr t2,THREAD_ID_X：线性块号。 */
        0x00000313, /* addi t1,zero,k_base：发布前同步替换。 */
        (6U << 27) | (GPGPU_TENSOR_LINEAR_W8 << 25) | (7U << 20) |
        (5U << 15) | (GPGPU_TENSOR_KIND_MATRIX << 12) |
        GPGPU_TENSOR_OPCODE,
        0x00100073, /* ebreak：本 Lane 已完成这一段 K。 */
    };
    uint8_t prefix[GPGPU_TENSOR_PARAM_BYTES] = { 0 };
    uint8_t input[TENSOR_ROWS * TENSOR_K * sizeof(uint32_t)];
    int8_t q[TENSOR_COLS * TENSOR_K];

    stq_le_p(prefix + GPGPU_TENSOR_IN0, x);
    stq_le_p(prefix + GPGPU_TENSOR_IN1, weights);
    stq_le_p(prefix + GPGPU_TENSOR_IN2, scales);
    stq_le_p(prefix + GPGPU_TENSOR_IN3, bias);
    stq_le_p(prefix + GPGPU_TENSOR_OUT, output);
    stq_le_p(prefix + GPGPU_TENSOR_ELEMENTS, TENSOR_ROWS * TENSOR_COLS);
    stq_le_p(prefix + GPGPU_TENSOR_DIMS, TENSOR_ROWS);
    stq_le_p(prefix + GPGPU_TENSOR_DIMS + GPGPU_TENSOR_SLOT_BYTES,
             TENSOR_COLS);
    stq_le_p(prefix + GPGPU_TENSOR_DIMS + 2 * GPGPU_TENSOR_SLOT_BYTES,
             TENSOR_K);
    for (unsigned int i = 0; i < TENSOR_ROWS * TENSOR_K; i++)
        stl_le_p(input + i * sizeof(uint32_t), 0x3f800000);

    for (unsigned int n = 0; n < TENSOR_COLS; n++)
        memset(q + n * TENSOR_K, n & 1 ? 3 : (uint8_t)-2, TENSOR_K);

    for (unsigned int mode = 0; mode < 2; mode++) {
        CPTest s = { .tensor_native = mode != 0 };

        cp_boot(&s, false, 4096, true);
        queue_mqd(&s, 1);
        queue_map(&s, 1);
        queue_mqd(&s, 2);
        queue_map(&s, 2);
        kernel_load(&s, KERNEL_CODE);
        gpgpu_test_writel(s.dev, s.vram,
                         KERNEL_DESC +
                         offsetof(GpgpuKernelDescriptor, code_size),
                         sizeof(code));
        for (unsigned int i = 0; i < ARRAY_SIZE(code); i++)
            gpgpu_test_writel(s.dev, s.vram,
                             KERNEL_CODE + i * sizeof(code[0]), code[i]);
        gpgpu_test_memwrite(s.dev, s.vram, command, prefix, sizeof(prefix));
        gpgpu_test_memwrite(s.dev, s.vram, x, input, sizeof(input));
        gpgpu_test_memwrite(s.dev, s.vram, weights, q, sizeof(q));
        for (unsigned int n = 0; n < TENSOR_COLS; n++) {
            gpgpu_test_writel(s.dev, s.vram, scales + n * sizeof(uint32_t),
                             0x3f000000);
            gpgpu_test_writel(s.dev, s.vram, bias + n * sizeof(uint32_t),
                             0x3e800000);
        }
        for (unsigned int i = 0; i < TENSOR_ROWS * TENSOR_COLS; i++)
            gpgpu_test_writel(s.dev, s.vram, output + i * sizeof(uint32_t),
                             0x7fc00000);

        for (unsigned int task = 0; task < 2; task++) {
            unsigned int k_base = task ? GPGPU_TENSOR_TILE_K : 0;
            unsigned int terms = task ? TENSOR_K : GPGPU_TENSOR_TILE_K;

            /* 收尾后同址换代码，再发布新包。 */
            gpgpu_test_writel(s.dev, s.vram,
                             KERNEL_CODE + TENSOR_K_INSN * sizeof(code[0]),
                             code[TENSOR_K_INSN] | (k_base << 20));
            aql_submit(&s, 2, task, task, KERNEL_DESC,
                       GPA(QUEUE_ARGS(task)), GPA(QUEUE_SIGNAL(task)),
                       GPA(QUEUE_MAILBOX(task)), command, TENSOR_BLOCKS);
            wait_host32(&s, QUEUE_RPTR(2), task + 1);
            assert_retired(&s, 2, task, task);
            for (unsigned int i = 0; i < TENSOR_ROWS * TENSOR_COLS; i++) {
                union {
                    float value;
                    uint32_t bits;
                } expected = {
                    .value = terms *
                             ((i % TENSOR_COLS) & 1 ? 1.5f : -1.0f) +
                             (task ? 0.25f : 0.0f),
                };

                g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram,
                                                output + i *
                                                sizeof(uint32_t)),
                                ==, expected.bits);
            }
        }
        cp_stop(&s);
    }
}

/*
 * test_fault_scope() - 用户队列错误只影响本队列
 *
 * 准备：映射 HIQ 与槽 2、3，在槽 2 放一个工作组尺寸为 0 的非法包。
 *
 * 期望：非法包只停止所属队列，不写成功 Signal、不推进 R；另一条用户
 * 队列继续执行；故障队列卸载后保存首次错误，带错误的 MQD 不能直接重装。
 */
static void test_fault_scope(void)
{
	CPTest s = { 0 };
	uint64_t at = QUEUE_RING(2);

    cp_boot(&s, false, 4096, false);
	queue_mqd(&s, 1);
	queue_map(&s, 1);
	queue_mqd(&s, 2);
	queue_map(&s, 2);
	queue_mqd(&s, 3);
	queue_map(&s, 3);
	kernel_load(&s, KERNEL_CODE);

	/*
	 * 1. 槽 2 发布 task 0 后把 workgroup_size_x 改成 0。CP 检查尺寸失败，
	 *    记录 PACKET 错误并清除 ACTIVE；R 仍为 0，Signal 保持 1。
	 */
	aql_submit_phys(&s, 2, 0, 0);
	qtest_writew(s.host, at + offsetof(GpgpuAqlPacket, workgroup_size_x), 0);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_PACKET);
	g_assert_cmpuint(hqd_read(&s, 2, GPGPU_HQD_ACTIVE), ==, 0);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);

	/* 2. 槽 3 的 task 1 照常完成，说明槽 2 的错误没有停止整个 CP。 */
	aql_submit_phys(&s, 3, 0, 1);
	wait_host32(&s, QUEUE_RPTR(3), 1);
	assert_retired(&s, 3, 0, 1);

	/*
	 * 3. IH 中依次是槽 2 的 QUEUE_ERROR 与槽 3 的 SIGNAL_NOTIFY；
	 *    错误记录的数据字段是错误号 PACKET。
	 */
	g_assert_cmpuint(rd(&s, GPGPU_REG_IH_WPTR), ==, 2);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, type)), ==,
			 GPGPU_IH_QUEUE_ERROR);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + offsetof(GpgpuIhRecord, data)), ==,
			 GPGPU_QUEUE_ERROR_PACKET);

	/*
	 * 4. 故障队列不要求 R == W，可以直接卸载；CP 把首次错误写入
	 *    saved_error。用这份 MQD 重新映射时，saved_error 非零，CP 返回
	 *    INVALID，必须由驱动清理后重建队列。
	 */
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_MQD(2) + offsetof(GpgpuMqd, saved_error)), ==,
			 GPGPU_QUEUE_ERROR_PACKET);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_INVALID);
	cp_stop(&s);
}

/*
 * GPUVM 测试的 GPUVA 布局与 VRAM 页表根。每个 GPUVA 映射一页：Ring、
 * 进度、参数和 Signal 映射到 Host RAM 中槽 2 或 task 0 使用的对象，测试
 * 随时改写它们，叶子项带 UNCACHED；描述对象、代码和数据映射到 VRAM，
 * VM_DCACHE 映射 Host 页 DCACHE_ARGS，这些页都是 WB。页表从 VM_ROOT 开始
 * 连续分配。
 *
 * 所有 GPUVA 都加上 VM_BASE：它高于 4 GiB，L0 索引为 0xff、L1 索引为
 * 0x1fc，地址高 32 位非零，翻译要走完整的 48 位路径。VM_BASE 按 64 页
 * 对齐，各页的 TLB 组号与不加基址时相同。
 */
#define VM_ROOT 0x20000U
#define VM_BASE 0x00007fff00000000ULL
#define VM_RING (VM_BASE + 0x00100000U)
#define VM_PROGRESS (VM_BASE + 0x00101000U)
#define VM_ARGS (VM_BASE + 0x00102000U)
#define VM_SIGNAL (VM_BASE + 0x00103000U)
/* 通知页由用户态创建，GPUVA 不固定；测试把它放在 Signal 之后。 */
#define VM_NOTIFY (VM_BASE + 0x00104000U)
/* dcache 用例的可缓存 Host 页。 */
#define VM_DCACHE (VM_BASE + 0x00105000U)
#define VM_DESC (VM_BASE + 0x00400000U)
#define VM_CODE (VM_BASE + 0x00401000U)
#define VM_DATA (VM_BASE + 0x00402000U)
/* test_gpuvm_system() 把一张 L3 叶表搬到这个 Host 页，经 SYSTEM 中间项访问。 */
#define VM_HOST_TABLE (QUEUE_HOST + 0xd000)
/*
 * dcache 用例的可缓存 Host 页，经 VM_DCACHE 按 WB 访问：参数块与数据各占
 * 整数条缓存行。DCACHE_WORD 是数据行中间的一个字，部分写只改它。
 */
#define DCACHE_ARGS (VM_HOST_TABLE + GPGPU_VM_PAGE_SIZE)
#define DCACHE_DATA (DCACHE_ARGS + QEMU_ALIGN_UP(ARGS_BYTES, GPGPU_DCACHE_LINE_SIZE))
#define DCACHE_WORD (DCACHE_DATA + GPGPU_DCACHE_LINE_SIZE / 2)
/* Host 页 DCACHE_ARGS 内的地址换成 VM_DCACHE 下的 GPUVA。 */
#define DCACHE_VA(host) (VM_DCACHE + ((host) - DCACHE_ARGS))
/* DCACHE_DATA 整行的初值。 */
#define DCACHE_FILL 0x5a
/*
 * test_gpuvm_tlb() 的数据页：比一组的路数多一页，第 k 页的 GPUVA 比 VM_DATA
 * 高 k × 64 页，VPN 对组数取模相同，全部落在 VM_DATA 所在的组；第 k 页
 * 指向 VRAM 中 KERNEL_OUT 之后的第 k 页。
 */
#define VM_TLB_PAGES (GPGPU_VM_TLB_WAYS + 1)
#define VM_TLB_DATA(k) (VM_DATA + (k) * GPGPU_VM_TLB_SETS * GPGPU_VM_PAGE_SIZE)
#define VM_TLB_OUT(k) (KERNEL_OUT + (k) * GPGPU_VM_PAGE_SIZE)

/*
 * 一页用户映射：va 是 GPUVA，target 是叶子项的地址域，flags 是 SYSTEM、
 * UNCACHED 与 R/W/X 位。VRAM 页写 VRAM 偏移；Host 页写 Host IOVA 并带
 * SYSTEM，设备按 GPA() 访问。
 */
typedef struct VmMapping {
	uint64_t va;
	uint64_t target;
	uint64_t flags;
} VmMapping;

/*
 * vm_entry() - 返回 GPUVA 在第 level 级表项所在的 VRAM 偏移，按需创建中间表
 * @level: 0 为根，GPGPU_VM_LEVELS - 1 为叶子。
 * @next_table: 下一个可用表页的 VRAM 偏移，创建表页后向后推进一页。
 *
 * 四级 4 KiB 页表，每级 512 项，L0～L3 依次按 VA[47:39]、[38:30]、
 * [29:21]、[20:12] 索引。中间项只保存下一级表页地址和 VALID 位。
 *
 * 循环走过 level 之前的各级：某项无效时取 next_table 作为新表页并写回
 * 该项。新表页依赖 vm_prepare() 预先清零，所以其中各项都是无效项。
 */
static uint32_t vm_entry(CPTest *s, uint64_t va, unsigned int level, uint32_t *next_table)
{
	uint32_t table = VM_ROOT;

	for (unsigned int i = 0; i < level; i++) {
		uint32_t index = (va >> GPGPU_VM_LEVEL_SHIFT(i)) & GPGPU_VM_INDEX_MASK;
		uint32_t entry_at = table + index * 8;
        uint64_t entry = gpgpu_test_readq(s->dev, s->vram, entry_at);

		if (!(entry & GPGPU_VM_VALID)) {
			entry = *next_table | GPGPU_VM_VALID;
			*next_table += GPGPU_VM_PAGE_SIZE;
            gpgpu_test_writeq(s->dev, s->vram, entry_at, entry);
		}
		table = entry & GPGPU_VM_ADDR_MASK;
	}
	return table + ((va >> GPGPU_VM_LEVEL_SHIFT(level)) & GPGPU_VM_INDEX_MASK) * 8;
}

/* 返回 GPUVA 叶子项所在的 VRAM 偏移，按需创建中间表。 */
static uint32_t vm_leaf(CPTest *s, uint64_t va, uint32_t *next_table)
{
	return vm_entry(s, va, GPGPU_VM_LEVELS - 1, next_table);
}

/* 同步失效 VMID 1 的全部 TLB 项并要求确认；调用时槽 2 不能激活。 */
static void vm_invalidate(CPTest *s)
{
	wr(s, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(1));
	g_assert_cmphex(rd(s, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_VMID(1));
}

/* 经 HIQ 卸载用户队列 slot 并要求成功；故障队列同样可以卸载。 */
static void queue_unmap(CPTest *s, unsigned int slot)
{
	g_assert_cmpuint(queue_manage(s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(slot))), ==,
			 GPGPU_MANAGEMENT_OK);
}

/*
 * vm_prepare() - 建立 VMID 1 的地址空间并映射用户队列（槽 2）
 * @code_flags: 代码页权限，用于构造取指权限故障。
 *
 * 1. 清空表页区并写入各页映射；2. 写根、同步失效并启用上下文；
 * 3. 以 GPUVA 准备 MQD，经 HIQ 映射。
 *
 * 必须先启用上下文再映射用户队列：队列激活后，VMID 1 的根和使能都对
 * Host 只读，失效也会被拒绝。
 */
static void vm_prepare(CPTest *s, uint64_t code_flags)
{
	const uint64_t rw = GPGPU_VM_READ | GPGPU_VM_WRITE;
	const uint64_t host = GPGPU_VM_SYSTEM;
	const uint64_t uc = GPGPU_VM_SYSTEM | GPGPU_VM_UNCACHED;
	const VmMapping maps[] = {
		{ VM_RING, QUEUE_RING(2), uc | rw },
		{ VM_PROGRESS, QUEUE_WPTR(0), uc | rw },
		{ VM_ARGS, QUEUE_ARGS(0), uc | GPGPU_VM_READ },
		{ VM_SIGNAL, QUEUE_SIGNAL(0), uc | rw },
		{ VM_NOTIFY, QUEUE_MAILBOX(0), uc | rw },
		{ VM_DCACHE, DCACHE_ARGS, host | rw },
		{ VM_DESC, KERNEL_DESC, rw },
		{ VM_CODE, KERNEL_CODE, code_flags },
		{ VM_DATA, KERNEL_OUT, rw },
		{ VM_TLB_DATA(1), VM_TLB_OUT(1), rw },
		{ VM_TLB_DATA(2), VM_TLB_OUT(2), rw },
		{ VM_TLB_DATA(3), VM_TLB_OUT(3), rw },
		{ VM_TLB_DATA(4), VM_TLB_OUT(4), rw },
	};
	uint32_t next_table = VM_ROOT + GPGPU_VM_PAGE_SIZE;

    cp_boot(s, true, 4096, false);
	queue_mqd(s, 1);
	queue_map(s, 1);
	kernel_load(s, VM_CODE);

	/*
	 * 1. 从 VM_ROOT 起清零 8 页，作为根表和按需分配的中间表、
	 *    叶子表；再为 maps 中的每页写叶子项：地址域、SYSTEM、
	 *    UNCACHED 与权限位，以及 VALID。
	 */
	for (uint32_t off = 0; off < 8 * GPGPU_VM_PAGE_SIZE; off += 8)
                gpgpu_test_writeq(s->dev, s->vram, VM_ROOT + off, 0);
	for (unsigned int i = 0; i < ARRAY_SIZE(maps); i++)
                gpgpu_test_writeq(s->dev, s->vram,
                                  vm_leaf(s, maps[i].va, &next_table),
                                  maps[i].target | maps[i].flags |
                                  GPGPU_VM_VALID);

	/*
	 * 2. 写 VMID 1 的页表根，同步失效该 VMID 的 TLB 并确认 ACK，
	 *    再启用上下文。设备只在根页合法时接受启用，没有全局错误
	 *    说明配置成功。
	 */
	wr(s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ROOT_LO, VM_ROOT);
	vm_invalidate(s);
	wr(s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE, 1);
	g_assert_cmphex(rd(s, GPGPU_REG_ERROR_STATUS), ==, 0);

	/*
	 * 3. 槽 2 的 MQD 中 Ring 与进度地址改用 GPUVA。VM_PROGRESS 映射到进度区
	 *    起点 QUEUE_WPTR(0)，偏移 32、40 正是 QUEUE_WPTR(2)、QUEUE_RPTR(2)，
	 *    测试仍可按 Host 地址读写这条队列的 W、R。
	 */
	queue_mqd_at(s, 2, VM_RING, VM_PROGRESS + 40, VM_PROGRESS + 32);
	queue_map(s, 2);
}

/*
 * 在槽 2 提交第 number 个包，执行 task，Kernel 把结果写到 GPUVA out。
 * 包中所有地址都是 VMID 1 下的 GPUVA；task 的 Signal、通知槽和参数块
 * 与 QUEUE_SIGNAL()、QUEUE_MAILBOX()、QUEUE_ARGS() 同页同偏移。
 */
static void vm_submit(CPTest *s, uint32_t number, unsigned int task, uint64_t out)
{
    aql_submit(s, 2, number, task, VM_DESC, VM_ARGS + ARGS_BYTES * task,
               VM_SIGNAL + 32 * task, VM_NOTIFY + 8 * task, out, 1);
}

/*
 * test_gpuvm_translation() - GPUVM 开启时的完整派发与驻留队列的失效
 *
 * 准备：vm_prepare() 建好 VMID 1，代码页可读可执行，槽 2 使用 GPUVA。
 *
 * 期望：CP 读包、描述对象和 Signal，执行器取指、读参数、写数据都经
 * GPUVA 翻译；Kernel 写入 VM_DATA，落到 VRAM KERNEL_OUT。槽 2 仍激活时
 * 失效照常完成，但上下文的使能不能修改。
 */
static void test_gpuvm_translation(void)
{
	CPTest s = { 0 };

	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	vm_submit(&s, 0, 0, VM_DATA);
	wait_host32(&s, QUEUE_RPTR(2), 1);
    g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram, KERNEL_OUT),
                    ==, 0xaabb0000);
	assert_retired(&s, 2, 0, 0);

	/*
	 * 槽 2 仍激活时，VMID 1 的失效照常完成，ACK 为 VMID 1 的位；
	 * 停用上下文仍被拒绝，使能保持 1。
	 */
	vm_invalidate(&s);
	wr(&s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE, 0);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE), ==, 1);
	queue_unmap(&s, 2);
	cp_stop(&s);
}

/*
 * test_gpuvm_exec_fault() - 代码页缺少 EXEC 时的取指故障
 *
 * 准备：vm_prepare() 只给代码页 READ 权限，然后提交 task 0。
 *
 * 期望：执行器取指失败，槽 2 记录 EXECUTOR 错误；槽 2 的 VM 故障组记录
 * PERMISSION、VMID 1、故障地址 VM_CODE、访问类型 EXEC 和取指阶段。
 * 任务没有成功，Signal 保持 1，R 不推进；故障队列仍可卸载。
 */
static void test_gpuvm_exec_fault(void)
{
	const uint64_t fault = GPGPU_REG_VM_FAULT(2);
	CPTest s = { 0 };

	vm_prepare(&s, GPGPU_VM_READ);
	vm_submit(&s, 0, 0, VM_DATA);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_EXECUTOR);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_REASON), ==, GPGPU_VM_FAULT_PERMISSION);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_VMID), ==, 1);
	g_assert_cmphex(rd64(&s, fault + GPGPU_VM_FAULT_ADDR_LO), ==, VM_CODE);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_ACCESS), ==, GPGPU_VM_EXEC);
	g_assert_cmphex(rd(&s, fault + GPGPU_VM_FAULT_PHASE), ==, GPGPU_VM_PHASE_FETCH);
	g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);
	g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	cp_stop(&s);
}

/*
 * test_gpuvm_system() - 页表项 SYSTEM 位的地址解释
 *
 * 准备：vm_prepare() 建好 VMID 1 后卸载槽 2，把 Ring、进度、参数和 Signal
 * 所在的 L3 叶表从 VRAM 复制到 Host 页 VM_HOST_TABLE，指向它的 L2 项改为
 * “IOVA | SYSTEM | VALID”。之后每次改表都先卸载槽 2，失效后再映射。
 *
 * 期望：
 * 1. 经 Host 中间表也能完成派发，结果写到 VRAM KERNEL_OUT；
 * 2. 参数页叶子写成 v3 风格的 GPU 物理地址 GPA()、不带 SYSTEM：地址落在
 *    Outbound 而不在 VRAM，执行器读参数时记 TABLE 故障。空洞地址本来就
 *    不可路由，测不出 SYSTEM 与地址域的匹配检查，所以不用它；
 * 3. L2 中间项带上 UNCACHED：缓存属性只能出现在叶子项，映射槽 2 时
 *    读 W 就记 TABLE 故障，管理命令返回 MQD_FAULT。
 */
static void test_gpuvm_system(void)
{
	const uint64_t fault = GPGPU_REG_VM_FAULT(2);
	const uint64_t args_leaf = VM_HOST_TABLE +
		((VM_ARGS >> GPGPU_VM_PAGE_SHIFT) & GPGPU_VM_INDEX_MASK) * 8;
	CPTest s = { 0 };
	uint8_t table[GPGPU_VM_PAGE_SIZE];
	uint32_t no_alloc = 0;
	uint32_t dir_entry;
	uint64_t leaf_table;

	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	queue_unmap(&s, 2);

	/* 1. 找到指向 VM_RING 所在叶表的 L2 项；路径已建立，不分配表页。 */
	dir_entry = vm_entry(&s, VM_RING, GPGPU_VM_LEVELS - 2, &no_alloc);
    leaf_table = gpgpu_test_readq(s.dev, s.vram, dir_entry) &
                 GPGPU_VM_ADDR_MASK;
    gpgpu_test_memread(s.dev, s.vram, leaf_table, table, sizeof(table));
	qtest_memwrite(s.host, VM_HOST_TABLE, table, sizeof(table));
    gpgpu_test_writeq(s.dev, s.vram, dir_entry,
                      VM_HOST_TABLE | GPGPU_VM_SYSTEM | GPGPU_VM_VALID);
	vm_invalidate(&s);
	queue_map(&s, 2);
	vm_submit(&s, 0, 0, VM_DATA);
	wait_host32(&s, QUEUE_RPTR(2), 1);
    g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram, KERNEL_OUT),
                    ==, 0xaabb0000);

	/* 2. SYSTEM=0 的叶子按 VRAM 偏移解释，0xc100_b000 超出 VRAM。 */
	queue_unmap(&s, 2);
	qtest_writeq(s.host, args_leaf, GPA(QUEUE_ARGS(0)) | GPGPU_VM_READ | GPGPU_VM_VALID);
	vm_invalidate(&s);
	queue_map(&s, 2);
	vm_submit(&s, 1, 1, VM_DATA);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_EXECUTOR);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_REASON), ==, GPGPU_VM_FAULT_TABLE);
	g_assert_cmphex(rd64(&s, fault + GPGPU_VM_FAULT_ADDR_LO), ==, VM_ARGS + ARGS_BYTES);

	/*
	 * 3. 故障队列卸载后清除故障记录，重写 MQD 去掉 saved_error，
	 *    W、R 从 0 开始。指向 Host 叶表的 L2 项带上 UNCACHED。
	 */
	queue_unmap(&s, 2);
	wr(&s, fault + GPGPU_VM_FAULT_CLEAR, 1);
	queue_mqd_at(&s, 2, VM_RING, VM_PROGRESS + 40, VM_PROGRESS + 32);
    gpgpu_test_writeq(s.dev, s.vram, dir_entry,
		       VM_HOST_TABLE | GPGPU_VM_SYSTEM | GPGPU_VM_UNCACHED | GPGPU_VM_VALID);
	vm_invalidate(&s);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_MQD_FAULT);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_REASON), ==, GPGPU_VM_FAULT_TABLE);
	cp_stop(&s);
}

/* 依次派发写 VM_TLB_DATA(0..4)；Ring 只有 4 槽，每包完成后再提交下一个。 */
static void vm_tlb_round(CPTest *s, uint32_t *number)
{
	for (unsigned int k = 0; k < VM_TLB_PAGES; k++, (*number)++) {
		vm_submit(s, *number, k, VM_TLB_DATA(k));
		wait_host32(s, QUEUE_RPTR(2), *number + 1);
	}
}

/*
 * test_gpuvm_tlb() - 组相联 TLB 的替换、范围失效与整 VMID 失效
 *
 * 准备：vm_prepare() 映射的五个数据页 VM_TLB_DATA(0..4) 都落在第 2 组，
 * 各自指向不同 VRAM 页。参数页 VM_ARGS（VPN 低位 0x102）也在第 2 组；
 * 每次派发都先读参数页再写数据页，参数页总比数据页更近被使用，
 * 不会被替换。
 *
 * 期望：
 * b. 依次写五个数据页，第 2 组装不下全部翻译，EVICTIONS 至少加 1
 *    （参数页占一路，实际加 2）；
 * c. 队列保持驻留，把第 4 页改指新 VRAM 页，只失效这一页：再次写它时
 *    结果落到新页，说明旧翻译已清除；MISSES 恰好加 1，说明同组其他翻译
 *    仍在 TLB 中；
 * d. 整 VMID 失效后重复 b 的派发序列，MISSES 增量与 b 相同。
 * b、d 都从刚映射完队列时开始计量，两次起点的 TLB 内容一致。CP 轮询会
 * 持续命中，HITS 不作断言。
 */
static void test_gpuvm_tlb(void)
{
	const uint32_t last = VM_TLB_PAGES - 1;
	const uint64_t vpn = VM_TLB_DATA(last) / GPGPU_VM_PAGE_SIZE;
	CPTest s = { 0 };
	uint32_t number = 0;
	uint32_t no_alloc = 0;
	uint32_t misses, evictions, round_misses;

	/* a. 五个数据页已由 vm_prepare() 映射。 */
	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);

	/* b. 依次写五页，核对结果与替换次数。 */
	misses = rd(&s, GPGPU_REG_VM_TLB_MISSES);
	evictions = rd(&s, GPGPU_REG_VM_TLB_EVICTIONS);
	vm_tlb_round(&s, &number);
	round_misses = rd(&s, GPGPU_REG_VM_TLB_MISSES) - misses;
	for (unsigned int k = 0; k < VM_TLB_PAGES; k++)
                g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram,
                                                VM_TLB_OUT(k)),
                                ==, 0xaabb0000 + k);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_TLB_EVICTIONS) - evictions, >=, 1);

	/*
	 * c. 队列仍驻留，第 4 页改指 VM_TLB_OUT(5)，按 VPN 只失效这一页。
	 *    页已映射，vm_leaf() 只查找叶子项，不会分配新表页。
	 */
    gpgpu_test_writeq(s.dev, s.vram, vm_leaf(&s, VM_TLB_DATA(last), &no_alloc),
		       VM_TLB_OUT(VM_TLB_PAGES) | GPGPU_VM_READ | GPGPU_VM_WRITE | GPGPU_VM_VALID);
	wr64(&s, GPGPU_REG_VM_INV_VPN_START_LO, vpn);
	wr64(&s, GPGPU_REG_VM_INV_VPN_END_LO, vpn);
	wr(&s, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(1) | GPGPU_VM_INV_RANGE);
	g_assert_cmphex(rd(&s, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_VMID(1));
	misses = rd(&s, GPGPU_REG_VM_TLB_MISSES);
	vm_submit(&s, number, VM_TLB_PAGES, VM_TLB_DATA(last));
	wait_host32(&s, QUEUE_RPTR(2), ++number);
    g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram,
                                    VM_TLB_OUT(VM_TLB_PAGES)), ==,
			0xaabb0000 + VM_TLB_PAGES);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_TLB_MISSES) - misses, ==, 1);

	/* d. 整 VMID 失效后从同样的起点重复 b 的序列。 */
	queue_unmap(&s, 2);
	vm_invalidate(&s);
	queue_map(&s, 2);
	misses = rd(&s, GPGPU_REG_VM_TLB_MISSES);
	vm_tlb_round(&s, &number);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_TLB_MISSES) - misses, ==, round_misses);
	cp_stop(&s);
}

/*
 * test_gpuvm_cached_permission() - dcache 命中不绕过页权限
 *
 * 准备：vm_prepare() 建好 VMID 1，task 0 写 WB 页 VM_DATA；收尾回写后
 * KERNEL_OUT 所在行仍是干净的缓存行。随后卸载槽 2，把 VM_DATA 的叶子
 * 改为只读，失效 TLB 后重新映射。期间不经 BAR 读这一行，BAR 读会让它
 * 失效。
 *
 * 期望：task 1 写同一行时先检查页权限，记 PERMISSION 故障与 EXECUTOR
 * 错误；KERNEL_OUT 保持 task 0 的结果。
 */
static void test_gpuvm_cached_permission(void)
{
	const uint64_t fault = GPGPU_REG_VM_FAULT(2);
	CPTest s = { 0 };
	uint32_t no_alloc = 0;

	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	vm_submit(&s, 0, 0, VM_DATA);
	wait_host32(&s, QUEUE_RPTR(2), 1);

	queue_unmap(&s, 2);
    gpgpu_test_writeq(s.dev, s.vram, vm_leaf(&s, VM_DATA, &no_alloc),
		       KERNEL_OUT | GPGPU_VM_READ | GPGPU_VM_VALID);
	vm_invalidate(&s);
	queue_map(&s, 2);
	vm_submit(&s, 1, 1, VM_DATA);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_EXECUTOR);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_REASON), ==, GPGPU_VM_FAULT_PERMISSION);
	g_assert_cmpuint(rd(&s, fault + GPGPU_VM_FAULT_ACCESS), ==, GPGPU_VM_WRITE);
    g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram, KERNEL_OUT),
                    ==, 0xaabb0000);
	cp_stop(&s);
}

/* [addr, end) 覆盖的缓存行数；期望的未命中与回写次数由它推导。 */
static uint32_t dcache_lines(uint64_t addr, uint64_t end)
{
	return (end - 1) / GPGPU_DCACHE_LINE_SIZE - addr / GPGPU_DCACHE_LINE_SIZE + 1;
}

/*
 * dcache 用例的共同准备：vm_prepare() 建好 VMID 1 并映射槽 2，DCACHE_DATA
 * 整行填 DCACHE_FILL，expected 记下同样的内容。缓存行为只在 GPUVM 开启
 * 时出现：Ring、进度、Signal 和 task 参数块是 UNCACHED 页，描述对象、
 * VM_DATA 与 VM_DCACHE 是 WB 页。
 */
static void dcache_prepare(CPTest *s, uint8_t *expected)
{
	vm_prepare(s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	memset(expected, DCACHE_FILL, GPGPU_DCACHE_LINE_SIZE);
	qtest_memwrite(s->host, DCACHE_DATA, expected, GPGPU_DCACHE_LINE_SIZE);
}

/* 写 DCACHE_ARGS 中的两个参数：Kernel 把 value 写到 GPUVA target。 */
static void dcache_args(CPTest *s, uint64_t target, uint32_t value)
{
	qtest_writeq(s->host, DCACHE_ARGS, target);
	qtest_writeq(s->host, DCACHE_ARGS + 8, value);
}

/*
 * 以 WB 页上的 DCACHE_ARGS 为参数块提交第 number 个包，并等槽 2 收尾。
 * aql_submit() 照常填写 task 参数块，Kernel 不读它。
 */
static void dcache_run(CPTest *s, uint32_t number)
{
    aql_submit(s, 2, number, number, VM_DESC, VM_DCACHE,
               VM_SIGNAL + 32 * number, VM_NOTIFY + 8 * number, 0, 1);
	wait_host32(s, QUEUE_RPTR(2), number + 1);
}

/* 读回 DCACHE_DATA 整行并与 expected 比较。 */
static void dcache_check(CPTest *s, const uint8_t *expected)
{
	uint8_t line[GPGPU_DCACHE_LINE_SIZE];

	qtest_memread(s->host, DCACHE_DATA, line, sizeof(line));
	g_assert_cmpmem(line, sizeof(line), expected, GPGPU_DCACHE_LINE_SIZE);
}

/*
 * test_dcache_shared_line() - CP 与执行器共用同一物理行
 *
 * Kernel 经 WB 页 VM_DESC 把值写到紧跟描述对象之后的 shared。CP 读描述
 * 对象时回填它所在的行，执行器的 store 直接命中，所以未命中次数等于
 * [描述对象, shared] 覆盖的行数；若两方各有一份缓存，执行器还会多一次
 * 未命中。其余对象都在 UNCACHED 页，不计入。收尾只回写 shared 所在的
 * 脏行，BAR 读到新值，描述对象不变。
 */
static void test_dcache_shared_line(void)
{
	const uint64_t shared = KERNEL_DESC + sizeof(GpgpuKernelDescriptor);
	CPTest s = { 0 };
	uint8_t expected[GPGPU_DCACHE_LINE_SIZE];
	uint32_t misses, writebacks;

	dcache_prepare(&s, expected);
	misses = rd(&s, GPGPU_REG_DCACHE_MISSES);
	writebacks = rd(&s, GPGPU_REG_DCACHE_WRITEBACKS);
	vm_submit(&s, 0, 0, VM_DESC + (shared - KERNEL_DESC));
	wait_host32(&s, QUEUE_RPTR(2), 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_DCACHE_MISSES) - misses, ==,
			 dcache_lines(KERNEL_DESC, shared + sizeof(uint32_t)));
	g_assert_cmpuint(rd(&s, GPGPU_REG_DCACHE_WRITEBACKS) - writebacks, ==,
			 dcache_lines(shared, shared + sizeof(uint32_t)));
    g_assert_cmphex(gpgpu_test_readl(s.dev, s.vram, shared), ==, 0xaabb0000);
    g_assert_cmphex(gpgpu_test_readq(s.dev, s.vram, KERNEL_DESC), ==, VM_CODE);
	cp_stop(&s);
}

/*
 * test_dcache_partial_write() - Host 数据写分配后的部分写
 *
 * Kernel 经 WB 页只写 DCACHE_WORD。写分配先回填整行，收尾回写整行后，
 * Host 读到新值，同一行的其余字节保持初值。
 */
static void test_dcache_partial_write(void)
{
	CPTest s = { 0 };
	uint8_t expected[GPGPU_DCACHE_LINE_SIZE];

	dcache_prepare(&s, expected);
	vm_submit(&s, 0, 0, DCACHE_VA(DCACHE_WORD));
	wait_host32(&s, QUEUE_RPTR(2), 1);
	stl_le_p(expected + (DCACHE_WORD - DCACHE_DATA), 0xaabb0000);
	dcache_check(&s, expected);
	cp_stop(&s);
}

/*
 * test_dcache_invalidate() - CPU 改写可缓存参数后失效旧副本
 *
 * 第一次派发把参数块所在行留在缓存中，收尾回写后它是干净的。CPU 改写
 * 参数并失效这段范围，第二次派发读到新参数，把值写到 DCACHE_WORD；
 * 不失效时执行器仍读到旧参数，再次写 DCACHE_DATA。
 */
static void test_dcache_invalidate(void)
{
	CPTest s = { 0 };
	uint8_t expected[GPGPU_DCACHE_LINE_SIZE];

	dcache_prepare(&s, expected);
	dcache_args(&s, DCACHE_VA(DCACHE_DATA), 0xaabb0000);
	dcache_run(&s, 0);
	dcache_args(&s, DCACHE_VA(DCACHE_WORD), 0xaabb0001);
	g_assert_cmpuint(dcache_cmd(&s, GPGPU_DCACHE_INVALIDATE, GPA(DCACHE_ARGS), ARGS_BYTES), ==,
			 GPGPU_DCACHE_OK);
	dcache_run(&s, 1);
	stl_le_p(expected, 0xaabb0000);
	stl_le_p(expected + (DCACHE_WORD - DCACHE_DATA), 0xaabb0001);
	dcache_check(&s, expected);
	cp_stop(&s);
}

/*
 * test_dcache_bar_write() - BAR 写入使已缓存的描述对象行失效
 *
 * 第一次派发让 WB 页上的描述对象所在行进入缓存。BAR 把 ABI 版本改为 0
 * 后，CP 读到新内容，下一个包记 DESCRIPTOR 错误；BAR 写入不失效时 CP
 * 仍命中旧行，派发照常成功。
 */
static void test_dcache_bar_write(void)
{
	CPTest s = { 0 };
	uint8_t expected[GPGPU_DCACHE_LINE_SIZE];

	dcache_prepare(&s, expected);
	vm_submit(&s, 0, 0, VM_DATA);
	wait_host32(&s, QUEUE_RPTR(2), 1);
    gpgpu_test_writel(s.dev, s.vram,
		       KERNEL_DESC + offsetof(GpgpuKernelDescriptor, kernel_abi_version), 0);
	vm_submit(&s, 1, 1, VM_DATA);
	wait_queue_error(&s, 2, GPGPU_QUEUE_ERROR_DESCRIPTOR);
	cp_stop(&s);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	qtest_add_func("/gpgpu/cp/management", test_management);
	qtest_add_func("/gpgpu/cp/dispatch", test_dispatch);
    qtest_add_func("/gpgpu/cp/shared-fetch-lanes", test_shared_fetch_lanes);
    qtest_add_func("/gpgpu/cp/tensor-k-tail", test_tensor_k_tail);
	qtest_add_func("/gpgpu/cp/fault-scope", test_fault_scope);
	qtest_add_func("/gpgpu/cp/gpuvm-translation", test_gpuvm_translation);
	qtest_add_func("/gpgpu/cp/gpuvm-exec-fault", test_gpuvm_exec_fault);
	qtest_add_func("/gpgpu/cp/gpuvm-system", test_gpuvm_system);
	qtest_add_func("/gpgpu/cp/gpuvm-tlb", test_gpuvm_tlb);
	qtest_add_func("/gpgpu/cp/gpuvm-cached-permission", test_gpuvm_cached_permission);
	qtest_add_func("/gpgpu/cp/dcache-shared-line", test_dcache_shared_line);
	qtest_add_func("/gpgpu/cp/dcache-partial-write", test_dcache_partial_write);
	qtest_add_func("/gpgpu/cp/dcache-invalidate", test_dcache_invalidate);
	qtest_add_func("/gpgpu/cp/dcache-bar-write", test_dcache_bar_write);
	return g_test_run();
}
