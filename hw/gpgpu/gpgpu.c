/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "gpgpu.h"
#include "qemu/host-utils.h"
#include "gpgpu_core.h"

/*
 * gpgpu_vram_read() - 读取 BAR2 的小端数据
 *
 * 先检查 VRAM 范围，越界时锁存故障并返回零。
 */
static uint64_t gpgpu_vram_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return 0;
	}

	return ldn_le_p(s->vram_ptr + addr, size);
}

/*
 * gpgpu_vram_store() - 向 VRAM 写入一个小端标量
 *
 * 经 AddressSpace 写入，使原生 CP 已翻译的代码同步失效。
 */
void gpgpu_vram_store(GPGPUState *s, hwaddr addr, uint64_t val, unsigned int size)
{
	uint8_t buf[8];

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return;
	}

	stn_le_p(buf, size, val);
	address_space_write(&s->vram_as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
}

/*
 * gpgpu_vram_write() - 处理 BAR2 写回调
 *
 * 转交共用写入口，沿用范围检查和 TCG 失效处理。
 */
static void gpgpu_vram_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	gpgpu_vram_store(opaque, addr, val, size);
}

static const MemoryRegionOps gpgpu_vram_ops = {
    .read = gpgpu_vram_read,
    .write = gpgpu_vram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

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
	    s->vram_size > GPGPU_CORE_CTRL_BASE || !is_power_of_2(s->vram_size)) {
		error_setg(errp,
			   "GPGPU: warp_size must be 32; vram_size must be a "
			   "power of two between 4 KiB and 2 GiB");
		return;
	}

	pci_config_set_interrupt_pin(pci_conf, 1);

	/* RAM 支持原生取指，BAR2 保留原访问语义。 */
	if (!memory_region_init_ram_nomigrate(&s->vram_ram, OBJECT(s), "gpgpu-vram-ram",
					      s->vram_size, errp)) {
		return;
	}
	s->vram_ptr = memory_region_get_ram_ptr(&s->vram_ram);
	address_space_init(&s->vram_as, &s->vram_ram, "gpgpu-vram-writes");

	/* BAR0：控制寄存器。 */
	memory_region_init_io(&s->ctrl_mmio, OBJECT(s), &gpgpu_ctrl_ops, s, "gpgpu-ctrl",
			      GPGPU_CTRL_BAR_SIZE);
	pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
			 &s->ctrl_mmio);

	/* BAR2：设备 VRAM。 */
	memory_region_init_io(&s->vram, OBJECT(s), &gpgpu_vram_ops, s, "gpgpu-vram", s->vram_size);
	pci_register_bar(pdev, 2,
			 PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
				 PCI_BASE_ADDRESS_MEM_PREFETCH,
			 &s->vram);

	/* BAR4：Doorbell 通知寄存器。 */
	memory_region_init_io(&s->doorbell_mmio, OBJECT(s), &gpgpu_doorbell_ops, s,
			      "gpgpu-doorbell", GPGPU_DOORBELL_BAR_SIZE);
	pci_register_bar(pdev, 4, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->doorbell_mmio);

	if (!gpgpu_irq_init(s, errp)) {
		address_space_destroy(&s->vram_as);
		return;
	}
	if (pcie_endpoint_cap_init(pdev, 0) < 0) {
		error_setg(errp, "GPGPU: cannot initialize PCIe endpoint capability");
		gpgpu_irq_cleanup(s);
		address_space_destroy(&s->vram_as);
		return;
	}
	gpgpu_dma_init(s);
	gpgpu_dispatch_init(s);

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
	address_space_destroy(&s->vram_as);
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
	/* 复位发生在 realize 成功之后，此时 VRAM 已分配。 */
	address_space_set(&s->vram_as, 0, 0, s->vram_size, MEMTXATTRS_UNSPECIFIED);
	gpgpu_irq_reset(s);
}

static const Property gpgpu_properties[] = {
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
					   VMSTATE_UINT32(irq_enable, GPGPUState),
					   VMSTATE_UINT32(irq_status, GPGPUState),
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
