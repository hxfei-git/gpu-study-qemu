/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 连接板级 CPU，管理 CP 启动与复位；窗口状态由窗口模块维护。 */
#ifndef HW_CP_GPGPU_CP_H
#define HW_CP_GPGPU_CP_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "gpgpu_cp_window.h"

/* 板级回调须在目标 CPU 线程中同步完成复位或启动。 */
typedef void (*GPGPUCPSetState)(void *opaque, bool reset_hold, uint32_t reset_vector);

/**
 * gpgpu_attach_cp() - 连接板级 CP 并立即保持复位
 * @s: 设备实例；调用期间须保持设备存活。
 * @set_state: 同步控制原生 CPU 的回调，连接后保持有效。
 * @opaque: 回调借用的板级上下文。
 */
void gpgpu_attach_cp(GPGPUState *s, GPGPUCPSetState set_state, void *opaque);

/**
 * gpgpu_cp_reset() - 使原生 CP 保持复位
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 保留 HQD、队列错误和本轮已经启动的记录。
 */
void gpgpu_cp_reset(GPGPUState *s);

/**
 * gpgpu_cp_device_reset() - 复位 CP 控制与本轮启动记录
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 仅由整体寄存器复位调用；先同步停止原生 CPU。
 */
void gpgpu_cp_device_reset(GPGPUState *s);

/**
 * gpgpu_cp_reset_access() - 清理 CP 窗口和固件握手
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 由板级代码在原生 CPU 已进入复位后调用，不清除 HQD。
 */
void gpgpu_cp_reset_access(GPGPUState *s);

/**
 * gpgpu_cp_host_configurable() - 查询 Host 是否仍可配置冷启动队列
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: CP 保持复位且本轮从未启动时为 true。
 */
bool gpgpu_cp_host_configurable(GPGPUState *s);

/**
 * gpgpu_cp_read() - 读取 CP 控制或启动入口
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 *
 * Return: 寄存器值；未连接 CP 或未定义偏移返回零。
 */
uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_cp_write() - 更新 CP 复位状态或启动入口
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 * @val: 本次写入的寄存器值。
 *
 * 重复状态写入不重启 CPU；非法入口保留原值并锁存错误。
 */
void gpgpu_cp_write(GPGPUState *s, hwaddr addr, uint32_t val);

/**
 * struct GPGPUCPState - 由 CP 模块维护的控制状态
 * @set_state: 板级同步控制回调。
 * @opaque: 借用的板级上下文。
 * @reset_hold: 当前是否保持复位。
 * @started: 本轮设备复位后是否启动过 CP。
 * @reset_vector: VRAM 中的复位入口字节偏移。
 * @window: CP 访存窗口配置；随 CPU 复位清理。
 */
typedef struct GPGPUCPState {
	GPGPUCPSetState set_state;
	void *opaque;
	bool reset_hold;
	bool started;
	uint32_t reset_vector;
	GPGPUCPWindowState window;
} GPGPUCPState;

#endif /* HW_CP_GPGPU_CP_H */
