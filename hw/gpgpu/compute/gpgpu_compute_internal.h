/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef HW_GPGPU_COMPUTE_INTERNAL_H
#define HW_GPGPU_COMPUTE_INTERNAL_H

#include "fpu/softfloat.h"
#include "gpgpu_exec_types.h"

typedef struct GPGPUState GPGPUState;

/* RV32 通用寄存器和浮点寄存器的数量。 */
#define GPGPU_NUM_REGS 32
#define GPGPU_NUM_FREGS 32
/* mhartid 依次保存工作组、组内 warp 和 lane 编号。 */
#define MHARTID_ENCODE(block, warp, thread) (((block) << 13) | ((warp) << 5) | ((thread) & 0x1F))

/**
 * struct GPGPULane - 单个 RV32 线程的寄存器和运行状态
 * @gpr: 通用寄存器 x0～x31，x0 保持零。
 * @fpr: 浮点寄存器 f0～f31 的原始位值。
 * @pc: 当前指令地址，按任务模式解释。
 * @mhartid: 工作组、warp 和 lane 编号组成的只读线程身份。
 * @fcsr: fflags[4:0] 与 frm[7:5]。
 * @fp_status: SoftFloat 舍入和异常状态。
 * @active: 线程是否仍需执行，EBREAK 后清除。
 */
typedef struct GPGPULane {
	uint32_t gpr[GPGPU_NUM_REGS];
	uint32_t fpr[GPGPU_NUM_FREGS];
	uint32_t pc;
	uint32_t mhartid;
	uint32_t fcsr;
	float_status fp_status;
	bool active;
} GPGPULane;

/**
 * struct GPGPUWarp - Core 当前推进的一组线程
 * @lanes: 各 lane 独立保存寄存器和 PC。
 * @active_mask: 第 N 位表示 lane N 仍需执行。
 * @thread_id_base: 本 warp 在工作组中的起始线性线程号。
 * @warp_id: 本 warp 在工作组中的编号。
 * @block_id: 所属工作组的 X/Y/Z 坐标。
 */
typedef struct GPGPUWarp {
	GPGPULane lanes[GPGPU_WARP_SIZE];
	uint32_t active_mask;
	uint32_t thread_id_base;
	uint32_t warp_id;
	uint32_t block_id[3];
} GPGPUWarp;

/**
 * struct GPGPUExecContext - ISA 与计算访存共用的任务上下文
 * @device: 借用的设备指针，任务寿命内保持有效。
 * @params: Core 持有的派发参数快照。
 * @mode: 提交时固定的执行模式，不由任务指针是否存在推断。
 * @result: ISA 或访存写入的失败结果，由 Core 返回给 Dispatch。
 */
typedef struct GPGPUExecContext {
	GPGPUState *device;
	GPGPUKernelParams params;
	GPGPUExecMode mode;
	GPGPUExecResult result;
} GPGPUExecContext;

/**
 * struct GPGPUDispatchTask - Core 持有的队列任务及分段进度
 * @exec: 固定配置、执行模式与本段结果。
 * @warp: 当前执行的 warp。
 * @block_count: 工作组总数。
 * @threads_per_block: 每工作组线程数。
 * @warp_count: 每工作组需要执行的 warp 数。
 * @block_index: 当前线性工作组号，X 维变化最快。
 * @warp_index: 当前工作组内的 warp 号。
 * @warp_cycles: 当前 warp 已执行的轮数，一轮遍历全部活跃 lane。
 * @total_cycles: 整个任务累计执行轮数，仅记录，不增设总预算限制。
 * @warp_ready: 当前 warp 已初始化，下一段应从原进度继续。
 */
struct GPGPUDispatchTask {
	GPGPUExecContext exec;
	GPGPUWarp warp;
	uint32_t block_count;
	uint32_t threads_per_block;
	uint32_t warp_count;
	uint32_t block_index;
	uint32_t warp_index;
	uint32_t warp_cycles;
	uint64_t total_cycles;
	bool warp_ready;
};

/* 记录本次失败；error 为零时保留“没有产生细分错误”的含义。 */
static inline bool gpgpu_exec_fail(GPGPUExecContext *ctx, uint32_t error)
{
	ctx->result = (GPGPUExecResult) {
		.status = GPGPU_EXEC_FAILED,
		.error = error,
		.error_valid = error != 0,
	};
	return false;
}

#endif /* HW_GPGPU_COMPUTE_INTERNAL_H */
