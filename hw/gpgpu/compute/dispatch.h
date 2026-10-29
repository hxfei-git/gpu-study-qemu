/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Dispatch：执行器的配置寄存器、提交与结果确认
 *
 * CP 与执行器之间的握手（寄存器均在 BAR0 0x300 组，只有 CP 可写）：
 *
 *   IDLE ──CP 写配置，再写 DISPATCH=1──▶ BUSY ──任务结束──▶ DONE / FAILED
 *     ▲                                                         │
 *     └──────────────────CP 写 DISPATCH_ACK=1 ◀─────────────────┘
 *
 * 提交时检查配置与队列绑定，不通过则直接进入 FAILED（INVALID_CONFIG）。
 * BUSY 与终态期间拒绝改写配置；ACK 同时清除 DISPATCH_ERROR。
 */
#ifndef HW_GPGPU_COMPUTE_DISPATCH_H
#define HW_GPGPU_COMPUTE_DISPATCH_H

#include "exec/hwaddr.h"
#include "qemu/typedefs.h"
#include "compute.h"

typedef struct GPGPUState GPGPUState;
typedef struct GPGPUTask GPGPUTask;

/**
 * struct GPGPUDispatchState - Dispatch 状态
 * @config: 配置寄存器；只在 IDLE 时可写，提交时复制给任务。
 * @task: 当前任务；只在 BUSY 时非 NULL。
 * @timer: 分段推进任务的定时器。
 * @status: GpgpuDispatchStatus。
 * @error: GpgpuDispatchError，终态时有效。
 */
typedef struct GPGPUDispatchState {
	GPGPUKernelParams config;
	GPGPUTask *task;
	QEMUTimer *timer;
	uint32_t status;
	uint32_t error;
} GPGPUDispatchState;

/* Dispatch 寄存器组读取（Host 与 CP 视图相同）。 */
uint32_t gpgpu_dispatch_read(GPGPUState *s, hwaddr reg);

/* CP 写入：配置、提交与确认。 */
void gpgpu_dispatch_cp_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 执行器是否 BUSY。 */
bool gpgpu_dispatch_busy(GPGPUState *s);

void gpgpu_dispatch_init(GPGPUState *s);

/* 停止定时器、释放任务、清除配置并回到 IDLE。 */
void gpgpu_dispatch_reset(GPGPUState *s);

void gpgpu_dispatch_cleanup(GPGPUState *s);

#endif /* HW_GPGPU_COMPUTE_DISPATCH_H */
