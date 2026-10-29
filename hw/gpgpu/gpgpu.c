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
 * 1. 有跨进程物理访问在途时拒绝：它可能暂时释放了主锁，返回后仍会使用
 *    当前地址配置；
 * 2. 先停止访问的发起方：CP、执行器；
 * 3. 再复位被访问的状态：GPUVM、HQD、IH；
 * 4. 最后复位全局寄存器、中断、DMA 与 Outbound。
 */
bool gpgpu_reset_registers(GPGPUState *s)
{
	/* 1. 在途访问。 */
	if (gpgpu_phys_busy(s)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return false;
	}

	/* 2. 发起方。 */
	gpgpu_cp_reset(s);
	gpgpu_dispatch_reset(s);

	/* 3. 被访问的状态。 */
	gpgpu_vm_reset(s);
	gpgpu_hqd_reset(s);
	gpgpu_ih_reset(s);

	/* 4. 全局与外围。 */
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
 * gpgpu_realize() - 建立 PCI 资源
 *
 * 1. 检查属性；2. 注册 VRAM（BAR2）、寄存器（BAR0）、Doorbell（BAR4）；
 * 3. 建立中断与 PCIe 能力；4. 创建定时器并进入复位后状态。
 */
static void gpgpu_realize(PCIDevice *pdev, Error **errp)
{
	GPGPUState *s = GPGPU(pdev);

	/* 1. 属性。 */
	if (!gpgpu_check_properties(s, errp))
		return;
	pci_config_set_interrupt_pin(pdev->config, 1);

	/* 2. BAR。 */
	if (!gpgpu_vram_init(s, errp))
		return;
	gpgpu_bar0_init(s);
	gpgpu_doorbell_init(s);

	/* 3. 中断与 PCIe。 */
	if (!gpgpu_irq_init(s, errp))
		goto fail_vram;
	if (pcie_endpoint_cap_init(pdev, 0) < 0) {
		error_setg(errp, "gpgpu: cannot initialize PCIe endpoint capability");
		goto fail_irq;
	}

	/* 4. 定时器与初始状态。 */
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

/* QOM 复位：复位寄存器后再清空 VRAM 和 PCI 中断能力；寄存器复位被拒绝时都保留。 */
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
