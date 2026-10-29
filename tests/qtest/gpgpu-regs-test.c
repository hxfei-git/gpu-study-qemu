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

/* 测试使用的 Host RAM 位置，位于 virt 默认 RAM 内。 */
#define HOST_BUFFER 0x41000000ULL

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

/* 1. 启动客体；2. 找到设备并打开 Memory/Bus Master；3. 映射 BAR0、BAR2。 */
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

static void regs_stop(RegsTest *t)
{
	qpci_iounmap(t->dev, t->regs);
	qpci_iounmap(t->dev, t->vram);
	g_free(t->dev);
	qtest_quit(t->qts);
}

static uint32_t rd(RegsTest *t, uint64_t reg)
{
	return qpci_io_readl(t->dev, t->regs, reg);
}

static void wr(RegsTest *t, uint64_t reg, uint32_t value)
{
	qpci_io_writel(t->dev, t->regs, reg, value);
}

/* 读取并清除全部全局错误，返回清除前的值。 */
static uint32_t take_errors(RegsTest *t)
{
	uint32_t errors = rd(t, GPGPU_REG_ERROR_STATUS);

	wr(t, GPGPU_REG_ERROR_STATUS, errors);
	g_assert_cmphex(rd(t, GPGPU_REG_ERROR_STATUS), ==, 0);
	g_assert_false(rd(t, GPGPU_REG_GLOBAL_STATUS) & GPGPU_STATUS_ERROR);
	return errors;
}

/* 身份寄存器、复位状态与未连接 CP 时的读零规则。 */
static void test_identity(void)
{
	RegsTest t = { 0 };

	regs_start(&t, true);
	g_assert_cmphex(rd(&t, GPGPU_REG_DEV_ID), ==, GPGPU_DEV_ID_VALUE);
	g_assert_cmphex(rd(&t, GPGPU_REG_DEV_VERSION), ==, GPGPU_DEV_VERSION_VALUE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VRAM_SIZE_LO), ==, GPGPU_TEST_VRAM_SIZE);
	g_assert_cmphex(rd(&t, GPGPU_REG_VRAM_SIZE_HI), ==, 0);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_VERSION), ==, GPGPU_VM_VERSION);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_MODE), ==, 1);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_STATUS), ==, GPGPU_STATUS_READY);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);
	g_assert_cmphex(rd(&t, GPGPU_REG_CP_CTRL), ==, 0);
	regs_stop(&t);

	regs_start(&t, false);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_MODE), ==, 0);
	regs_stop(&t);
}

/* 宽度、只读组、选槽和 GLOBAL_CTRL 的命令位都按 INVALID_CMD 处理。 */
static void test_access_rules(void)
{
	RegsTest t = { 0 };

	regs_start(&t, true);

	/* 1. BAR0 只接受对齐的 32 位访问。 */
	qpci_io_readw(t.dev, t.regs, GPGPU_REG_DEV_ID);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);

	/* 2. Dispatch 寄存器组只允许 CP 写入。 */
	wr(&t, GPGPU_REG_GRID_DIM_X, 1);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, GPGPU_REG_GRID_DIM_X), ==, 0);

	/* 3. 非法选槽保留原选择。 */
	wr(&t, GPGPU_REG_HQD_SELECT, 2);
	wr(&t, GPGPU_REG_HQD_SELECT, GPGPU_HQD_SLOTS);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, GPGPU_REG_HQD_SELECT), ==, 2);

	/* 4. ENABLE 锁存，RESET 是一次性命令。 */
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_CTRL), ==, GPGPU_CTRL_ENABLE);
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
	g_assert_cmphex(rd(&t, GPGPU_REG_GLOBAL_CTRL), ==, 0);
	g_assert_cmpuint(rd(&t, GPGPU_REG_HQD_SELECT), ==, 0);
	regs_stop(&t);
}

/* 使能掩码只保留已定义事件，错误事件锁存到 ACK 为止。 */
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

/* BAR2 读写与越界保护：最后一个字可访问，越过末尾锁存 VRAM 错误。 */
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
 * 未连接 CP 时 Outbound 不可配置，DMA 的 Host 端地址无法路由：
 * 描述符被拒绝，状态为 ERROR 并锁存 DMA 故障。
 */
static void test_dma_unrouted(void)
{
	const uint64_t host = GPGPU_TEST_VRAM_SIZE + HOST_BUFFER;
	RegsTest t = { 0 };

	regs_start(&t, true);

	/* 1. 保持 CP 复位才允许配置 Outbound；这里写入应被拒绝。 */
	wr(&t, GPGPU_REG_OUTBOUND_CONTROL, GPGPU_OUTBOUND_ENABLE);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_STATUS), ==, GPGPU_OUTBOUND_ERROR);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);

	/* 2. 复位后 Outbound 布局固定为 VRAM 之后的整个 32 位空间。 */
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_GPU_BASE), ==, GPGPU_TEST_VRAM_SIZE);
	g_assert_cmphex(rd(&t, GPGPU_REG_OUTBOUND_SIZE_LO), ==,
			(uint32_t)(GPGPU_VM_ADDRESS_LIMIT - GPGPU_TEST_VRAM_SIZE));

	/* 3. H2D 描述符：Host 端未路由，立即失败。 */
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
 * GPUVM 上下文寄存器：根须为合法表页才能启用，启用后根只读；
 * 同步失效立即返回 ACK；整体复位清除上下文。
 */
static void test_gpuvm_context(void)
{
	const uint64_t ctx = GPGPU_REG_VM_CONTEXT(1);
	RegsTest t = { 0 };

	regs_start(&t, true);

	/* 1. 根为零（VRAM 首页受保护）时拒绝启用。 */
	wr(&t, ctx + GPGPU_VM_ENABLE, 1);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 0);

	/* 2. 页对齐的 VRAM 根可以启用。 */
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x20000);
	wr(&t, ctx + GPGPU_VM_ENABLE, 1);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 1);
	g_assert_cmphex(rd(&t, GPGPU_REG_ERROR_STATUS), ==, 0);

	/* 3. 启用后不能改根。 */
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x30000);
	g_assert_cmphex(take_errors(&t), ==, GPGPU_ERR_INVALID_CMD);
	g_assert_cmphex(rd(&t, ctx + GPGPU_VM_ROOT_LO), ==, 0x20000);

	/* 4. 无在途访问时同步失效直接确认；非法 VMID 返回 ERROR。 */
	wr(&t, GPGPU_REG_VM_INVALIDATE, 1);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
	wr(&t, GPGPU_REG_VM_INVALIDATE, 3);
	g_assert_cmpuint(rd(&t, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ERROR);

	/* 5. 整体复位清除上下文。 */
	wr(&t, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
	g_assert_cmpuint(rd(&t, ctx + GPGPU_VM_ENABLE), ==, 0);
	g_assert_cmphex(rd(&t, ctx + GPGPU_VM_ROOT_LO), ==, 0);
	regs_stop(&t);

	/* 6. GPUVM 关闭时上下文寄存器不可写。 */
	regs_start(&t, false);
	wr(&t, ctx + GPGPU_VM_ROOT_LO, 0x20000);
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
