// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu.h"
#include "gpgpu_internal.h"
#include "gpgpu_mmio.h"
#include "cp/gpgpu_cp.h"
#include "cp/gpgpu_hqd.h"
#include "cp/gpgpu_doorbell.h"
#include "compute/gpgpu_dispatch.h"
#include "compute/gpgpu_memory.h"
#include "compute/gpgpu_exec_types.h"
#include "memory/gpgpu_vram.h"
#include "memory/gpgpu_vm.h"
#include "memory/gpgpu_host.h"
#include "memory/gpgpu_dma.h"
#include "interrupt/gpgpu_ih.h"
#include "interrupt/gpgpu_irq.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"

#include "qemu/host-utils.h"

/* 停止原生 CP 和任务后，依次复位各模块；此入口保留 VRAM。 */
void gpgpu_reset_state(GPGPUState *s)
{
	/* 跨进程 DMA 可能临时让出主锁；其返回前不能释放或重配地址空间。 */
	if (s->physical.accesses) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	gpgpu_cp_device_reset(s);
	gpgpu_dispatch_reset(s);
	gpgpu_vm_reset(s);
	gpgpu_hqd_reset(s);
	gpgpu_ih_reset(s);
	s->global_ctrl = 0;
	s->global_status = GPGPU_STATUS_READY;
	s->error_status = 0;
	gpgpu_irq_clear(s);
	gpgpu_dma_reset(s);
	gpgpu_outbound_reset(s);
}

/* 全局错误由设备锁存，再交 IRQ 模块通知 Host。 */
void gpgpu_set_error(GPGPUState *s, GPGPUError error)
{
	s->error_status |= error;
	s->global_status |= GPGPU_STATUS_ERROR;
	gpgpu_raise_irq(s, GPGPU_IRQ_ERROR);
}

/*
 * gpgpu_realize() - 建立 PCI 设备资源
 *
 * 校验属性后注册 VRAM、控制与 Doorbell BAR，再初始化中断和定时器。
 */
static void gpgpu_realize(PCIDevice *pdev, Error **errp)
{
	GPGPUState *s = GPGPU(pdev);
	uint8_t *pci_conf = pdev->config;

	if (s->warp_size != GPGPU_WARP_SIZE || s->vram_size < 4096 ||
	    s->vram_size >= (1ULL << 31) || !is_power_of_2(s->vram_size)) {
		error_setg(errp,
			   "GPGPU: warp_size must be 32; vram_size must be a "
			   "power of two from 4 KiB to less than 2 GiB");
		return;
	}

	pci_config_set_interrupt_pin(pci_conf, 1);

	if (!gpgpu_vram_init(s, errp))
		return;

	/* BAR0：控制寄存器。 */
	memory_region_init_io(&s->ctrl_mmio, OBJECT(s), &gpgpu_ctrl_ops, s, "gpgpu-ctrl",
			      GPGPU_CTRL_BAR_SIZE);
	pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
			 &s->ctrl_mmio);

	gpgpu_vram_register_bar(s);
	gpgpu_doorbell_init(s);

	if (!gpgpu_irq_init(s, errp)) {
		gpgpu_vram_cleanup(s);
		return;
	}
	if (pcie_endpoint_cap_init(pdev, 0) < 0) {
		error_setg(errp, "GPGPU: cannot initialize PCIe endpoint capability");
		gpgpu_irq_cleanup(s);
		gpgpu_vram_cleanup(s);
		return;
	}
	gpgpu_dma_init(s);
	gpgpu_dispatch_init(s);
	gpgpu_outbound_reset(s);

	s->global_status = GPGPU_STATUS_READY;
}

/*
 * gpgpu_exit() - 销毁 QEMU 设备资源
 *
 * 先保持 CP 复位，再释放 DMA、执行器、中断和 VRAM 地址空间。
 */
static void gpgpu_exit(PCIDevice *pdev)
{
	GPGPUState *s = GPGPU(pdev);

	gpgpu_cp_reset(s);
	gpgpu_dma_cleanup(s);
	gpgpu_dispatch_cleanup(s);
	gpgpu_irq_cleanup(s);
	pcie_cap_exit(pdev);
	gpgpu_vram_cleanup(s);
}

/*
 * gpgpu_reset() - 执行 QOM 设备复位
 *
 * 停止当前操作、清空 VRAM，并复位 PCI 中断能力状态。
 */
static void gpgpu_reset(DeviceState *dev)
{
	GPGPUState *s = GPGPU(dev);

	gpgpu_reset_state(s);
	if (s->physical.accesses)
		return;
	/* 复位发生在 realize 成功之后，此时 VRAM 已分配。 */
	gpgpu_vram_reset(s);
	gpgpu_irq_reset(s);
}

static const Property gpgpu_properties[] = {
	DEFINE_PROP_BOOL("gpuvm", GPGPUState, gpuvm, true),
	DEFINE_PROP_UINT32("num_cus", GPGPUState, num_cus, GPGPU_DEFAULT_NUM_CUS),
	DEFINE_PROP_UINT32("warps_per_cu", GPGPUState, warps_per_cu, GPGPU_DEFAULT_WARPS_PER_CU),
	DEFINE_PROP_UINT32("warp_size", GPGPUState, warp_size, GPGPU_DEFAULT_WARP_SIZE),
	DEFINE_PROP_UINT64("vram_size", GPGPUState, vram_size, GPGPU_DEFAULT_VRAM_SIZE),
};

static const VMStateDescription vmstate_gpgpu = {
	.name = "gpgpu",
	/* 目前尚未迁移 VRAM 和执行中的操作。 */
	.unmigratable = true,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) { VMSTATE_PCI_DEVICE(parent_obj, GPGPUState),
					   VMSTATE_UINT32(global_ctrl, GPGPUState),
					   VMSTATE_UINT32(global_status, GPGPUState),
					   VMSTATE_UINT32(error_status, GPGPUState),
					   VMSTATE_UINT32(irq.enable, GPGPUState),
					   VMSTATE_UINT32(irq.status, GPGPUState),
					   VMSTATE_END_OF_LIST() }
};

/*
 * gpgpu_class_init() - 安装 QOM 设备类回调
 *
 * 登记 PCI 标识、属性和生命周期入口，并禁止当前设备迁移。
 */
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

/*
 * gpgpu_register_types() - 注册教学 GPGPU 的 QOM 类型
 *
 * 由模块初始化调用，使 PCI 总线能按类型名实例化设备。
 */
static void gpgpu_register_types(void)
{
	type_register_static(&gpgpu_type_info);
}

type_init(gpgpu_register_types)
