/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 描述符 DMA 的寄存器和延迟完成通知；Host 访存由独立模块提供。 */
#ifndef HW_MEMORY_GPGPU_DMA_H
#define HW_MEMORY_GPGPU_DMA_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "qemu/typedefs.h"

/**
 * struct GPGPUDMAState - 保存 DMA 描述符及延迟完成通知状态
 * @src_addr: 源地址；H2D 为 Host DMA 地址，D2H 为 VRAM 偏移。
 * @dst_addr: 目的地址；H2D 为 VRAM 偏移，D2H 为 Host DMA 地址。
 * @size: 客体写入的传输字节数。
 * @ctrl: DMA_CTRL 标志，START 位自动清除。
 * @status: 只读：IDLE/BUSY/COMPLETE/ERROR。
 * @timer: DMA 模块的完成回调定时器。
 */
typedef struct GPGPUDMAState {
	/*
	 * TO_VRAM：src 为客体 DMA 地址，dst 为 VRAM 字节偏移；
	 * FROM_VRAM 的含义相反。两者均不是 QEMU 进程指针。
	 */
	uint64_t src_addr;
	uint64_t dst_addr;
	uint32_t size;
	uint32_t ctrl;
	uint32_t status;
	QEMUTimer *timer;
} GPGPUDMAState;

/**
 * gpgpu_dma_read() - 读取搬运描述符或完成状态
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 *
 * Return: 寄存器值，未定义偏移返回零。
 */
uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_dma_write() - 配置或启动一次描述符搬运
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 * @val: 本次写入的寄存器值。
 *
 * BUSY 时拒绝覆盖；搬运同步完成后延迟发布完成事件。
 */
void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val);

/**
 * gpgpu_dma_init() - 创建完成通知定时器
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_dma_init(GPGPUState *s);

/**
 * gpgpu_dma_reset() - 取消完成通知并清零描述符
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 恢复 IDLE，调用前设备已停止提交。
 */
void gpgpu_dma_reset(GPGPUState *s);

/**
 * gpgpu_dma_cleanup() - 释放完成通知定时器
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 由设备退出路径调用。
 */
void gpgpu_dma_cleanup(GPGPUState *s);

#endif /* HW_MEMORY_GPGPU_DMA_H */
