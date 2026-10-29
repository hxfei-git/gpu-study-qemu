// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
/*
 * RV32 工作组、warp 和执行预算推进
 */
#include "qemu/osdep.h"
#include "gpgpu_core.h"
#include "gpgpu_compute_internal.h"
#include "gpgpu_isa.h"
#include "gpgpu/gpgpu_protocol.h"

/* 先除后乘检查维度，防止乘法溢出或线程身份超出既有编码范围。 */
static bool core_set_shape(GPGPUDispatchTask *task)
{
	const GPGPUKernelParams *p = &task->exec.params;

	task->block_count = 1;
	task->threads_per_block = 1;
	for (unsigned int i = 0; i < 3; i++) {
		if (!p->grid_dim[i] || !p->block_dim[i] ||
		    p->grid_dim[i] > GPGPU_MAX_BLOCKS / task->block_count ||
		    p->block_dim[i] > GPGPU_MAX_BLOCK_THREADS / task->threads_per_block)
			return false;
		task->block_count *= p->grid_dim[i];
		task->threads_per_block *= p->block_dim[i];
	}
	if (task->block_count > GPGPU_MAX_TOTAL_THREADS / task->threads_per_block)
		return false;
	task->warp_count = DIV_ROUND_UP(task->threads_per_block, GPGPU_WARP_SIZE);
	return true;
}

/* 仅激活实际存在的线程，所有 lane 从同一个入口和初始浮点状态开始。 */
static void core_init_warp(GPGPUWarp *warp, const GPGPUKernelParams *params,
			   uint32_t thread_id_base,
			   const uint32_t block_id[3], uint32_t num_threads, uint32_t warp_id,
			   uint32_t block_id_linear)
{
	memset(warp, 0, sizeof(*warp));

	for (int i = 0; i < 3; i++) {
		warp->startup_sgpr[i] = block_id[i];
		warp->startup_sgpr[3 + i] = params->block_dim[i];
		warp->startup_sgpr[6 + i] = params->grid_dim[i];
	}

	warp->active_mask = num_threads == GPGPU_WARP_SIZE ? UINT32_MAX : (1U << num_threads) - 1;

	for (uint32_t lane_id = 0; lane_id < num_threads; lane_id++) {
		GPGPULane *lane = &warp->lanes[lane_id];
		uint32_t thread = thread_id_base + lane_id;

		lane->startup_vgpr[0] = thread % params->block_dim[0];
		lane->startup_vgpr[1] = thread / params->block_dim[0] % params->block_dim[1];
		lane->startup_vgpr[2] = thread / params->block_dim[0] / params->block_dim[1];
		lane->gpr[10] = params->kernel_args;
		lane->active = true;
		lane->pc = params->kernel_addr;
		lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id, lane_id);
		set_float_rounding_mode(float_round_nearest_even, &lane->fp_status);
		set_float_detect_tininess(float_tininess_after_rounding, &lane->fp_status);
		set_default_nan_mode(true, &lane->fp_status);
		set_float_default_nan_pattern(0x40, &lane->fp_status);
	}
}

/*
 * 一轮依次执行全部活跃 lane 的一条指令。预算按轮计数，不随活跃 lane 数变化；
 * 任务保存跨回调计数，同一 warp 分段继续时不重新初始化启动状态。
 */
static GPGPUExecResult core_exec_warp(GPGPUDispatchTask *task, uint32_t max_cycles)
{
	GPGPUExecContext *ctx = &task->exec;
	GPGPUWarp *warp = &task->warp;
	uint32_t cycles = 0;

	while (warp->active_mask) {
		if (cycles >= max_cycles)
			return (GPGPUExecResult) { .status = GPGPU_EXEC_CONTINUE };
		if (task->warp_cycles >= 100000) {
			gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_BUDGET);
			return ctx->result;
		}
		task->warp_cycles++;
		task->total_cycles++;
		for (unsigned int lane_id = 0; lane_id < GPGPU_WARP_SIZE; lane_id++)
			if (warp->lanes[lane_id].active && !gpgpu_isa_step(ctx, warp, lane_id))
				return ctx->result;
		cycles++;
	}
	return (GPGPUExecResult) { .status = GPGPU_EXEC_COMPLETE };
}

/* 保存参数和 GPUVM 状态；创建成功后才交由 Dispatch 安排执行。 */
GPGPUDispatchTask *gpgpu_core_create_task(GPGPUState *s, const GPGPUKernelParams *params,
					  bool gpuvm_enabled)
{
	GPGPUDispatchTask *task = g_new0(GPGPUDispatchTask, 1);

	task->exec.device = s;
	task->exec.params = *params;
	task->exec.gpuvm_enabled = gpuvm_enabled;
	if (!core_set_shape(task)) {
		g_free(task);
		return NULL;
	}
	return task;
}

/* 当前 warp 完成后只保存下一处位置，下一个回调再初始化下一个 warp。 */
GPGPUExecResult gpgpu_core_step_task(GPGPUDispatchTask *task)
{
	const GPGPUKernelParams *params = &task->exec.params;
	GPGPUExecResult result;

	if (!task->warp_ready) {
		uint32_t block_id[3] = {
			task->block_index % params->grid_dim[0],
			task->block_index / params->grid_dim[0] % params->grid_dim[1],
			task->block_index / params->grid_dim[0] / params->grid_dim[1],
		};
		uint32_t first = task->warp_index * GPGPU_WARP_SIZE;
		uint32_t count = MIN(GPGPU_WARP_SIZE, task->threads_per_block - first);

		core_init_warp(&task->warp, params, first, block_id, count,
			       task->warp_index, task->block_index);
		task->warp_ready = true;
		task->warp_cycles = 0;
	}
	result = core_exec_warp(task, 128);
	if (result.status != GPGPU_EXEC_COMPLETE)
		return result;
	task->warp_ready = false;
	if (++task->warp_index == task->warp_count) {
		task->warp_index = 0;
		task->block_index++;
	}
	return (GPGPUExecResult) {
		.status = task->block_index == task->block_count ? GPGPU_EXEC_COMPLETE :
								   GPGPU_EXEC_CONTINUE,
	};
}

/* Core 任务没有异步访存；调用者停止推进后即可释放内部上下文。 */
void gpgpu_core_destroy_task(GPGPUDispatchTask *task)
{
	g_free(task);
}
