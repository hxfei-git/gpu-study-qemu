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

/* 描述符非法或 Host 访问失败：状态置 ERROR，并锁存全局 DMA_FAULT。 */
static void dma_fail(GPGPUState *s)
{
	s->dma.status = GPGPU_DMA_ERROR;
	gpgpu_set_error(s, GPGPU_ERR_DMA_FAULT);
}

/*
 * dma_start() - 处理一次 DMA_CTRL 写入
 *
 * 驱动先写源、目的和长度，最后写 DMA_CTRL。方向位决定哪一端是 VRAM：
 * H2D 时源在 Host、目的在 VRAM，D2H 时相反。数据在本次写寄存器的调用里
 * 同步搬完，完成状态再延迟发布：
 *
 * 1. 锁存方向与中断使能位；没有 START 位时只更新这两位，不搬运；
 * 2. 检查描述符：设备已使能、长度非零、VRAM 端完整位于 VRAM 内，
 *    Host 端不在 VRAM 范围内且落在已使能的 Outbound 窗口里。
 *    不合法时置 ERROR 并锁存 DMA_FAULT，不搬运；
 * 3. 置 BUSY，把 VRAM 端映射成一段 Host 指针，再经 gpgpu_phys_rw()
 *    与 Host 端直接互拷。Host 访问失败时置 ERROR，已写入的部分保留；
 * 4. 搬运成功后安排 1 ms 定时器，到期才发布 COMPLETE 和完成中断，
 *    模拟真实引擎的完成时延。BUSY 期间描述符不能改写。
 */
static void dma_start(GPGPUState *s, uint32_t ctrl)
{
	GPGPUDMAState *d = &s->dma;
	bool to_host = ctrl & GPGPU_DMA_DIR_FROM_VRAM;
	uint64_t vram_addr = to_host ? d->src_addr : d->dst_addr;
	uint64_t host_addr = to_host ? d->dst_addr : d->src_addr;
	MemTxResult result;
	void *vram;

	/* 1. START 不锁存，读回 DMA_CTRL 只有方向与中断使能位。 */
	d->ctrl = ctrl & (GPGPU_DMA_DIR_FROM_VRAM | GPGPU_DMA_IRQ_ENABLE);
	if (!(ctrl & GPGPU_DMA_START))
		return;

	/* 2. 两端范围检查，任一项不满足即失败。 */
	if (!gpgpu_enabled(s) || !d->size || !gpgpu_vram_contains(s, vram_addr, d->size) ||
	    host_addr < gpgpu_vram_size(s) || !gpgpu_phys_contains(s, host_addr, d->size)) {
		dma_fail(s);
		return;
	}

	/*
	 * 3. VRAM 端映射：D2H 时只读，H2D 时只写；Host 端读写方向与之相反。
	 *    Host 端走 Outbound 时经 vfio-user DMA，可能暂时释放主锁。
	 */
	d->status = GPGPU_DMA_BUSY;
	vram = gpgpu_vram_map(s, vram_addr, d->size, !to_host);
	result = gpgpu_phys_rw(s, host_addr, vram, d->size, to_host);
	gpgpu_vram_unmap(s, vram, d->size, !to_host);
	if (result != MEMTX_OK) {
		dma_fail(s);
		return;
	}

	/* 4. 状态保持 BUSY，直到定时器回调 dma_complete() 发布 COMPLETE。 */
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

	/* BUSY 期间描述符和 CTRL 都不能改写；它们的偏移都不大于 DMA_CTRL。 */
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
