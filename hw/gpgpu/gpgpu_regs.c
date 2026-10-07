/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu.h"

/* BAR0 访问分发、全局控制与教学用 SIMT 寄存器存储。 */

void gpgpu_reset_state(GPGPUState *s)
{
    s->global_ctrl = 0;
    s->global_status = GPGPU_STATUS_READY;
    s->error_status = 0;
    gpgpu_irq_clear(s);
    memset(&s->kernel, 0, sizeof(s->kernel));
    memset(&s->simt, 0, sizeof(s->simt));
    gpgpu_dma_reset(s);
}

static uint64_t gpgpu_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    GPGPUState *s = opaque;

    /* 每个 0x100 字节的寄存器组对应一个设备模块。 */
    switch (addr & ~0xffULL) {
    case GPGPU_REG_IRQ_ENABLE:
        return gpgpu_irq_read(s, addr);
    case GPGPU_REG_KERNEL_ADDR_LO:
        return gpgpu_kernel_read(s, addr);
    case GPGPU_REG_DMA_SRC_LO:
        return gpgpu_dma_read(s, addr);
    }

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

static void gpgpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    GPGPUState *s = opaque;

    switch (addr & ~0xffULL) {
    case GPGPU_REG_IRQ_ENABLE:
        gpgpu_irq_write(s, addr, val);
        return;
    case GPGPU_REG_KERNEL_ADDR_LO:
        gpgpu_kernel_write(s, addr, val);
        return;
    case GPGPU_REG_DMA_SRC_LO:
        gpgpu_dma_write(s, addr, val);
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
    default:
        break;
    }
}

const MemoryRegionOps gpgpu_ctrl_ops = {
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
