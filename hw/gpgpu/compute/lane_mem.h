/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lane 访存：RV64 线程的取指与 8/32/64 位数据访问
 *
 * 三个入口都经 gpgpu_user_rw() 访问 64 位用户地址，故障记到任务的来源槽。
 * 参数块不是特殊窗口：Kernel 用 a0 中的真实地址以普通 LD/LW 读取。
 */
#ifndef HW_GPGPU_COMPUTE_LANE_MEM_H
#define HW_GPGPU_COMPUTE_LANE_MEM_H

typedef struct GPGPUExecContext GPGPUExecContext;

/**
 * gpgpu_lane_fetch() - 读取 pc 处的一条指令
 * @ctx: 任务上下文。
 * @pc: 指令地址。
 * @insn: 成功时写入指令字。
 *
 * 先检查 pc 位于 [kernel_addr, kernel_addr + code_size) 且 4 字节对齐，
 * 再以 EXEC 权限读取。
 *
 * Return: 成功为 true；失败时 ctx->error 为 GPGPU_EXEC_ERR_CODE_RANGE。
 */
bool gpgpu_lane_fetch(GPGPUExecContext *ctx, uint64_t pc, uint32_t *insn);

/**
 * gpgpu_lane_load() - 按小端读取一个数据值
 * @ctx: 任务上下文。
 * @addr: 64 位用户地址，须按 size 自然对齐。
 * @size: 1、4 或 8 字节，由解释器先校验指令宽度。
 * @value: 成功时写入零扩展后的值；有符号扩展由指令执行函数完成。
 *
 * Return: 成功为 true；失败时 ctx->error 为 GPGPU_EXEC_ERR_MEMORY。
 */
bool gpgpu_lane_load(GPGPUExecContext *ctx, uint64_t addr, unsigned int size, uint64_t *value);

/**
 * gpgpu_lane_store() - 按小端写入一个数据值
 * @ctx: 任务上下文。
 * @addr: 64 位用户地址，须按 size 自然对齐。
 * @size: 1、4 或 8 字节，由解释器先校验指令宽度。
 * @value: 写入其低 size 字节。
 *
 * Return: 成功为 true；失败时 ctx->error 为 GPGPU_EXEC_ERR_MEMORY。
 */
bool gpgpu_lane_store(GPGPUExecContext *ctx, uint64_t addr, unsigned int size, uint64_t value);

#endif /* HW_GPGPU_COMPUTE_LANE_MEM_H */
