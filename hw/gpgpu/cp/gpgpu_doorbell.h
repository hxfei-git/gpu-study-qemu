/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 处理 BAR4 访问并交给 HQD 记录提示，固件继续自行轮询队列。 */
#ifndef HW_CP_GPGPU_DOORBELL_H
#define HW_CP_GPGPU_DOORBELL_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/**
 * gpgpu_doorbell_init() - 建立 BAR4 门铃访问窗口
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 在设备 realize 中调用，MemoryRegion 的所属对象仍为设备。
 */
void gpgpu_doorbell_init(GPGPUState *s);

/**
 * struct GPGPUDoorbellState - Doorbell 模块拥有的 PCI 窗口
 * @mmio: BAR4 的 MemoryRegion，通知状态由 HQD 保存。
 */
typedef struct GPGPUDoorbellState {
	MemoryRegion mmio;
} GPGPUDoorbellState;

#endif /* HW_CP_GPGPU_DOORBELL_H */
