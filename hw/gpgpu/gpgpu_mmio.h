/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 只分发 Host 与 CP 寄存器访问，并保存独立的教学 SIMT 读回值。 */
#ifndef HW_GPGPU_MMIO_H
#define HW_GPGPU_MMIO_H

#include "gpgpu.h"
#include "system/memory.h"

/**
 * struct GPGPUSIMTContext - BAR0 教学寄存器读回值，与解释器的 lane 上下文分开
 * @thread_id: threadIdx.x/y/z 的读回值。
 * @block_id: blockIdx.x/y/z 的读回值。
 * @warp_id: warp 编号的读回值。
 * @lane_id: lane 编号的读回值。
 * @thread_mask: 第 N 位对应 lane N。
 */
typedef struct GPGPUSIMTContext {
	uint32_t thread_id[3];
	uint32_t block_id[3];
	uint32_t warp_id;
	uint32_t lane_id;
	uint32_t thread_mask;
} GPGPUSIMTContext;

/* BAR0 和 CP 控制窗口的回调，opaque 均为设备实例。 */
extern const MemoryRegionOps gpgpu_ctrl_ops;
extern const MemoryRegionOps gpgpu_cp_regs_ops;

/**
 * gpgpu_mmio_reset() - 清零 BAR0 教学 SIMT 寄存器
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 不影响计算任务内部的 lane 或 warp 状态。
 */
void gpgpu_mmio_reset(GPGPUState *s);

#endif /* HW_GPGPU_MMIO_H */
