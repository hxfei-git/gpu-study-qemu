// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 调度器：网格规模检查、warp 初始化与分段推进
 */
#include "qemu/osdep.h"
#include "compute_internal.h"
#include "rv32.h"
#include "scheduler.h"

/**
 * struct GPGPUTask - 一次派发的执行进度
 * @exec: 配置快照与当前指令的错误。
 * @warp: 正在执行的 warp。
 * @block_count: 工作组总数。
 * @threads_per_block: 每工作组线程数。
 * @warps_per_block: 每工作组 warp 数。
 * @block_index: 当前线性工作组号。
 * @warp_index: 当前工作组内的 warp 号。
 * @warp_rounds: 当前 warp 已推进的轮数。
 * @warp_ready: 当前 warp 已初始化，下一段从原进度继续。
 */
struct GPGPUTask {
	GPGPUExecContext exec;
	GPGPUWarp warp;
	uint32_t block_count;
	uint32_t threads_per_block;
	uint32_t warps_per_block;
	uint32_t block_index;
	uint32_t warp_index;
	uint32_t warp_rounds;
	bool warp_ready;
};

/*
 * task_set_shape() - 计算工作组数与每组线程数并检查上限
 *
 * 按 X、Y、Z 依次累乘工作组数和每组线程数。每乘一维之前先用
 * “上限 / 当前累计值”判断，这一维会让乘积超过上限就直接拒绝，
 * 所以乘法本身不会溢出 32 位。三维都通过后再检查线程总数上限，
 * 最后按 32 线程一组算出每个工作组的 warp 数（向上取整）。
 *
 * Return: false 表示某一维为 0 或超出上限。
 */
static bool task_set_shape(GPGPUTask *task)
{
	const GPGPUKernelParams *p = &task->exec.params;

	/* 三维逐一累乘；乘之前先判断，乘积不会溢出。 */
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

	/* 线程总数 = 工作组数 × 每组线程数。 */
	if (task->block_count > GPGPU_MAX_TOTAL_THREADS / task->threads_per_block)
		return false;
	task->warps_per_block = DIV_ROUND_UP(task->threads_per_block, GPGPU_WARP_SIZE);
	return true;
}

/* 新线程的浮点状态：就近舍入、舍入后判断下溢、默认 NaN。 */
static void lane_init_fp(GPGPULane *lane)
{
	set_float_rounding_mode(float_round_nearest_even, &lane->fp_status);
	set_float_detect_tininess(float_tininess_after_rounding, &lane->fp_status);
	set_default_nan_mode(true, &lane->fp_status);
	set_float_default_nan_pattern(0x40, &lane->fp_status);
}

/*
 * task_init_warp() - 初始化 (block_index, warp_index) 位置的 warp
 *
 * 任务只保存一个 warp 的寄存器状态，每推进到新位置都重新初始化：
 *
 * 1. 由线性工作组号还原 blockIdx（X 变化最快），与 blockDim、
 *    gridDim 一起写入整个 warp 共享的只读启动值；
 * 2. 计算活跃掩码。工作组线程数不是 32 的倍数时，最后一个 warp
 *    不满，多出的 lane 不激活；
 * 3. 逐个设置活跃 lane：由组内线程号还原 threadIdx，a0 = 参数块地址，
 *    PC = 代码入口，mhartid 编码线程身份，并初始化浮点状态。
 */
static void task_init_warp(GPGPUTask *task)
{
	const GPGPUKernelParams *p = &task->exec.params;
	GPGPUWarp *warp = &task->warp;
	uint32_t first_thread = task->warp_index * GPGPU_WARP_SIZE;
	uint32_t lanes = MIN(GPGPU_WARP_SIZE, task->threads_per_block - first_thread);
	uint32_t block_id[3] = {
		task->block_index % p->grid_dim[0],
		task->block_index / p->grid_dim[0] % p->grid_dim[1],
		task->block_index / p->grid_dim[0] / p->grid_dim[1],
	};

	memset(warp, 0, sizeof(*warp));

	/* 1. 启动值依次为 blockIdx[3]、blockDim[3]、gridDim[3]。 */
	for (unsigned int i = 0; i < 3; i++) {
		warp->startup_sgpr[i] = block_id[i];
		warp->startup_sgpr[3 + i] = p->block_dim[i];
		warp->startup_sgpr[6 + i] = p->grid_dim[i];
	}

	/* 2. 低 lanes 位为 1；lanes 为 32 时单独处理，避免移位 32 位。 */
	warp->active_mask = lanes == GPGPU_WARP_SIZE ? UINT32_MAX : (1u << lanes) - 1;

	/* 3. 只初始化活跃 lane，其余 lane 保持 memset 后的全零。 */
	for (uint32_t lane_id = 0; lane_id < lanes; lane_id++) {
		GPGPULane *lane = &warp->lanes[lane_id];
		uint32_t thread = first_thread + lane_id;

		lane->thread_id[0] = thread % p->block_dim[0];
		lane->thread_id[1] = thread / p->block_dim[0] % p->block_dim[1];
		lane->thread_id[2] = thread / p->block_dim[0] / p->block_dim[1];
		lane->gpr[GPGPU_REG_A0] = p->kernel_args;
		lane->pc = p->kernel_addr;
		lane->mhartid = GPGPU_MHARTID(task->block_index, task->warp_index, lane_id);
		lane->active = true;
		lane_init_fp(lane);
	}

	/* 新 warp 的轮数预算从零开始累计。 */
	task->warp_rounds = 0;
	task->warp_ready = true;
}

static GPGPUExecResult task_failed(GPGPUTask *task)
{
	return (GPGPUExecResult) { .status = GPGPU_EXEC_FAILED, .error = task->exec.error };
}

/*
 * task_run_warp() - 推进当前 warp 至多 GPGPU_SEGMENT_ROUNDS 轮
 *
 * 一轮让每个仍活跃的 lane 各执行一条指令；lane 执行 EBREAK 后
 * 退出活跃掩码，掩码清空即 warp 结束。本段轮数用完时返回 CONTINUE，
 * 下一段从同一位置继续。warp_rounds 跨段累计，超过
 * GPGPU_WARP_ROUND_BUDGET 判为超预算，防止死循环的 Kernel 永不结束。
 * 预算按轮计数，与活跃 lane 数无关。
 *
 * Return: warp 结束为 COMPLETE，段用完为 CONTINUE，指令失败或超预算为 FAILED。
 */
static GPGPUExecResult task_run_warp(GPGPUTask *task)
{
	GPGPUWarp *warp = &task->warp;

	for (unsigned int round = 0; warp->active_mask; round++) {
		/* 本段轮数用完，让出给定时器。 */
		if (round == GPGPU_SEGMENT_ROUNDS)
			return (GPGPUExecResult) { .status = GPGPU_EXEC_CONTINUE };

		/* 累计轮数达到预算仍未结束，判为超预算。 */
		if (task->warp_rounds++ == GPGPU_WARP_ROUND_BUDGET) {
			gpgpu_exec_fail(&task->exec, GPGPU_EXEC_ERR_BUDGET);
			return task_failed(task);
		}

		/* 每个活跃 lane 各执行一条指令，任一失败即任务失败。 */
		for (unsigned int lane_id = 0; lane_id < GPGPU_WARP_SIZE; lane_id++)
			if (warp->lanes[lane_id].active &&
			    !gpgpu_rv32_step(&task->exec, warp, lane_id))
				return task_failed(task);
	}
	return (GPGPUExecResult) { .status = GPGPU_EXEC_COMPLETE };
}

GPGPUTask *gpgpu_task_create(GPGPUState *s, const GPGPUKernelParams *params)
{
	GPGPUTask *task = g_new0(GPGPUTask, 1);

	/* 复制配置快照，之后 Dispatch 寄存器的写入不影响本任务。 */
	task->exec.device = s;
	task->exec.params = *params;
	if (!task_set_shape(task)) {
		g_free(task);
		return NULL;
	}
	return task;
}

/*
 * gpgpu_task_step() - Dispatch 定时器每次回调推进一段
 *
 * 1. 新位置的 warp 尚未初始化时先初始化；已初始化说明上一段没跑完，
 *    直接从原进度继续；
 * 2. 推进当前 warp 一段；没结束或失败时把结果原样返回；
 * 3. warp 结束时前进到下一个位置：先换组内下一个 warp，本组 warp
 *    用完再换下一个工作组。所有工作组都完成才返回 COMPLETE，
 *    否则返回 CONTINUE，由下一段初始化新 warp。
 */
GPGPUExecResult gpgpu_task_step(GPGPUTask *task)
{
	GPGPUExecResult result;

	/* 1. warp_ready 为 false 表示刚换到新位置。 */
	if (!task->warp_ready)
		task_init_warp(task);

	/* 2. CONTINUE 或 FAILED 直接返回给 Dispatch。 */
	result = task_run_warp(task);
	if (result.status != GPGPU_EXEC_COMPLETE)
		return result;

	/* 3. 当前 warp 已结束，移到下一个 warp 或下一个工作组。 */
	task->warp_ready = false;
	if (++task->warp_index == task->warps_per_block) {
		task->warp_index = 0;
		task->block_index++;
	}
	return (GPGPUExecResult) {
		.status = task->block_index == task->block_count ? GPGPU_EXEC_COMPLETE :
								   GPGPU_EXEC_CONTINUE,
	};
}

void gpgpu_task_destroy(GPGPUTask *task)
{
	g_free(task);
}
