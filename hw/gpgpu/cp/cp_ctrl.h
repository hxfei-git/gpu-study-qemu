/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 控制：原生 CP 的复位与启动
 *
 * 设备本身不含 CPU。板级代码（hw/riscv/gpgpu-cp.c）创建 RV64 CPU 后调用
 * gpgpu_cp_attach() 登记一个回调；Host 写 CP_CTRL / CP_RESET_VECTOR 时，
 * 设备通过该回调在 CPU 线程中同步完成复位或启动。
 *
 * “冷启动”指设备复位后 CP 还没有释放过复位；
 * 只有此时 Host 才能配置 KIQ。
 */
#ifndef HW_GPGPU_CP_CTRL_H
#define HW_GPGPU_CP_CTRL_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"

/**
 * typedef GPGPUCPSetState - 板级回调：同步把 CPU 置于复位或从 reset_vector 启动
 * @opaque: 板级上下文。
 * @reset_hold: true 为保持复位，false 为释放复位并开始执行。
 * @reset_vector: 释放复位后的入口，VRAM 字节偏移。
 *
 * 回调返回时状态已生效。保持复位时，回调须在 CPU 停住后调用
 * gpgpu_cp_on_cpu_reset()。
 */
typedef void (*GPGPUCPSetState)(void *opaque, bool reset_hold, uint32_t reset_vector);

/**
 * struct GPGPUCPState - CP 控制状态
 * @set_state: 板级回调；为 NULL 表示未接入原生 CP。
 * @opaque: 回调的板级上下文。
 * @reset_hold: 当前是否保持复位。
 * @started: 本次设备复位后是否释放过复位。
 * @reset_vector: 复位向量，VRAM 字节偏移。
 */
typedef struct GPGPUCPState {
	GPGPUCPSetState set_state;
	void *opaque;
	bool reset_hold;
	bool started;
	uint32_t reset_vector;
} GPGPUCPState;

/* 板级代码接入原生 CP，接入后立即保持复位。 */
void gpgpu_cp_attach(GPGPUState *s, GPGPUCPSetState set_state, void *opaque);

/* 板级回调在 CPU 停住后调用：清除访存窗口与固件握手，保留 HQD。 */
void gpgpu_cp_on_cpu_reset(GPGPUState *s);

/* CP 控制寄存器组（BAR0 0x500）；未接入 CP 时读零、写忽略。 */
uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr reg);
void gpgpu_cp_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 让 CP 保持复位，保留 HQD 与冷启动标记；用于设备退出。 */
void gpgpu_cp_hold(GPGPUState *s);

/* 设备复位：保持 CP 复位并重新进入冷启动状态。 */
void gpgpu_cp_reset(GPGPUState *s);

/* CP 保持复位且从未启动：Host 可以配置 KIQ。 */
bool gpgpu_cp_cold(GPGPUState *s);

/* CP 保持复位（包括未接入 CP 的情况为 false）。 */
bool gpgpu_cp_held(GPGPUState *s);

#endif /* HW_GPGPU_CP_CTRL_H */
