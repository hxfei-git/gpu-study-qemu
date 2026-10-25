/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPGPU_CORE_H
#define HW_GPGPU_CORE_H

#include "gpgpu_exec_types.h"

typedef struct GPGPUState GPGPUState;
typedef struct GPGPUDispatchTask GPGPUDispatchTask;

/**
 * gpgpu_core_exec_kernel() - 同步执行完整网格
 * @s: 提供存储访问的设备。
 * @params: 已完成地址和非零维度检查的配置，函数进入时复制。
 *
 * 调用期间保持原设备执行锁。函数返回时全部访问已结束，结果由 Dispatch
 * 发布；每个 warp 仍最多执行 100000 轮。
 *
 * Return: 完成或失败，以及本次产生的细分错误。
 */
GPGPUExecResult gpgpu_core_exec_kernel(GPGPUState *s, const GPGPUKernelParams *params);

/**
 * gpgpu_core_create_task() - 保存队列任务快照并准备分段进度
 * @s: 任务寿命内有效的设备。
 * @params: 已通过地址、权限和队列绑定检查的配置。
 * @mode: 物理队列或 GPUVM 队列模式。
 *
 * 本函数核对网格规模，不执行指令。Dispatch 持有返回的句柄，并在终态发布
 * 或取消时调用 gpgpu_core_destroy_task()；不得直接修改任务内部字段。
 *
 * Return: 任务句柄；网格规模非法时返回 NULL。
 */
GPGPUDispatchTask *gpgpu_core_create_task(GPGPUState *s, const GPGPUKernelParams *params,
					  GPGPUExecMode mode);

/**
 * gpgpu_core_step_task() - 按保存进度推进队列任务
 * @task: 本次要推进的任务，由 Dispatch 串行调用。
 *
 * 每次最多推进 128 轮，不跨过当前 warp 的完成边界。返回待续时由 Dispatch
 * 安排下一次定时器，返回终态时任务仍由调用者持有。
 *
 * Return: 待续、完成或失败及细分错误。
 */
GPGPUExecResult gpgpu_core_step_task(GPGPUDispatchTask *task);

/**
 * gpgpu_core_destroy_task() - 释放已经停止推进的任务
 * @task: 要释放的任务，可以为 NULL。
 *
 * 调用前撤销后续定时器；调用后句柄失效，不发布设备状态或中断。
 */
void gpgpu_core_destroy_task(GPGPUDispatchTask *task);

#endif /* HW_GPGPU_CORE_H */
