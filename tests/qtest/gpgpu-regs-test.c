/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU 进程内设备的寄存器级回归。
 *
 * 设备直接挂在 A57 virt 的 PCIe 总线上，不连接原生 CP。此时 CP 不处于
 * 保持复位，Outbound 与 HQD 均拒绝 Host 配置，因此本文件只覆盖与 CP
 * 无关的寄存器语义；完整队列路径见 gpgpu-cp-test.c。
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"
#include "gpgpu-test.h"

/* DMA 测试的 Host 端地址，位于 virt 默认 RAM 内；本文件中无法路由。 */
#define HOST_BUFFER 0x41000000ULL

/* 进程内设备的测试环境：qts 是 A57 客体，regs、vram 映射 BAR0、BAR2。 */
typedef struct RegsTest {
	QTestState *qts;
	QGenericPCIBus bus;
	QPCIDevice *dev;
	QPCIBar regs;
	QPCIBar vram;
} RegsTest;

static void save_device(QPCIDevice *dev, int devfn, void *opaque)
{
	*(QPCIDevice **)opaque = dev;
}

/*
 * regs_start() - 启动挂有 gpgpu 设备的 A57 客体并映射 BAR
 *
 * 1. 以 -device gpgpu 启动客体，gpuvm 开关由参数决定；
 * 2. 按厂商号、设备号找到设备，打开 Memory 与 Bus Master；
 * 3. 映射 BAR0 寄存器和 BAR2 VRAM。本文件不使用 BAR4 Doorbell。
 */
static void regs_start(RegsTest *t, bool gpuvm)
{
	t->qts = qtest_initf("-machine virt -cpu cortex-a57 -m 512M -nodefaults "
			     "-device gpgpu,gpuvm=%s", gpuvm ? "on" : "off");
	qpci_init_generic(&t->bus, t->qts, NULL, false);
	qpci_device_foreach(&t->bus.bus, GPGPU_VENDOR_ID, GPGPU_DEVICE_ID, save_device,
			    &t->dev);
	g_assert_nonnull(t->dev);
	qpci_device_enable(t->dev);
	t->regs = qpci_iomap(t->dev, 0, NULL);
	t->vram = qpci_iomap(t->dev, 2, NULL);
}

/* 解除映射并关闭客体。 */
static void regs_stop(RegsTest *t)
{
	qpci_iounmap(t->dev, t->regs);
	qpci_iounmap(t->dev, t->vram);
	g_free(t->dev);
	qtest_quit(t->qts);
}

/* BAR0 的对齐 32 位读写，宽度和对齐都符合设备要求。 */
static uint32_t rd(RegsTest *t, uint64_t reg)
{
	return qpci_io_readl(t->dev, t->regs, reg);
}

static void wr(RegsTest *t, uint64_t reg, uint32_t value)
{
	qpci_io_writel(t->dev, t->regs, reg, value);
}

/*
 * 读取并清除全部全局错误，返回清除前的值。ERROR_STATUS 写 1 清除对应位；
 * 清除后 GLOBAL_STATUS 的 ERROR 位也应随之消失，函数顺带检查这一点。
 */
static uint32_t take_errors(RegsTest *t)
{
	uint32_t errors = rd(t, GPGPU_REG_ERROR_STATUS);

	wr(t, GPGPU_REG_ERROR_STATUS, errors);
	g_assert_cmphex(rd(t, GPGPU_REG_ERROR_STATUS), ==, 0);
	g_assert_false(rd(t, GPGPU_REG_GLOBAL_STATUS) & GPGPU_STATUS_ERROR);
	return errors;
}

/*
 * test_identity() - 身份寄存器与复位状态
 *
 * 期望：设备号、当前协议版本对应的 DEV_VERSION、64 MiB VRAM 容量和 GPUVM
 * 协议版本都符合头文件定义；VM_MODE 反映 gpuvm 属性；复位后设备 READY、
 * 没有错误。进程内设备没有连接 CP，CP_CTRL 读作 0。
 */
static void test_identity(void)
{
	RegsTest t = { 0 };

	regs_start(&t, true);
	g_assert_cmphex(rd(&t, GPGPU_REG_DEV_ID), ==, GPGPU_DEV_ID_VALUE);
	g_assert_cmphex(rd(&t, GPGPU_REG_DEV_VERSION), ==, GPGPU_DEV_VERSION_VALUE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VRAM_SIZE_LO), ==, GPGPU_TEST_VRAM_SIZE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VRAM_SIZE_HI), ==, 0);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_VERSION), ==, GPGPU_PROTOCOL_VERSION);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_MODE), ==, 1);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_STATUS), ==, GPGPU_STATUS_READY);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);
	g_assert_cmphex(rd(&t, GPGPU_REG_CP_CTRL), ==, 0);
	regs_stop(&t);

	regs_start(&t, false);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_MODE), ==, 0);
	regs_stop(&t);
}

/*
 * test_access_rules() - BAR0 的非法访问与 GLOBAL_CTRL 语义
 *
 * 期望：非 32 位访问、Host 写 CP 专用寄存器、越界选槽都锁存 INVALID_CMD，
 * 并保留目标原值；GLOBAL_CTRL 的 ENABLE 可读回，RESET 执行整体复位后
 * 不留在寄存器中。
 */
static void test_access_rules(void)
{
	RegsTest t = { 0 };

	regs_start(&t, true);

	/* 1. BAR0 只接受对齐的 32 位访问，16 位读取锁存 INVALID_CMD。 */
	qpci_io_readw(t.dev, t.regs, GPGPU_REG_DEV_ID);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);

	/* 2. Dispatch 寄存器组只允许 CP 写入；Host 写入被拒绝，读回仍为 0。 */
	wr(&t, GPGPU_REG_GRID_DIM_X, 1);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, GPGPU_REG_GRID_DIM_X), ==, 0);

	/*
	 * 3. 先合法地选中槽 2，再写超出范围的槽号 GPGPU_HQD_SLOTS：
	 *    后一次写入锁存 INVALID_CMD，HQD_SELECT 仍为 2。
	 */
	wr(&t, GPGPU_REG_HQD_SELECT, 2);
	wr(&t, GPGPU_REG_HQD_SELECT, GPGPU_HQD_SLOTS);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, GPGPU_REG_HQD_SELECT), ==, 2);

	/*
	 * 4. ENABLE 写入后可读回；RESET 是一次性命令，执行整体复位后
	 *    GLOBAL_CTRL 读作 0，第 3 步选中的 HQD_SELECT 也被清零。
	 */
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_CTRL), ==, GPGPU_CTRL_ENABLE);
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_CTRL), ==, 0);
	g_assert_cmpuint(rd(&t, GPGPU_REG_HQD_SELECT), ==, 0);
	regs_stop(&t);
}

/*
 * test_irq() - 中断使能掩码与错误事件锁存
 *
 * 期望：IRQ_ENABLE 写全 1 后只保留 DMA_DONE、ERROR、IH 三个已定义位。
 * 用一次非法写入（Host 写 GRID_DIM_X）触发全局错误，IRQ_STATUS 的
 * ERROR 位置 1，写 IRQ_ACK 后清除；最后清掉 ERROR_STATUS 中的错误。
 */
static void test_irq(void)
{
	const uint32_t defined = GPGPU_IRQ_DMA_DONE | GPGPU_IRQ_ERROR | GPGPU_IRQ_IH;
	RegsTest t = { 0 };

	regs_start(&t, true);
	wr(&t, GPGPU_REG_IRQ_ENABLE, UINT32_MAX);
	g_assert_cmphex(rd(&t, GPGPU_REG_IRQ_ENABLE), ==, defined);

	wr(&t, GPGPU_REG_GRID_DIM_X, 1);
	g_assert_cmphex(rd(&t, GPGPU_REG_IRQ_STATUS) & GPGPU_IRQ_ERROR, ==, GPGPU_IRQ_ERROR);
	wr(&t, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_ERROR);
	g_assert_cmphex(rd(&t, GPGPU_REG_IRQ_STATUS) & GPGPU_IRQ_ERROR, ==, 0);
	take_errors(&t);
	regs_stop(&t);
}

/*
 * test_vram() - BAR2 的 32 位、64 位读写与末尾边界
 *
 * 期望：VRAM 中的 32 位和 64 位写入都能原样读回；VRAM 最后一个 32 位字
 * 可以访问，整个过程不产生全局错误。
 */
static void test_vram(void)
{
	RegsTest t = { 0 };

	regs_start(&t, true);
	qpci_io_writel(t.dev, t.vram, 0x1000, 0x12345678);
	qpci_io_writeq(t.dev, t.vram, 0x2000, 0x0123456789abcdefULL);
	g_assert_cmphex(qpci_io_readl(t.dev, t.vram, 0x1000), ==, 0x12345678);
	g_assert_cmphex(qpci_io_readq(t.dev, t.vram, 0x2000), ==, 0x0123456789abcdefULL);
	qpci_io_writel(t.dev, t.vram, GPGPU_TEST_VRAM_SIZE - 4, 0xa5a5a5a5);
	g_assert_cmphex(qpci_io_readl(t.dev, t.vram, GPGPU_TEST_VRAM_SIZE - 4), ==, 0xa5a5a5a5);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);
	regs_stop(&t);
}

/*
 * test_dma_unrouted() - 未打开 Outbound 时 DMA 的 Host 端无法路由
 *
 * 准备：进程内设备没有连接 CP，CP 不处于保持复位，设备不满足
 * “引擎全部停止”的条件，所以 Outbound 不能打开。
 *
 * 期望：打开 Outbound 被拒绝；布局寄存器只读；Host→VRAM 的描述符因
 * Host 端地址无法路由而立即失败，状态为 ERROR 并锁存 DMA 故障。
 */
static void test_dma_unrouted(void)
{
	const uint64_t host = GPGPU_OUTBOUND_BASE + HOST_BUFFER;
	RegsTest t = { 0 };

	regs_start(&t, true);

	/*
	 * 1. 只有引擎全部停止（含 CP 保持复位）时才允许配置 Outbound；这里
	 *    写入被拒绝，OUTBOUND_STATUS 为 ERROR 并锁存 INVALID_CMD。
	 */
	wr(&t, GPGPU_REG_OUTBOUND_CONTROL, GPGPU_OUTBOUND_ENABLE);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_STATUS), ==, GPGPU_OUTBOUND_ERROR);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);

	/*
	 * 2. Outbound 布局固定为 0x8000_0000 起的 2 GiB，与 VRAM 容量无关，
	 *    布局寄存器只读：改写 GPU_BASE 锁存 INVALID_CMD，读回不变。
	 */
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_GPU_BASE), ==, GPGPU_OUTBOUND_BASE);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_SIZE_LO), ==, (uint32_t)GPGPU_OUTBOUND_SIZE);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_SIZE_HI), ==, GPGPU_OUTBOUND_SIZE >> 32);
	wr(&t, GPGPU_REG_OUTBOUND_GPU_BASE, 0);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_GPU_BASE), ==, GPGPU_OUTBOUND_BASE);

	/*
	 * 3. 设备使能后提交 Host→VRAM 的 64 字节描述符：源地址落在未打开的
	 *    Outbound 窗口内，START 时检查即失败，不会搬运任何数据。
	 */
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	wr(&t, GPGPU_REG_DMA_SRC_LO, (uint32_t)host);
	wr(&t, GPGPU_REG_DMA_SRC_HI, host >> 32);
	wr(&t, GPGPU_REG_DMA_DST_LO, 0x3000);
	wr(&t, GPGPU_REG_DMA_DST_HI, 0);
	wr(&t, GPGPU_REG_DMA_SIZE, 64);
	wr(&t, GPGPU_REG_DMA_CTRL, GPGPU_DMA_START);
	g_assert_cmphex(rd(&t, GPGPU_REG_DMA_STATUS), ==, GPGPU_DMA_ERROR);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_DMA_FAULT);
	regs_stop(&t);
}

/*
 * test_gpuvm_context() - GPUVM 上下文寄存器的写入规则
 *
 * 期望：根须为合法表页才能启用，启用后根只读；没有激活队列和
 * 在途访问时同步失效立即完成，ACK 为所选 VMID 的位；含保留位、位图
 * 为空或范围起点大于终点的请求整体拒绝，ACK 只有 ERROR；整体复位清除
 * 上下文、ACK 与 VPN 范围；gpuvm=off 时上下文与失效寄存器不可写。
 */
static void test_gpuvm_context(void)
{
	const uint64_t ctx = GPGPU_REG_VM_CONTEXT(1);
	RegsTest t = { 0 };

	regs_start(&t, true);

	/*
	 * 1. 根复位为 0，指向受保护的 VRAM 首页，不是合法表页：
	 *    启用被拒绝，ENABLE 仍为 0。
	 */
	wr(&t, ctx + GPGPU_VM_ENABLE, 1);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 0);

	/* 2. 根设为页对齐的 VRAM 地址 0x20000 后可以启用，不产生错误。 */
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x20000);
	wr(&t, ctx + GPGPU_VM_ENABLE, 1);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 1);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);

	/* 3. 启用后根只读：改写为 0x30000 锁存 INVALID_CMD，读回仍为 0x20000。 */
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x30000);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmphex(rd(&t, ctx + GPGPU_VM_ROOT_LO), ==, 0x20000);

	/*
	 * 4. VMID 1 没有激活队列和在途访问，同步失效直接完成，ACK 为 VMID 1
	 *    的位。下面三个请求都整体拒绝，ACK 只有 ERROR：带上保留的
	 *    VMID 0 位；只有 RANGE、没有 VMID；范围 0x402～0x400 起点大于终点。
	 */
	wr(&t, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(1));
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_VMID(1));
	wr(&t, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(0) | GPGPU_VM_INV_VMID(1));
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_ERROR);
	wr(&t, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_RANGE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_ERROR);
	wr(&t, GPGPU_REG_VM_INV_VPN_START, 0x402);
	wr(&t, GPGPU_REG_VM_INV_VPN_END, 0x400);
	wr(&t, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(1) | GPGPU_VM_INV_RANGE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_ACK), ==, GPGPU_VM_INV_ERROR);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);

	/* 5. 整体复位后 ENABLE、根、ACK 与 VPN 范围都回到 0。 */
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 0);
	g_assert_cmphex(rd(&t, ctx + GPGPU_VM_ROOT_LO), ==, 0);
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_ACK), ==, 0);
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_VPN_START), ==, 0);
	g_assert_cmphex(rd(&t, GPGPU_REG_VM_INV_VPN_END), ==, 0);
	regs_stop(&t);

	/* 6. gpuvm=off 时写上下文的根或失效请求都锁存 INVALID_CMD。 */
	regs_start(&t, false);
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x20000);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	wr(&t, GPGPU_REG_VM_INV_REQ, GPGPU_VM_INV_VMID(1));
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	regs_stop(&t);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	qtest_add_func("/gpgpu/regs/identity", test_identity);
	qtest_add_func("/gpgpu/regs/access-rules", test_access_rules);
	qtest_add_func("/gpgpu/regs/irq", test_irq);
	qtest_add_func("/gpgpu/regs/vram", test_vram);
	qtest_add_func("/gpgpu/regs/dma-unrouted", test_dma_unrouted);
	qtest_add_func("/gpgpu/regs/gpuvm-context", test_gpuvm_context);
	return g_test_run();
}
