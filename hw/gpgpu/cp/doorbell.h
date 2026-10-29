/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Doorbell：BAR4 的通知入口
 *
 * Host 发布包后向队列的 Doorbell 写入完整 64 位值（低 32 位为已发布编号，
 * 高 32 位为 0）。固件按轮询读取共享 W 与 Header，不依赖 Doorbell 的内容，
 * 因此设备只检查写入形式，不保存写入值。
 */
#ifndef HW_GPGPU_CP_DOORBELL_H
#define HW_GPGPU_CP_DOORBELL_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/**
 * struct GPGPUDoorbellState - Doorbell 模块状态
 * @bar: BAR4 的 MMIO 窗口。
 */
typedef struct GPGPUDoorbellState {
	MemoryRegion bar;
} GPGPUDoorbellState;

/* 建立并注册 BAR4。 */
void gpgpu_doorbell_init(GPGPUState *s);

#endif /* HW_GPGPU_CP_DOORBELL_H */
