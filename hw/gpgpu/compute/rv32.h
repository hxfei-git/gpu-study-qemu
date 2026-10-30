/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RV32 解释器：执行一个 lane 的一条指令
 *
 * 支持的指令子集：
 *   整数：LUI、ADDI、ANDI、SLLI、ADD、SUB、MUL、条件分支、JAL
 *   访存：LW、SW、FLW、FSW
 *   浮点：FADD.S、FMUL.S、FCVT.W.S、FCVT.S.W、FMV.W.X，BF16/FP8/FP4 转换
 *   系统：EBREAK（结束线程），CSRRS rs1=x0（只读启动 CSR 与 mhartid）
 */
#ifndef HW_GPGPU_COMPUTE_RV32_H
#define HW_GPGPU_COMPUTE_RV32_H

typedef struct GPGPUExecContext GPGPUExecContext;
typedef struct GPGPUWarp GPGPUWarp;

/**
 * gpgpu_rv32_step() - 执行一个活跃 lane 的一条指令
 * @ctx: 任务上下文。
 * @warp: lane 所属 warp。
 * @lane_id: lane 编号，0～31，调用者保证该 lane 活跃。
 *
 * 1. 按 lane 的 pc 取指，越出代码范围或未对齐时失败；
 * 2. 拆出寄存器号、funct 字段和立即数，此时不判断编码是否合法；
 * 3. 按主 opcode 分派到类别函数，由它检查编码并执行，更新寄存器、
 *    PC 或结束标记。非法编码记 GPGPU_EXEC_ERR_INSTRUCTION。
 *
 * Return: 成功为 true；失败时 ctx->error 为细分错误。
 */
bool gpgpu_rv32_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id);

#endif /* HW_GPGPU_COMPUTE_RV32_H */
