/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPGPU_DISPATCH_H
#define HW_GPGPU_DISPATCH_H

#include "exec/hwaddr.h"
#include "qemu/typedefs.h"
#include "gpgpu_exec_types.h"

typedef struct GPGPUState GPGPUState;
typedef struct GPGPUDispatchTask GPGPUDispatchTask;

/**
 * struct GPGPUDispatchState - Dispatch 管理的配置、任务和结果寄存器
 * @kernel: 空闲时可写的配置；提交后由 Core 另存快照。
 * @task: 当前队列任务的句柄，内部上下文由 Core 管理。
 * @timer: 使用虚拟时钟推进任务的定时器。
 * @status: IDLE、BUSY、DONE 或 FAILED，队列终态由 CP 写 ACK 确认。
 * @error: 本次细分错误，保留同步与队列路径各自的清除规则。
 *
 * 设备内嵌这份状态；其他模块通过以下入口访问，不能修改任务句柄和寄存器。
 */
typedef struct GPGPUDispatchState {
	GPGPUKernelParams kernel;
	GPGPUDispatchTask *task;
	QEMUTimer *timer;
	uint32_t status;
	uint32_t error;
} GPGPUDispatchState;

/**
 * gpgpu_dispatch_read() - 读回派发配置或执行结果
 * @s: 目标设备。
 * @addr: BAR0 寄存器字节偏移，调用者已检查访问宽度和对齐。
 *
 * Return: 当前 32 位寄存器值，未定义偏移返回零；读取不清除状态。
 */
uint32_t gpgpu_dispatch_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_dispatch_write() - 处理 Host 同步派发配置和启动命令
 * @s: 目标设备，在现有设备主锁保护下调用。
 * @addr: BAR0 寄存器字节偏移。
 * @value: 写入的 32 位值。
 *
 * 队列模式拒绝此入口。同步启动返回前完成计算和通知发布；非法配置保持
 * 原有状态并锁存全局命令错误。
 */
void gpgpu_dispatch_write(GPGPUState *s, hwaddr addr, uint32_t value);

/**
 * gpgpu_dispatch_cp_write() - 处理 CP 的配置、提交及结果确认
 * @s: 目标设备，在现有设备主锁保护下调用。
 * @addr: CP 窗口已换算的寄存器字节偏移。
 * @value: 写入的 32 位值。
 *
 * 提交成功只保存任务并安排定时器；后续推进发布 DONE 或 FAILED。CP 必须在
 * 终态写入 ACK 后才能再次配置，ACK 同时清除细分错误。
 */
void gpgpu_dispatch_cp_write(GPGPUState *s, hwaddr addr, uint32_t value);

/**
 * gpgpu_dispatch_init() - 创建分段执行定时器
 * @s: 已完成基础存储与中断初始化的设备。
 *
 * 定时器归 Dispatch 所有，调用本函数不会提交任务。
 */
void gpgpu_dispatch_init(GPGPUState *s);

/**
 * gpgpu_dispatch_reset() - 停止推进并清空派发状态
 * @s: CP 已停止的设备。
 *
 * 先撤销定时器，再释放 Core 任务，最后清配置、细分错误并恢复 IDLE。
 * 本函数保留定时器对象，不操作其他模块的状态。
 */
void gpgpu_dispatch_reset(GPGPUState *s);

/**
 * gpgpu_dispatch_cleanup() - 释放 Dispatch 生命周期资源
 * @s: 已停止 CP、不再接受提交的设备。
 *
 * 取消任务后释放定时器，调用后不再使用派发入口。
 */
void gpgpu_dispatch_cleanup(GPGPUState *s);

#endif /* HW_GPGPU_DISPATCH_H */
