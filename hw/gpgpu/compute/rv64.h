/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RV64 解释器：按 lane 顺序执行；可在同一轮内共享取指和字段译码
 *
 * 支持的指令子集（按 RV64 语义，整数寄存器与 PC 为 64 位）：
 *   整数：LUI/AUIPC、ADDI、位运算、立即数移位、ADD/SUB/MUL、DIVU/REMU、
 *         条件分支、JAL/JALR
 *   访存：LB/LBU、LW/LD、SB/SW/SD、FLW/FSW
 *   浮点：FADD/FSUB/FMUL/FDIV/FSQRT.S、FMADD.S、FMIN/FMAX.S、FEQ/FLT/FLE.S、
 *         FCVT.W.S/FCVT.S.W、FMV.W.X/FMV.X.W，BF16/FP8/FP4 转换
 *   系统：EBREAK（结束线程），CSRRS rs1=x0（只读启动 CSR 与 mhartid）
 */
#ifndef HW_GPGPU_COMPUTE_RV64_H
#define HW_GPGPU_COMPUTE_RV64_H

typedef struct GPGPUExecContext GPGPUExecContext;
typedef struct GPGPUWarp GPGPUWarp;

/**
 * gpgpu_rv64_step() - 执行一个活跃 lane 的一条指令
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
bool gpgpu_rv64_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id);

/**
 * gpgpu_rv64_round() - 按 lane 编号推进一轮，可共享同 PC 的取指和字段译码
 * @ctx: 任务上下文，shared_fetch 在创建任务时锁存。
 * @warp: 当前 warp；每个活跃 lane 最多执行一条指令。
 *
 * 调度器已扣除一轮预算。共享表只在本次调用内有效，容量由
 * GPGPU_WARP_SIZE 推导；不跨轮、执行段、warp 或 Dispatch 缓存。
 * 例如 lane0/2 的 PC 为 0x1000，lane1/3 为 0x1020，执行顺序仍为
 * 0、1、2、3，不能先执行整个 0x1000 组再执行 0x1020 组。
 *
 * 1. 未共享且未剖析时，使用原逐 lane 路径；
 * 2. 共享模式首次遇到 PC 时，经原取指检查取得静态字段，不预取后续组；
 * 3. 后续同 PC lane 复用字段，仍独立检查编码、读取寄存器、处理动态
 *    舍入和副作用。首次失败立即停止，先前写入不回退。
 *
 * 代码和映射须在派发期间保持稳定，并按既有约定发布。
 * 不支持执行期自改代码、经别名回写代码或改写在途页表。
 * RX 本身不能证明不存在可写别名。
 *
 * Return: 本轮全部活跃 lane 执行成功时为 true；否则 ctx->error 已记录。
 */
bool gpgpu_rv64_round(GPGPUExecContext *ctx, GPGPUWarp *warp);

#endif /* HW_GPGPU_COMPUTE_RV64_H */
