/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 计算执行器内部：线程、warp 与任务状态
 *
 * 只供 compute/ 目录内的调度器、解释器和 lane 访存使用。
 */
#ifndef HW_GPGPU_COMPUTE_INTERNAL_H
#define HW_GPGPU_COMPUTE_INTERNAL_H

#include "fpu/softfloat.h"
#include "compute.h"

typedef struct GPGPUState GPGPUState;

/* RV32 整数与浮点寄存器数。 */
#define GPGPU_NUM_REGS 32
/* RV32 的 a0 寄存器编号；启动时保存参数块地址。 */
#define GPGPU_REG_A0 10
/* 启动 SGPR 个数：blockIdx、blockDim、gridDim 各 X/Y/Z。 */
#define GPGPU_STARTUP_SGPRS 9

/* mhartid 编码：[31:13] 线性工作组号，[12:5] 组内 warp 号，[4:0] lane 号。 */
#define GPGPU_MHARTID(block, warp, lane) (((block) << 13) | ((warp) << 5) | ((lane) & 0x1f))

/**
 * struct GPGPULane - 一个 RV32 线程
 * @gpr: 整数寄存器 x0～x31，x0 恒为 0。
 * @fpr: 浮点寄存器 f0～f31 的原始位。
 * @thread_id: 只读启动值 threadIdx.x/y/z，经 CSR 读取。
 * @pc: 当前指令地址，与 kernel_addr 同一地址空间。
 * @mhartid: 只读线程身份，见 GPGPU_MHARTID()。
 * @fcsr: fflags[4:0] 与 frm[7:5]。
 * @fp_status: SoftFloat 舍入与异常状态。
 * @active: 尚未执行 EBREAK。
 */
typedef struct GPGPULane {
	uint32_t gpr[GPGPU_NUM_REGS];
	uint32_t fpr[GPGPU_NUM_REGS];
	uint32_t thread_id[3];
	uint32_t pc;
	uint32_t mhartid;
	uint32_t fcsr;
	float_status fp_status;
	bool active;
} GPGPULane;

/**
 * struct GPGPUWarp - 一组同时推进的线程
 * @lanes: 各 lane 独立保存寄存器与 PC。
 * @startup_sgpr: 只读启动值 blockIdx[3]、blockDim[3]、gridDim[3]。
 * @active_mask: 第 N 位为 1 表示 lane N 仍需执行。
 */
typedef struct GPGPUWarp {
	GPGPULane lanes[GPGPU_WARP_SIZE];
	uint32_t startup_sgpr[GPGPU_STARTUP_SGPRS];
	uint32_t active_mask;
} GPGPUWarp;

/**
 * struct GPGPUExecContext - 解释器与 lane 访存共用的任务上下文
 * @device: 设备，任务寿命内有效。
 * @params: 派发配置快照。
 * @error: 当前指令失败时写入的 GpgpuDispatchError。
 */
typedef struct GPGPUExecContext {
	GPGPUState *device;
	GPGPUKernelParams params;
	uint32_t error;
} GPGPUExecContext;

/* 记录当前指令的失败原因，返回 false 以便调用者直接返回。 */
static inline bool gpgpu_exec_fail(GPGPUExecContext *ctx, uint32_t error)
{
	ctx->error = error;
	return false;
}

#endif /* HW_GPGPU_COMPUTE_INTERNAL_H */
