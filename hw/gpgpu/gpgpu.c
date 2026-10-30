// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 教学设备：QOM/PCI 组装、全局寄存器与复位编排
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "gpgpu_state.h"
#include "bar0.h"

/* vram_size 属性的下限与上限（不含），单位字节。 */
#define VRAM_SIZE_MIN 4096
#define VRAM_SIZE_LIMIT (1ULL << 31)

/* ---- 跨模块入口 ---- */

void gpgpu_set_error(GPGPUState *s, uint32_t error)
{
	s->error_status |= error;
	s->global_status |= GPGPU_STATUS_ERROR;
	gpgpu_raise_irq(s, GPGPU_IRQ_ERROR);
}

bool gpgpu_enabled(GPGPUState *s)
{
	return s->global_ctrl & GPGPU_CTRL_ENABLE;
}

void gpgpu_set_busy(GPGPUState *s, bool busy)
{
	if (busy)
		s->global_status |= GPGPU_STATUS_BUSY;
	else
		s->global_status &= ~GPGPU_STATUS_BUSY;
}

bool gpgpu_gpuvm_enabled(GPGPUState *s)
{
	return s->gpuvm;
}

bool gpgpu_engines_idle(GPGPUState *s)
{
	return gpgpu_cp_held(s) && !gpgpu_dispatch_busy(s) && !gpgpu_dma_busy(s) &&
		!gpgpu_ih_busy(s) && !gpgpu_hqd_any_active(s);
}

/*
 * gpgpu_reset_registers() - 按依赖顺序复位各模块寄存器
 *
 * Host 写 GLOBAL_CTRL.RESET 和 QOM 复位都走这里。各模块之间有访问
 * 关系：CP、执行器会读写 GPUVM、HQD、IH 的状态，所以先停发起方，
 * 再清被访问的状态，避免复位中途还有访问用到半清空的配置：
 *
 * 1. 有跨进程物理访问在途时拒绝复位并锁存 INVALID_CMD。这类访问
 *    经 vfio-user DMA 时可能暂时释放了主锁，返回后仍会使用当前的
 *    地址配置，此时清空会让它访问到错误位置；
 * 2. 先让 CP 保持复位、停止执行器，此后不再产生新访问；
 * 3. 再复位被访问的状态：GPUVM 上下文与 TLB、HQD、IH；
 * 4. 最后复位全局寄存器、中断、DMA 与 Outbound 窗口。
 *
 * Return: true 表示已复位；false 表示被拒绝，除锁存的 INVALID_CMD
 * 外各模块状态不变。
 */
bool gpgpu_reset_registers(GPGPUState *s)
{
	/* 1. 在途物理访问未结束时整体拒绝，不做部分复位。 */
	if (gpgpu_phys_busy(s)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return false;
	}

	/* 2. 停住访问发起方：CP 保持复位，执行器回到 IDLE 并释放任务。 */
	gpgpu_cp_reset(s);
	gpgpu_dispatch_reset(s);

	/* 3. 发起方已停，可以安全清空它们会访问的状态。 */
	gpgpu_vm_reset(s);
	gpgpu_hqd_reset(s);
	gpgpu_ih_reset(s);

	/* 4. 全局状态回到 READY，中断、DMA 与 Outbound 回到复位值。 */
	s->global_ctrl = 0;
	s->global_status = GPGPU_STATUS_READY;
	s->error_status = 0;
	gpgpu_irq_reset(s);
	gpgpu_dma_reset(s);
	gpgpu_outbound_reset(s);
	return true;
}

/* ---- 0x000 设备信息与 0x100 全局控制（由 bar0.c 转发） ---- */

uint32_t gpgpu_info_read(GPGPUState *s, hwaddr reg)
{
	switch (reg) {
	case GPGPU_REG_DEV_ID:
		return GPGPU_DEV_ID_VALUE;
	case GPGPU_REG_DEV_VERSION:
		return GPGPU_DEV_VERSION_VALUE;
	case GPGPU_REG_DEV_CAPS:
		return (s->num_cus & 0xff) | (s->warps_per_cu & 0xff) << 8 |
			(GPGPU_WARP_SIZE & 0xff) << 16;
	case GPGPU_REG_VRAM_SIZE_LO:
	case GPGPU_REG_VRAM_SIZE_HI:
		return gpgpu_reg64_read(s->vram_size, reg - GPGPU_REG_VRAM_SIZE_LO);
	default:
		return 0;
	}
}

uint32_t gpgpu_global_read(GPGPUState *s, hwaddr reg)
{
	switch (reg) {
	case GPGPU_REG_GLOBAL_CTRL:
		return s->global_ctrl;
	case GPGPU_REG_GLOBAL_STATUS:
		return s->global_status;
	case GPGPU_REG_ERROR_STATUS:
		return s->error_status;
	default:
		return 0;
	}
}

/* GLOBAL_CTRL：RESET 复位寄存器，否则锁存 ENABLE。ERROR_STATUS：写 1 清除。 */
void gpgpu_global_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	switch (reg) {
	case GPGPU_REG_GLOBAL_CTRL:
		if (value & GPGPU_CTRL_RESET)
			gpgpu_reset_registers(s);
		else
			s->global_ctrl = value & GPGPU_CTRL_ENABLE;
		break;
	case GPGPU_REG_ERROR_STATUS:
		s->error_status &= ~value;
		if (!s->error_status)
			s->global_status &= ~GPGPU_STATUS_ERROR;
		break;
	default:
		break;
	}
}

/* ---- QOM 生命周期 ---- */

static bool gpgpu_check_properties(GPGPUState *s, Error **errp)
{
	if (s->vram_size < VRAM_SIZE_MIN || s->vram_size >= VRAM_SIZE_LIMIT ||
	    !is_power_of_2(s->vram_size)) {
		error_setg(errp, "gpgpu: vram_size must be a power of two in [4 KiB, 2 GiB)");
		return false;
	}
	return true;
}

/*
 * gpgpu_realize() - 实例化设备并建立 PCI 资源
 *
 * QOM 在设备加入机器时调用一次。按资源依赖分四步，后一步失败时
 * 逆序撤销已建立的部分：
 *
 * 1. 检查 vram_size 等属性，设置 INTx 引脚；
 * 2. 注册 VRAM（BAR2）、寄存器（BAR0）、Doorbell（BAR4）三个 BAR；
 * 3. 建立中断与 PCIe 端点能力。MSI-X 表挂在 BAR0 末尾，
 *    所以必须在第 2 步建好 BAR0 之后；
 * 4. 创建 DMA 和执行器的定时器，把 Outbound 与全局状态置为复位值。
 */
static void gpgpu_realize(PCIDevice *pdev, Error **errp)
{
	GPGPUState *s = GPGPU(pdev);

	/* 1. 属性不合法时直接返回，此时尚未分配任何资源。 */
	if (!gpgpu_check_properties(s, errp))
		return;
	pci_config_set_interrupt_pin(pdev->config, 1);

	/* 2. VRAM 失败直接返回；BAR0、Doorbell 只建区域，不会失败。 */
	if (!gpgpu_vram_init(s, errp))
		return;
	gpgpu_bar0_init(s);
	gpgpu_doorbell_init(s);

	/* 3. 失败时跳到对应标签，逆序释放中断和 VRAM。 */
	if (!gpgpu_irq_init(s, errp))
		goto fail_vram;
	if (pcie_endpoint_cap_init(pdev, 0) < 0) {
		error_setg(errp, "gpgpu: cannot initialize PCIe endpoint capability");
		goto fail_irq;
	}

	/* 4. 定时器与复位后状态，此后 Host 读到 GLOBAL_STATUS = READY。 */
	gpgpu_dma_init(s);
	gpgpu_dispatch_init(s);
	gpgpu_outbound_reset(s);
	s->global_status = GPGPU_STATUS_READY;
	return;

fail_irq:
	gpgpu_irq_cleanup(s);
fail_vram:
	gpgpu_vram_cleanup(s);
}

/* 先让 CP 停住，再释放定时器、中断与 VRAM 地址空间。 */
static void gpgpu_exit(PCIDevice *pdev)
{
	GPGPUState *s = GPGPU(pdev);

	gpgpu_cp_hold(s);

	gpgpu_dma_cleanup(s);
	gpgpu_dispatch_cleanup(s);
	gpgpu_irq_cleanup(s);
	pcie_cap_exit(pdev);
	gpgpu_vram_cleanup(s);
}

/*
 * QOM 复位：先复位寄存器，再清空 VRAM 和 PCI 中断能力。
 * 寄存器复位被拒绝（仍有在途物理访问）时，VRAM 和中断能力也保留。
 */
static void gpgpu_reset(DeviceState *dev)
{
	GPGPUState *s = GPGPU(dev);

	if (!gpgpu_reset_registers(s))
		return;
	gpgpu_vram_clear(s);
	gpgpu_irq_reset_pci(s);
}

static const Property gpgpu_properties[] = {
	DEFINE_PROP_BOOL("gpuvm", GPGPUState, gpuvm, true),
	DEFINE_PROP_UINT32("num_cus", GPGPUState, num_cus, GPGPU_DEFAULT_NUM_CUS),
	DEFINE_PROP_UINT32("warps_per_cu", GPGPUState, warps_per_cu, GPGPU_DEFAULT_WARPS_PER_CU),
	DEFINE_PROP_UINT64("vram_size", GPGPUState, vram_size, GPGPU_DEFAULT_VRAM_SIZE),
};

/* VRAM 与执行中的任务都不迁移。 */
static const VMStateDescription vmstate_gpgpu = {
	.name = TYPE_GPGPU,
	.unmigratable = 1,
};

static void gpgpu_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

	pc->realize = gpgpu_realize;
	pc->exit = gpgpu_exit;
	pc->vendor_id = GPGPU_VENDOR_ID;
	pc->device_id = GPGPU_DEVICE_ID;
	pc->revision = GPGPU_REVISION;
	pc->class_id = GPGPU_CLASS_CODE;

	device_class_set_legacy_reset(dc, gpgpu_reset);
	dc->desc = "Educational GPGPU Device";
	dc->vmsd = &vmstate_gpgpu;
	device_class_set_props(dc, gpgpu_properties);
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo gpgpu_type_info = {
	.name = TYPE_GPGPU,
	.parent = TYPE_PCI_DEVICE,
	.instance_size = sizeof(GPGPUState),
	.class_init = gpgpu_class_init,
	.interfaces = (InterfaceInfo[]) { { INTERFACE_PCIE_DEVICE }, {} },
};

static void gpgpu_register_types(void)
{
	type_register_static(&gpgpu_type_info);
}

type_init(gpgpu_register_types)
