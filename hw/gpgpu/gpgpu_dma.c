/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU Educational GPGPU Device
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"
#include "gpgpu.h"

/* DMA descriptor registers, data movement and delayed completion. */

static void gpgpu_dma_complete(void *opaque)
{
    (void)opaque;
}

uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr addr)
{
    switch (addr) {
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
    default:
        return 0;
    }
}

void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case GPGPU_REG_DMA_SRC_LO:
        s->dma.src_addr = deposit64(s->dma.src_addr, 0, 32, val);
        break;
    case GPGPU_REG_DMA_SRC_HI:
        s->dma.src_addr = deposit64(s->dma.src_addr, 32, 32, val);
        break;
    case GPGPU_REG_DMA_DST_LO:
        s->dma.dst_addr = deposit64(s->dma.dst_addr, 0, 32, val);
        break;
    case GPGPU_REG_DMA_DST_HI:
        s->dma.dst_addr = deposit64(s->dma.dst_addr, 32, 32, val);
        break;
    case GPGPU_REG_DMA_SIZE:
        s->dma.size = val;
        break;
    default:
        break;
    }
}

void gpgpu_dma_init(GPGPUState *s)
{
    s->dma.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, gpgpu_dma_complete, s);
}

void gpgpu_dma_cleanup(GPGPUState *s)
{
    timer_free(s->dma.timer);
}

void gpgpu_dma_reset(GPGPUState *s)
{
    timer_del(s->dma.timer);
    s->dma.src_addr = 0;
    s->dma.dst_addr = 0;
    s->dma.size = 0;
    s->dma.ctrl = 0;
    s->dma.status = GPGPU_DMA_IDLE;
}
