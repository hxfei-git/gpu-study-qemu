/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef HW_GPGPU_MEMORY_H
#define HW_GPGPU_MEMORY_H

typedef struct GPGPUExecContext GPGPUExecContext;
/**
 * gpgpu_memory_fetch() - 从用户地址空间读取一条 RV32 指令
 * @ctx: 保存代码范围和地址模式的执行上下文。
 * @address: lane 当前 PC。
 * @instruction: 成功时接收小端解码后的指令。
 *
 * 先检查提交的代码范围，再经统一用户访问入口检查 EXEC 权限并读取。
 *
 * Return: 读取成功返回 true；失败返回 false，并保存细分错误和必要的 VM 故障。
 */
bool gpgpu_memory_fetch(GPGPUExecContext *ctx, uint32_t address, uint32_t *instruction);

/**
 * gpgpu_memory_load_word() - 读取线程可见的一个 32 位字
 * @ctx: 参数快照、地址模式和错误结果。
 * @address: RV32 数据地址。
 * @value: 成功时接收小端解码后的值。
 *
 * 所有地址经统一用户访问入口翻译或直通，不再识别特殊内存窗口。
 *
 * Return: 成功返回 true；失败返回 false，保存执行错误和所属队列故障。
 */
bool gpgpu_memory_load_word(GPGPUExecContext *ctx, uint32_t address, uint32_t *value);

/**
 * gpgpu_memory_store_word() - 写入线程可见的一个 32 位字
 * @ctx: 地址模式、VM 归属和错误结果。
 * @address: RV32 数据地址。
 * @value: 要写入的值，存储时转换为小端。
 *
 * 写入与读取使用相同的地址空间，返回前本次同步访问已经结束。
 *
 * Return: 成功返回 true；失败返回 false，并保存细分错误和必要的 VM 故障。
 */
bool gpgpu_memory_store_word(GPGPUExecContext *ctx, uint32_t address, uint32_t value);

#endif /* HW_GPGPU_MEMORY_H */
