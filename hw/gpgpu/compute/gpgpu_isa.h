/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPGPU_ISA_H
#define HW_GPGPU_ISA_H

typedef struct GPGPUExecContext GPGPUExecContext;
typedef struct GPGPUWarp GPGPUWarp;

/**
 * gpgpu_isa_step() - 执行一个活跃 lane 的一条 RV32 指令
 * @ctx: 本任务的参数、地址模式和失败结果。
 * @warp: lane 所属的 warp，提供线程身份并保存活跃掩码。
 * @lane_id: 活跃 lane 的编号，范围为 0～31。
 *
 * 取指和数据访问交给计算访存模块；成功后更新该 lane 的寄存器、PC 或结束
 * 状态。Core 负责调用顺序和预算，本函数不发布派发状态。
 *
 * Return: 指令已执行返回 true；失败返回 false，并在 ctx 中记录执行结果。
 */
bool gpgpu_isa_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id);

#endif /* HW_GPGPU_ISA_H */
