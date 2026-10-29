// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA：描述符寄存器、同步搬运与延迟完成
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/irq/irq.h"
#include "dma.h"
#include "phys.h"
#include "vram.h"

/* 数据搬运结束到发布 COMPLETE 的虚拟时间延迟，单位毫秒。 */
#define DMA_COMPLETION_DELAY_MS 1

/* 定时器回调：发布 COMPLETE，按描述符要求发完成中断。 */
static void dma_complete(void *opaque)
{
	GPGPUState *s = opaque;

	s->dma.status = GPGPU_DMA_COMPLETE;
	if (s->dma.ctrl & GPGPU_DMA_IRQ_ENABLE)
		gpgpu_raise_irq(s, GPGPU_IRQ_DMA_DONE);
}

static void dma_fail(GPGPUState *s)
{
	s->dma.status = GPGPU_DMA_ERROR;
	gpgpu_set_error(s, GPGPU_ERR_DMA_FAULT);
}

/*
 * dma_start() - 处理一次 DMA_CTRL 写入
 *
 * 1. 锁存方向与中断位，未带 START 时到此为止；
 * 2. 检查描述符：设备已使能、长度非零、VRAM 端完整在 VRAM 内、Host 端可路由
 *    且不在 VRAM 内；
 * 3. 映射 VRAM 端，与 Host 端直接搬运；失败保留已写入部分；
 * 4. 安排定时器延迟发布 COMPLETE。
 */
static void dma_start(GPGPUState *s, uint32_t ctrl)
{
	GPGPUDMAState *d = &s->dma;
	bool to_host = ctrl & GPGPU_DMA_DIR_FROM_VRAM;
	uint64_t vram_addr = to_host ? d->src_addr : d->dst_addr;
	uint64_t host_addr = to_host ? d->dst_addr : d->src_addr;
	MemTxResult result;
	void *vram;

	/* 1. 控制位。 */
	d->ctrl = ctrl & (GPGPU_DMA_DIR_FROM_VRAM | GPGPU_DMA_IRQ_ENABLE);
	if (!(ctrl & GPGPU_DMA_START))
		return;

	/* 2. 描述符。 */
	if (!gpgpu_enabled(s) || !d->size || !gpgpu_vram_contains(s, vram_addr, d->size) ||
	    host_addr < gpgpu_vram_size(s) || !gpgpu_phys_contains(s, host_addr, d->size)) {
		dma_fail(s);
		return;
	}

	/* 3. 搬运：VRAM 端只读（D2H）或只写（H2D）。 */
	d->status = GPGPU_DMA_BUSY;
	vram = gpgpu_vram_map(s, vram_addr, d->size, !to_host);
	result = gpgpu_phys_rw(s, host_addr, vram, d->size, to_host);
	gpgpu_vram_unmap(s, vram, d->size, !to_host);
	if (result != MEMTX_OK) {
		dma_fail(s);
		return;
	}

	/* 4. 延迟完成。 */
	timer_mod(d->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DMA_COMPLETION_DELAY_MS);
}

uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr reg)
{
	GPGPUDMAState *d = &s->dma;

	switch (reg) {
	case GPGPU_REG_DMA_SRC_LO:
	case GPGPU_REG_DMA_SRC_HI:
		return gpgpu_reg64_read(d->src_addr, reg - GPGPU_REG_DMA_SRC_LO);
	case GPGPU_REG_DMA_DST_LO:
	case GPGPU_REG_DMA_DST_HI:
		return gpgpu_reg64_read(d->dst_addr, reg - GPGPU_REG_DMA_DST_LO);
	case GPGPU_REG_DMA_SIZE:
		return d->size;
	case GPGPU_REG_DMA_CTRL:
		return d->ctrl;
	case GPGPU_REG_DMA_STATUS:
		return d->status;
	default:
		return 0;
	}
}

void gpgpu_dma_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUDMAState *d = &s->dma;

	/* BUSY 期间描述符和 CTRL 都不能改写。 */
	if (d->status == GPGPU_DMA_BUSY && reg <= GPGPU_REG_DMA_CTRL) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	switch (reg) {
	case GPGPU_REG_DMA_SRC_LO:
	case GPGPU_REG_DMA_SRC_HI:
		gpgpu_reg64_write(&d->src_addr, reg - GPGPU_REG_DMA_SRC_LO, value);
		break;
	case GPGPU_REG_DMA_DST_LO:
	case GPGPU_REG_DMA_DST_HI:
		gpgpu_reg64_write(&d->dst_addr, reg - GPGPU_REG_DMA_DST_LO, value);
		break;
	case GPGPU_REG_DMA_SIZE:
		d->size = value;
		break;
	case GPGPU_REG_DMA_CTRL:
		dma_start(s, value);
		break;
	default:
		break;
	}
}

bool gpgpu_dma_busy(GPGPUState *s)
{
	return s->dma.status == GPGPU_DMA_BUSY;
}

void gpgpu_dma_init(GPGPUState *s)
{
	s->dma.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dma_complete, s);
}

void gpgpu_dma_reset(GPGPUState *s)
{
	QEMUTimer *timer = s->dma.timer;

	timer_del(timer);
	s->dma = (GPGPUDMAState) { .timer = timer };
}

void gpgpu_dma_cleanup(GPGPUState *s)
{
	timer_free(s->dma.timer);
}
