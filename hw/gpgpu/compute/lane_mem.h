/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lane 访存：RV32 线程的取指与 32 位数据访问
 *
 * 三个入口都经 gpgpu_user_rw() 访问用户地址，故障记到任务的来源槽。
 * 参数块不是特殊窗口：Kernel 用 a0 中的真实地址以普通 LW 读取。
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
bool gpgpu_lane_fetch(GPGPUExecContext *ctx, uint32_t pc, uint32_t *insn);

/* 以 READ 权限读取 32 位字；失败时 ctx->error 为 GPGPU_EXEC_ERR_MEMORY。 */
bool gpgpu_lane_load(GPGPUExecContext *ctx, uint32_t addr, uint32_t *value);

/* 以 WRITE 权限写入 32 位字；失败时 ctx->error 为 GPGPU_EXEC_ERR_MEMORY。 */
bool gpgpu_lane_store(GPGPUExecContext *ctx, uint32_t addr, uint32_t value);

#endif /* HW_GPGPU_COMPUTE_LANE_MEM_H */
