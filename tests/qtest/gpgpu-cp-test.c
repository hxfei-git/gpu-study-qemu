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
 * 8 字节参数块。QUEUE_RESULT 是管理请求的 16 字节结果区，
 * 测试串行提交管理命令，所以全部请求复用这一份。
 */
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

	/*
	 * 1. 在临时目录中启动 gpgpu-cp 远端进程，再用 QMP 的 object-add 创建
	 *    vfio-user 服务端，把设备 gpu0 导出到 gpu.sock。设备的 stderr 写入
	 *    日志文件，gpuvm 开关经 -global 传给设备。
	 */
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
	s->vram = qpci_iomap(s->dev, 2, NULL);
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

/* BAR0 寄存器读写：设备只接受对齐的 32 位访问，64 位分两次写。 */
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

	/*
	 * 1. 复位后 CP_CTRL 应为“已连接 CP 且保持复位”。把固件镜像经 BAR2
	 *    写到 VRAM 末尾的固件区，复位向量指向它的入口。
	 */
	g_assert_cmphex(rd(s, GPGPU_REG_CP_CTRL), ==, GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD);
	qpci_memwrite(s->dev, s->vram, GPGPU_FIRMWARE_BASE, image, size);
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
	qpci_io_writeq(s->dev, s->doorbell, source * 8, wptr + 2);
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
 * 参数块为 { 目标地址, 值 }，Kernel 执行 *(u32 *)args[0] = args[1]：
 *   lw t0, 0(a0); lw t1, 4(a0); sw t1, 0(t0); ebreak
 *
 * a0 是 RV32 计算执行器传给线程的参数块地址。代码总放在 VRAM 的
 * KERNEL_CODE，描述对象放在 KERNEL_DESC，参数长度固定为 8 字节。
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
 * @number: 包编号；物理槽为 number 对 Ring 槽数取模。
 * @task: 测试任务编号，选择 Signal、通知槽和参数块，事件编号为 task + 1。
 * @out: Kernel 写入的目标地址，Kernel 会在这里写 0xaabb0000 + task。
 *
 * 1. 准备 Signal（值 1、事件编号、通知槽）与参数；
 * 2. 以 INVALID Header 写入包体；3. 最后写有效 Header，推进 W 并敲 Doorbell。
 * 各地址参数直接写入协议：GPUVM 开启时传 GPUVA，关闭时传 GPU 物理地址。
 *
 * 测试是单生产者，直接把 W 写成 number + 1，不经过原子预留。
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

	/*
	 * 1. Signal 置 1，表示任务尚未完成，并写入事件编号与通知槽地址；
	 *    通知槽清零，完成后 CP 会在其中写入事件编号。参数块依次是
	 *    输出地址和待写值，待写值带上 task，便于区分各任务的结果。
	 */
	qtest_memwrite(s->host, QUEUE_SIGNAL(task), &sig, sizeof(sig));
	qtest_writeq(s->host, QUEUE_MAILBOX(task), 0);
	qtest_writel(s->host, QUEUE_ARGS(task), out);
	qtest_writel(s->host, QUEUE_ARGS(task) + 4, 0xaabb0000 + task);

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
	qpci_io_writeq(s->dev, s->doorbell, slot * 8, number + 1);
}

/* GPUVM 关闭时的提交：所有地址都是 GPU 物理地址。 */
static void aql_submit_phys(CPTest *s, unsigned int slot, uint32_t number, unsigned int task)
{
	aql_submit(s, slot, number, task, KERNEL_DESC, GPA(QUEUE_ARGS(task)),
		   GPA(QUEUE_SIGNAL(task)), GPA(QUEUE_MAILBOX(task)), KERNEL_OUT + 4 * task);
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

	cp_boot(&s, false, 4096);
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
 * 期望：三个任务的输出都写到 VRAM；每个任务的 Signal 归零、通知槽为事件
 * 编号、Header 恢复 INVALID；IH 中有 3 条 SIGNAL_NOTIFY；卸载槽 2 后
 * saved_rptr 为 2，即这条队列消费的包数。Outbound 已打开时，Host 端落在
 * VRAM 与窗口之间空洞里的 DMA 仍然失败。
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

	/*
	 * 空洞不可路由：Host 端取 VRAM 末端之后的 0x0400_1000，即使窗口已
	 * 打开，START 时检查也失败，状态为 ERROR 并锁存 DMA_FAULT。
	 */
	wr(&s, GPGPU_REG_DMA_SRC_LO, GPGPU_TEST_VRAM_SIZE + 0x1000);
	wr(&s, GPGPU_REG_DMA_SRC_HI, 0);
	wr(&s, GPGPU_REG_DMA_DST_LO, 0x3000);
	wr(&s, GPGPU_REG_DMA_DST_HI, 0);
	wr(&s, GPGPU_REG_DMA_SIZE, 64);
	wr(&s, GPGPU_REG_DMA_CTRL, GPGPU_DMA_START);
	g_assert_cmphex(rd(&s, GPGPU_REG_DMA_STATUS), ==, GPGPU_DMA_ERROR);
	g_assert_cmphex(rd(&s, GPGPU_REG_ERROR_STATUS), ==, GPGPU_ERR_DMA_FAULT);
	cp_stop(&s);
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

	cp_boot(&s, false, 4096);
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
 * 进度、参数和 Signal 映射到 Host RAM 中槽 2 或 task 0 使用的对象；
 * 描述对象、代码和数据映射到 VRAM。页表从 VM_ROOT 开始连续分配。
 */
#define VM_ROOT 0x20000U
#define VM_RING 0x00100000U
#define VM_PROGRESS 0x00101000U
#define VM_ARGS 0x00102000U
#define VM_SIGNAL 0x00103000U
#define VM_DESC 0x00400000U
#define VM_CODE 0x00401000U
#define VM_DATA 0x00402000U

/*
 * 一页用户映射：va 是 GPUVA，target 是物理页地址，flags 是 R/W/X 权限位。
 * VRAM 页的物理地址就是 VRAM 偏移，Host 页写 GPA()。
 */
typedef struct VmMapping {
	uint32_t va;
	uint64_t target;
	uint64_t flags;
} VmMapping;

/*
 * vm_leaf() - 返回 GPUVA 叶子项所在的 VRAM 偏移，按需创建中间表
 * @next_table: 下一个可用表页的 VRAM 偏移，创建表页后向后推进一页。
 *
 * 三级 4 KiB 页表：L0 按 VA[31:30] 有 4 项，L1 按 VA[29:21]、L2 按 VA[20:12]
 * 各 512 项。中间项只保存下一级表页地址和 VALID 位。
 *
 * 循环只走 L0、L1 两级：某项无效时取 next_table 作为新表页并写回该项。
 * 新表页依赖 vm_prepare() 预先清零，所以其中各项都是无效项。
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
 *
 * 必须先启用上下文再映射用户队列：队列激活后，VMID 1 的根和使能都对
 * Host 只读，失效也会被拒绝。
 */
static void vm_prepare(CPTest *s, uint64_t code_flags)
{
	const uint64_t rw = GPGPU_VM_READ | GPGPU_VM_WRITE;
	const VmMapping maps[] = {
		{ VM_RING, GPA(QUEUE_RING(2)), rw },
		{ VM_PROGRESS, GPA(QUEUE_WPTR(0)), rw },
		{ VM_ARGS, GPA(QUEUE_ARGS(0)), GPGPU_VM_READ },
		{ VM_SIGNAL, GPA(QUEUE_SIGNAL(0)), rw },
		{ GPGPU_VM_NOTIFY_VA, GPA(QUEUE_MAILBOX(0)), rw },
		{ VM_DESC, KERNEL_DESC, GPGPU_VM_READ },
		{ VM_CODE, KERNEL_CODE, code_flags },
		{ VM_DATA, KERNEL_OUT, rw },
	};
	uint32_t next_table = VM_ROOT + GPGPU_VM_PAGE_SIZE;

	cp_boot(s, true, 4096);
	queue_mqd(s, 1);
	queue_map(s, 1);
	kernel_load(s, VM_CODE);

	/*
	 * 1. 从 VM_ROOT 起清零 8 页，作为根表和按需分配的中间表、
	 *    叶子表；再为 maps 中的每页写叶子项：物理页地址、权限位
	 *    和 VALID。
	 */
	for (uint32_t off = 0; off < 8 * GPGPU_VM_PAGE_SIZE; off += 8)
		qpci_io_writeq(s->dev, s->vram, VM_ROOT + off, 0);
	for (unsigned int i = 0; i < ARRAY_SIZE(maps); i++)
		qpci_io_writeq(s->dev, s->vram, vm_leaf(s, maps[i].va, &next_table),
			       maps[i].target | maps[i].flags | GPGPU_VM_VALID);

	/*
	 * 2. 写 VMID 1 的页表根，同步失效该 VMID 的 TLB 并确认 ACK，
	 *    再启用上下文。设备只在根页合法时接受启用，没有全局错误
	 *    说明配置成功。
	 */
	wr(s, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ROOT_LO, VM_ROOT);
	wr(s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
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

/* 在槽 2 提交 task 0，包中所有地址都是 VMID 1 下的 GPUVA。 */
static void vm_submit(CPTest *s)
{
	aql_submit(s, 2, 0, 0, VM_DESC, VM_ARGS, VM_SIGNAL, GPGPU_VM_NOTIFY_VA, VM_DATA);
}

/*
 * test_gpuvm_translation() - GPUVM 开启时的完整派发与失效限制
 *
 * 准备：vm_prepare() 建好 VMID 1，代码页可读可执行，槽 2 使用 GPUVA。
 *
 * 期望：CP 读包、描述对象和 Signal，执行器取指、读参数、写数据都经
 * GPUVA 翻译；Kernel 写入 VM_DATA，落到 VRAM KERNEL_OUT。
 */
static void test_gpuvm_translation(void)
{
	CPTest s = { 0 };

	vm_prepare(&s, GPGPU_VM_READ | GPGPU_VM_EXEC);
	vm_submit(&s);
	wait_host32(&s, QUEUE_RPTR(2), 1);
	g_assert_cmphex(qpci_io_readl(s.dev, s.vram, KERNEL_OUT), ==, 0xaabb0000);
	assert_retired(&s, 2, 0, 0);

	/*
	 * 槽 2 仍激活时，VMID 1 正在使用，失效返回 ERROR；卸载槽 2 后
	 * 该 VMID 没有激活队列，失效返回 ACK。
	 */
	wr(&s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ERROR);
	g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, GPA(QUEUE_MQD(2))), ==,
			 GPGPU_MANAGEMENT_OK);
	wr(&s, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(&s, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
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
