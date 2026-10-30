/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DMA：Host RAM 与 VRAM 之间的描述符搬运
 *
 * 驱动写好源、目的和长度后写 DMA_CTRL.START。设备在写寄存器的同一次
 * 调用里同步完成数据搬运，再延迟 1 ms 发布 COMPLETE 和完成中断，模拟
 * 真实引擎的完成时延。Host 端地址是 GPU 物理地址，经 Outbound 换算为 IOVA。
 */
#ifndef HW_GPGPU_MEM_DMA_H
#define HW_GPGPU_MEM_DMA_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "qemu/typedefs.h"

/**
 * struct GPGPUDMAState - 描述符与完成状态
 * @src_addr: 源地址：H2D 为 Host 端 GPU 物理地址，D2H 为 VRAM 偏移。
 * @dst_addr: 目的地址：H2D 为 VRAM 偏移，D2H 为 Host 端 GPU 物理地址。
 * @size: 传输字节数。
 * @ctrl: DMA_CTRL 中锁存的方向与中断使能位。
 * @status: GPGPU_DMA_IDLE/BUSY/COMPLETE/ERROR。
 * @timer: 延迟发布完成的定时器。
 */
typedef struct GPGPUDMAState {
	uint64_t src_addr;
	uint64_t dst_addr;
	uint32_t size;
	uint32_t ctrl;
	uint32_t status;
	QEMUTimer *timer;
} GPGPUDMAState;

/* DMA 寄存器组（BAR0 0x400）。BUSY 期间改写描述符锁存 INVALID_CMD。 */
uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr reg);
void gpgpu_dma_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* DMA 引擎是否在搬运或等待发布完成。 */
bool gpgpu_dma_busy(GPGPUState *s);

void gpgpu_dma_init(GPGPUState *s);
void gpgpu_dma_reset(GPGPUState *s);
void gpgpu_dma_cleanup(GPGPUState *s);

#endif /* HW_GPGPU_MEM_DMA_H */
