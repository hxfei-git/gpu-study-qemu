/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 调度器：把一次派发拆成 warp，分段推进
 *
 * 工作组按线性编号依次执行（X 维变化最快），每个工作组按 32 线程
 * 切成若干 warp。一段最多推进 GPGPU_SEGMENT_ROUNDS 轮，一轮让 warp
 * 中每个活跃 lane 各执行一条指令；段与段之间由 Dispatch 的定时器衔接，
 * 期间设备可以处理其他 MMIO。一个 warp 累计超过
 * GPGPU_WARP_ROUND_BUDGET 轮仍未结束则判为超预算。
 */
#ifndef HW_GPGPU_COMPUTE_SCHEDULER_H
#define HW_GPGPU_COMPUTE_SCHEDULER_H

#include "compute.h"

typedef struct GPGPUState GPGPUState;
typedef struct GPGPUTask GPGPUTask;

/* 每段最多推进的轮数。 */
#define GPGPU_SEGMENT_ROUNDS 128
/* 单个 warp 允许的累计轮数上限。 */
#define GPGPU_WARP_ROUND_BUDGET 100000

/**
 * gpgpu_task_create() - 为一次派发创建任务
 * @s: 设备，任务寿命内有效。
 * @params: 已通过 Dispatch 检查的配置，函数内复制一份。
 *
 * 只检查网格规模，不执行指令。
 *
 * Return: 任务；网格规模超出上限时返回 NULL。
 */
GPGPUTask *gpgpu_task_create(GPGPUState *s, const GPGPUKernelParams *params);

/**
 * gpgpu_task_step() - 推进一段
 * @task: 任务。
 *
 * 一段不跨过当前 warp 的结束边界：warp 结束后只记录下一个位置，
 * 下一段再初始化下一个 warp。
 *
 * Return: CONTINUE、COMPLETE，或 FAILED 及细分错误。
 */
GPGPUExecResult gpgpu_task_step(GPGPUTask *task);

/* 释放任务；可以为 NULL。调用前已停止后续推进。 */
void gpgpu_task_destroy(GPGPUTask *task);

#endif /* HW_GPGPU_COMPUTE_SCHEDULER_H */
