/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"
#include "gpgpu.h"

/* DMA 描述符寄存器、数据搬运和延迟完成通知。 */

static void gpgpu_dma_complete(void *opaque)
{
    GPGPUState *s = opaque;

    s->dma.status = GPGPU_DMA_COMPLETE;
    if (s->dma.ctrl & GPGPU_DMA_IRQ_ENABLE) {
        gpgpu_raise_irq(s, GPGPU_IRQ_DMA_DONE);
    }
}

static void gpgpu_start_dma(GPGPUState *s, GPGPUDMAControl ctrl)
{
    uint64_t vram_addr;
    uint64_t host_addr;
    MemTxResult result;

    if (s->dma.status == GPGPU_DMA_BUSY) {
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
        !gpgpu_vram_contains(s, vram_addr, s->dma.size) ||
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

    /* 此时完成数据搬运；完成事件固定延迟 1 ms 触发。 */
    timer_mod(s->dma.timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
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
    case GPGPU_REG_DMA_CTRL:
        return s->dma.ctrl;
    case GPGPU_REG_DMA_STATUS:
        return s->dma.status;
    default:
        return 0;
    }
}

void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
    /* BUSY 期间写描述符不能改变尚未完成的传输。 */
    if (addr <= GPGPU_REG_DMA_SIZE && s->dma.status == GPGPU_DMA_BUSY) {
        gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
        return;
    }

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
    case GPGPU_REG_DMA_CTRL:
        gpgpu_start_dma(s, val);
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
