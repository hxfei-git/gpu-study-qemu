/*
 * QEMU Educational GPGPU Device
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include "gpgpu.h"
#include "gpgpu_core.h"

static void gpgpu_update_irq(GPGPUState *s, uint32_t events)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint32_t pending = s->irq_status & s->irq_enable;

    events &= pending;
    pci_set_irq(pdev, pending && !msix_enabled(pdev) && !msi_enabled(pdev));
    if (msix_enabled(pdev)) {
        if (events & GPGPU_IRQ_KERNEL_DONE) {
            msix_notify(pdev, GPGPU_MSIX_VEC_KERNEL);
        }
        if (events & GPGPU_IRQ_DMA_DONE) {
            msix_notify(pdev, GPGPU_MSIX_VEC_DMA);
        }
        if (events & GPGPU_IRQ_ERROR) {
            msix_notify(pdev, GPGPU_MSIX_VEC_ERROR);
        }
    } else if (msi_enabled(pdev) && events) {
        msi_notify(pdev, 0);
    }
}

static void gpgpu_raise_irq(GPGPUState *s, uint32_t events)
{
    s->irq_status |= events;
    gpgpu_update_irq(s, events);
}

static void gpgpu_set_error(GPGPUState *s, uint32_t error)
{
    s->error_status |= error;
    s->global_status |= GPGPU_STATUS_ERROR;
    gpgpu_raise_irq(s, GPGPU_IRQ_ERROR);
}

static void gpgpu_reset_state(GPGPUState *s)
{
    s->global_ctrl = 0;
    s->global_status = GPGPU_STATUS_READY;
    s->error_status = 0;
    s->irq_enable = 0;
    s->irq_status = 0;
    memset(&s->kernel, 0, sizeof(s->kernel));
    memset(&s->dma, 0, sizeof(s->dma));
    memset(&s->simt, 0, sizeof(s->simt));
    timer_del(s->dma_timer);
    pci_set_irq(PCI_DEVICE(s), 0);
}

static uint64_t gpgpu_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    GPGPUState *s = opaque;

    (void)size;

    switch (addr) {
    case GPGPU_REG_DEV_ID:
        return GPGPU_DEV_ID_VALUE;
    case GPGPU_REG_DEV_VERSION:
        return GPGPU_DEV_VERSION_VALUE;
    case GPGPU_REG_DEV_CAPS:
        return (s->num_cus & 0xff) |
               ((s->warps_per_cu & 0xff) << 8) |
               ((s->warp_size & 0xff) << 16);
    case GPGPU_REG_VRAM_SIZE_LO:
        return (uint32_t)s->vram_size;
    case GPGPU_REG_VRAM_SIZE_HI:
        return (uint32_t)(s->vram_size >> 32);
    case GPGPU_REG_GLOBAL_CTRL:
        return s->global_ctrl;
    case GPGPU_REG_GLOBAL_STATUS:
        return s->global_status;
    case GPGPU_REG_ERROR_STATUS:
        return s->error_status;
    case GPGPU_REG_IRQ_ENABLE:
        return s->irq_enable;
    case GPGPU_REG_IRQ_STATUS:
        return s->irq_status;
    case GPGPU_REG_KERNEL_ADDR_LO:
        return (uint32_t)s->kernel.kernel_addr;
    case GPGPU_REG_KERNEL_ADDR_HI:
        return (uint32_t)(s->kernel.kernel_addr >> 32);
    case GPGPU_REG_KERNEL_ARGS_LO:
        return (uint32_t)s->kernel.kernel_args;
    case GPGPU_REG_KERNEL_ARGS_HI:
        return (uint32_t)(s->kernel.kernel_args >> 32);
    case GPGPU_REG_GRID_DIM_X:
        return s->kernel.grid_dim[0];
    case GPGPU_REG_GRID_DIM_Y:
        return s->kernel.grid_dim[1];
    case GPGPU_REG_GRID_DIM_Z:
        return s->kernel.grid_dim[2];
    case GPGPU_REG_BLOCK_DIM_X:
        return s->kernel.block_dim[0];
    case GPGPU_REG_BLOCK_DIM_Y:
        return s->kernel.block_dim[1];
    case GPGPU_REG_BLOCK_DIM_Z:
        return s->kernel.block_dim[2];
    case GPGPU_REG_SHARED_MEM_SIZE:
        return s->kernel.shared_mem_size;
    case GPGPU_REG_DMA_SRC_LO:
        return (uint32_t)s->dma.src_addr;
    case GPGPU_REG_DMA_SRC_HI:
        return (uint32_t)(s->dma.src_addr >> 32);
    case GPGPU_REG_DMA_DST_LO:
        return (uint32_t)s->dma.dst_addr;
    case GPGPU_REG_DMA_DST_HI:
        return (uint32_t)(s->dma.dst_addr >> 32);
    case GPGPU_REG_DMA_SIZE:
        return s->dma.size;
    case GPGPU_REG_DMA_CTRL:
        return s->dma.ctrl;
    case GPGPU_REG_DMA_STATUS:
        return s->dma.status;
    case GPGPU_REG_THREAD_ID_X:
        return s->simt.thread_id[0];
    case GPGPU_REG_THREAD_ID_Y:
        return s->simt.thread_id[1];
    case GPGPU_REG_THREAD_ID_Z:
        return s->simt.thread_id[2];
    case GPGPU_REG_BLOCK_ID_X:
        return s->simt.block_id[0];
    case GPGPU_REG_BLOCK_ID_Y:
        return s->simt.block_id[1];
    case GPGPU_REG_BLOCK_ID_Z:
        return s->simt.block_id[2];
    case GPGPU_REG_WARP_ID:
        return s->simt.warp_id;
    case GPGPU_REG_LANE_ID:
        return s->simt.lane_id;
    case GPGPU_REG_THREAD_MASK:
        return s->simt.thread_mask;
    default:
        return 0;
    }
}

static void gpgpu_dispatch_kernel(GPGPUState *s)
{
    int ret;

    if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) ||
        (s->global_status & GPGPU_STATUS_BUSY) ||
        !s->kernel.grid_dim[0] || !s->kernel.grid_dim[1] ||
        !s->kernel.grid_dim[2] || !s->kernel.block_dim[0] ||
        !s->kernel.block_dim[1] || !s->kernel.block_dim[2] ||
        s->kernel.shared_mem_size ||
        (s->kernel.kernel_addr & 3) ||
        (s->kernel.kernel_args & 3) ||
        s->vram_size < sizeof(uint32_t) ||
        s->kernel.kernel_addr > s->vram_size - sizeof(uint32_t) ||
        s->kernel.kernel_args > s->vram_size - sizeof(uint32_t)) {
        gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
        return;
    }

    s->global_status |= GPGPU_STATUS_BUSY;
    ret = gpgpu_core_exec_kernel(s);
    s->global_status &= ~GPGPU_STATUS_BUSY;
    s->global_status |= GPGPU_STATUS_READY;

    if (ret < 0) {
        gpgpu_set_error(s, GPGPU_ERR_KERNEL_FAULT);
    } else {
        gpgpu_raise_irq(s, GPGPU_IRQ_KERNEL_DONE);
    }
}

static void gpgpu_start_dma(GPGPUState *s, uint32_t ctrl)
{
    uint64_t vram_addr;
    uint64_t host_addr;
    MemTxResult result;

    if (s->dma.status & GPGPU_DMA_BUSY) {
        gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
        return;
    }

    s->dma.ctrl = ctrl & (GPGPU_DMA_START | GPGPU_DMA_DIR_FROM_VRAM |
                          GPGPU_DMA_IRQ_ENABLE);
    if (!(ctrl & GPGPU_DMA_START)) {
        return;
    }
    s->dma.ctrl &= ~GPGPU_DMA_START;
    vram_addr = ctrl & GPGPU_DMA_DIR_FROM_VRAM ? s->dma.src_addr :
                                               s->dma.dst_addr;
    host_addr = ctrl & GPGPU_DMA_DIR_FROM_VRAM ? s->dma.dst_addr :
                                               s->dma.src_addr;

    if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) || !s->dma.size ||
        vram_addr >= s->vram_size ||
        s->dma.size > s->vram_size - vram_addr ||
        host_addr > UINT64_MAX - (s->dma.size - 1)) {
        s->dma.status = GPGPU_DMA_ERROR;
        gpgpu_set_error(s, GPGPU_ERR_DMA_FAULT);
        return;
    }

    s->dma.status = GPGPU_DMA_BUSY;
    if (ctrl & GPGPU_DMA_DIR_FROM_VRAM) {
        result = pci_dma_write(PCI_DEVICE(s), host_addr,
                               s->vram_ptr + vram_addr, s->dma.size);
    } else {
        result = pci_dma_read(PCI_DEVICE(s), host_addr,
                              s->vram_ptr + vram_addr, s->dma.size);
    }
    if (result != MEMTX_OK) {
        s->dma.status = GPGPU_DMA_ERROR;
        gpgpu_set_error(s, GPGPU_ERR_DMA_FAULT);
        return;
    }

    /* Data moves now; the completion event has a fixed 1 ms latency. */
    timer_mod(s->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
}

static void gpgpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    GPGPUState *s = opaque;

    (void)size;

    if (addr >= GPGPU_REG_DMA_SRC_LO && addr <= GPGPU_REG_DMA_SIZE &&
        (s->dma.status & GPGPU_DMA_BUSY)) {
        gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
        return;
    }

    switch (addr) {
    case GPGPU_REG_GLOBAL_CTRL:
        if (val & GPGPU_CTRL_RESET) {
            gpgpu_reset_state(s);
        } else {
            s->global_ctrl = val & GPGPU_CTRL_ENABLE;
        }
        break;
    case GPGPU_REG_ERROR_STATUS:
        s->error_status &= ~(uint32_t)val;
        if (!s->error_status) {
            s->global_status &= ~GPGPU_STATUS_ERROR;
        }
        break;
    case GPGPU_REG_IRQ_ENABLE: {
        uint32_t old = s->irq_enable;

        s->irq_enable = val & (GPGPU_IRQ_KERNEL_DONE |
                              GPGPU_IRQ_DMA_DONE |
                              GPGPU_IRQ_ERROR);
        gpgpu_update_irq(s, s->irq_enable & ~old);
        break;
    }
    case GPGPU_REG_IRQ_ACK:
        s->irq_status &= ~(uint32_t)val;
        gpgpu_update_irq(s, 0);
        break;
    case GPGPU_REG_KERNEL_ADDR_LO:
        s->kernel.kernel_addr =
            (s->kernel.kernel_addr & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case GPGPU_REG_KERNEL_ADDR_HI:
        s->kernel.kernel_addr = (s->kernel.kernel_addr & 0xffffffffULL) |
                               ((uint64_t)(uint32_t)val << 32);
        break;
    case GPGPU_REG_KERNEL_ARGS_LO:
        s->kernel.kernel_args =
            (s->kernel.kernel_args & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case GPGPU_REG_KERNEL_ARGS_HI:
        s->kernel.kernel_args = (s->kernel.kernel_args & 0xffffffffULL) |
                               ((uint64_t)(uint32_t)val << 32);
        break;
    case GPGPU_REG_GRID_DIM_X:
        s->kernel.grid_dim[0] = (uint32_t)val;
        break;
    case GPGPU_REG_GRID_DIM_Y:
        s->kernel.grid_dim[1] = (uint32_t)val;
        break;
    case GPGPU_REG_GRID_DIM_Z:
        s->kernel.grid_dim[2] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_DIM_X:
        s->kernel.block_dim[0] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_DIM_Y:
        s->kernel.block_dim[1] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_DIM_Z:
        s->kernel.block_dim[2] = (uint32_t)val;
        break;
    case GPGPU_REG_SHARED_MEM_SIZE:
        s->kernel.shared_mem_size = val;
        break;
    case GPGPU_REG_DMA_SRC_LO:
        s->dma.src_addr = (s->dma.src_addr & 0xffffffff00000000ULL) |
                         (uint32_t)val;
        break;
    case GPGPU_REG_DMA_SRC_HI:
        s->dma.src_addr = (s->dma.src_addr & 0xffffffffULL) |
                         ((uint64_t)(uint32_t)val << 32);
        break;
    case GPGPU_REG_DMA_DST_LO:
        s->dma.dst_addr = (s->dma.dst_addr & 0xffffffff00000000ULL) |
                         (uint32_t)val;
        break;
    case GPGPU_REG_DMA_DST_HI:
        s->dma.dst_addr = (s->dma.dst_addr & 0xffffffffULL) |
                         ((uint64_t)(uint32_t)val << 32);
        break;
    case GPGPU_REG_DMA_SIZE:
        s->dma.size = (uint32_t)val;
        break;
    case GPGPU_REG_DMA_CTRL:
        gpgpu_start_dma(s, val);
        break;
    case GPGPU_REG_THREAD_ID_X:
        s->simt.thread_id[0] = (uint32_t)val;
        break;
    case GPGPU_REG_THREAD_ID_Y:
        s->simt.thread_id[1] = (uint32_t)val;
        break;
    case GPGPU_REG_THREAD_ID_Z:
        s->simt.thread_id[2] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_ID_X:
        s->simt.block_id[0] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_ID_Y:
        s->simt.block_id[1] = (uint32_t)val;
        break;
    case GPGPU_REG_BLOCK_ID_Z:
        s->simt.block_id[2] = (uint32_t)val;
        break;
    case GPGPU_REG_WARP_ID:
        s->simt.warp_id = (uint32_t)val;
        break;
    case GPGPU_REG_LANE_ID:
        s->simt.lane_id = (uint32_t)val;
        break;
    case GPGPU_REG_THREAD_MASK:
        s->simt.thread_mask = (uint32_t)val;
        break;
    case GPGPU_REG_DISPATCH:
        gpgpu_dispatch_kernel(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps gpgpu_ctrl_ops = {
    .read = gpgpu_ctrl_read,
    .write = gpgpu_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t gpgpu_vram_read(void *opaque, hwaddr addr, unsigned size)
{
    GPGPUState *s = opaque;

    if (addr >= s->vram_size || size > s->vram_size - addr) {
        gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
        return 0;
    }

    return ldn_le_p(s->vram_ptr + addr, size);
}

static void gpgpu_vram_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    GPGPUState *s = opaque;

    if (addr >= s->vram_size || size > s->vram_size - addr) {
        gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
        return;
    }

    stn_le_p(s->vram_ptr + addr, size, val);
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

static uint64_t gpgpu_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void gpgpu_doorbell_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)val;
    (void)size;
}

static const MemoryRegionOps gpgpu_doorbell_ops = {
    .read = gpgpu_doorbell_read,
    .write = gpgpu_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void gpgpu_dma_complete(void *opaque)
{
    GPGPUState *s = opaque;

    s->dma.status = GPGPU_DMA_COMPLETE;
    if (s->dma.ctrl & GPGPU_DMA_IRQ_ENABLE) {
        gpgpu_raise_irq(s, GPGPU_IRQ_DMA_DONE);
    }
}

static void gpgpu_realize(PCIDevice *pdev, Error **errp)
{
    GPGPUState *s = GPGPU(pdev);
    uint8_t *pci_conf = pdev->config;

    if (s->warp_size != GPGPU_WARP_SIZE || s->vram_size < 4096 ||
        s->vram_size > GPGPU_CORE_CTRL_BASE ||
        !is_power_of_2(s->vram_size)) {
        error_setg(errp, "GPGPU: warp_size must be 32; vram_size must be a "
                   "power of two between 4 KiB and 2 GiB");
        return;
    }

    pci_config_set_interrupt_pin(pci_conf, 1);

    s->vram_ptr = g_malloc0(s->vram_size);
    if (!s->vram_ptr) {
        error_setg(errp, "GPGPU: failed to allocate VRAM");
        return;
    }

    /* BAR 0: control registers */
    memory_region_init_io(&s->ctrl_mmio, OBJECT(s), &gpgpu_ctrl_ops, s,
                          "gpgpu-ctrl", GPGPU_CTRL_BAR_SIZE);
    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->ctrl_mmio);

    /* BAR 2: VRAM */
    memory_region_init_io(&s->vram, OBJECT(s), &gpgpu_vram_ops, s,
                          "gpgpu-vram", s->vram_size);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->vram);

    /* BAR 4: doorbell registers */
    memory_region_init_io(&s->doorbell_mmio, OBJECT(s), &gpgpu_doorbell_ops, s,
                          "gpgpu-doorbell", GPGPU_DOORBELL_BAR_SIZE);
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->doorbell_mmio);

    if (msix_init(pdev, GPGPU_MSIX_VECTORS,
                  &s->ctrl_mmio, 0, 0xFE000,
                  &s->ctrl_mmio, 0, 0xFF000,
                  0, errp)) {
        g_free(s->vram_ptr);
        return;
    }

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        msix_uninit(pdev, &s->ctrl_mmio, &s->ctrl_mmio);
        g_free(s->vram_ptr);
        return;
    }
    for (unsigned int i = 0; i < GPGPU_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }

    s->dma_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, gpgpu_dma_complete, s);

    s->global_status = GPGPU_STATUS_READY;
}

static void gpgpu_exit(PCIDevice *pdev)
{
    GPGPUState *s = GPGPU(pdev);

    timer_free(s->dma_timer);
    g_free(s->vram_ptr);
    msix_uninit(pdev, &s->ctrl_mmio, &s->ctrl_mmio);
    msi_uninit(pdev);
}

static void gpgpu_reset(DeviceState *dev)
{
    GPGPUState *s = GPGPU(dev);

    gpgpu_reset_state(s);
    if (s->vram_ptr) {
        memset(s->vram_ptr, 0, s->vram_size);
    }
    msix_reset(PCI_DEVICE(s));
    msi_reset(PCI_DEVICE(s));
}

static const Property gpgpu_properties[] = {
    DEFINE_PROP_UINT32("num_cus", GPGPUState, num_cus,
                       GPGPU_DEFAULT_NUM_CUS),
    DEFINE_PROP_UINT32("warps_per_cu", GPGPUState, warps_per_cu,
                       GPGPU_DEFAULT_WARPS_PER_CU),
    DEFINE_PROP_UINT32("warp_size", GPGPUState, warp_size,
                       GPGPU_DEFAULT_WARP_SIZE),
    DEFINE_PROP_UINT64("vram_size", GPGPUState, vram_size,
                       GPGPU_DEFAULT_VRAM_SIZE),
};

static const VMStateDescription vmstate_gpgpu = {
    .name = "gpgpu",
    /* VRAM and in-flight operations are intentionally not migrated yet. */
    .unmigratable = true,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, GPGPUState),
        VMSTATE_UINT32(global_ctrl, GPGPUState),
        VMSTATE_UINT32(global_status, GPGPUState),
        VMSTATE_UINT32(error_status, GPGPUState),
        VMSTATE_UINT32(irq_enable, GPGPUState),
        VMSTATE_UINT32(irq_status, GPGPUState),
        VMSTATE_END_OF_LIST()
    }
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
    .name          = TYPE_GPGPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GPGPUState),
    .class_init    = gpgpu_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void gpgpu_register_types(void)
{
    type_register_static(&gpgpu_type_info);
}

type_init(gpgpu_register_types)
