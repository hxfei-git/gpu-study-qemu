/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 张量块执行：受限矩阵乘加与布局传输
 *
 * RV64 指令分派器传入 GPR 的值，本模块不获取 GPU 后备存储的宿主指针：
 *
 *   指令分派 ─▶ 参数前缀 ─gpgpu_user_rw()─▶ 有界局部输入
 *                                            ├─SoftFloat─┐
 *                                            └─安全 SIMD─┴─用户访存─▶ 输出
 *
 * 访存沿 GPUVM/保护区检查和设备缓存路径完成。调用者设置本 Lane 的 RNE，
 * 清除本指令异常，负责汇总 fflags、推进 PC，以及失败后停止后续 Lane。
 */
#ifndef HW_GPGPU_COMPUTE_TENSOR_H
#define HW_GPGPU_COMPUTE_TENSOR_H

#include "compute_internal.h"

/**
 * struct GPGPUTensorStats - 可选的张量块执行累计值
 * @macs: 已完成本地计算的逻辑 FP32 FMA 次数，不含反量化和偏置加法。
 * @native_tiles: 通过安全条件与异常检查的 SIMD 矩阵块数。
 * @fallback_tiles: 完整 SoftFloat 参考路径计算的矩阵块数，
 *                  含禁用 SIMD 的情况。
 * @layout_elements: 已完成局部搬运或转换的逻辑元素数，不是访存请求数。
 *
 * 调用者负责初始化和合并，本模块只累加、不清零。
 * 计算完成后的写回即使失败，也保留对应计算量。
 * GPU 请求数和字节数另记到 ctx->profile，
 * 不能由本结构推导。
 */
typedef struct GPGPUTensorStats {
	uint64_t macs;
	uint64_t native_tiles;
	uint64_t fallback_tiles;
	uint64_t layout_elements;
} GPGPUTensorStats;

/**
 * gpgpu_tensor_execute() - 执行一条有界矩阵或布局指令
 * @ctx: 派发上下文，调用者持有设备主锁。
 * @lane: 当前 Lane；fp_status 已设为 RNE 并清除本指令异常。
 * @kind: gpgpu_tensor_abi.h 中的 GPGPU_TENSOR_KIND_*。
 * @format: 对应类别的格式，不使用模型评估器的操作编号。
 * @command: 按 8 字节对齐的 GPU 参数地址；只读取通用的 224 字节前缀。
 * @block: 线性工作块号；越界作为错误，不作为空操作。
 * @k_base: 矩阵的 K 起点；总 K 为 0 时只接受 0，布局必须为 0。
 * @native_enabled: 允许尝试宿主 SIMD；能力或数值条件不满足时使用 SoftFloat。
 * @stats: 可选的累计值，允许为 NULL。
 *
 * GPU 地址与派发参数同域。
 * 所有 GPU 访问均经过 gpgpu_user_rw()。
 * 先读取本块全部输入，再计算和写回。
 * Linear/BMM 按 M 行递增写回；
 * NCHW 卷积按输出通道递增写回连续空间片段。
 * 后行失败保留前行；当前行跨页也可能部分写入，
 * 遵循现有用户访存接口的语义。
 * 访存失败记录来源槽的首次故障，
 * 并设置 ctx->error。
 * 非法编码只设置 ctx->error，不记录 GPUVM 故障；
 * 错误码为 GPGPU_EXEC_ERR_INSTRUCTION。
 * 参数或地址检查失败仍记录 RANGE。
 *
 * 只累加 lane->fp_status 的异常，不修改 PC、active 或 fcsr，也不改变 frm。
 * native 成功、拒绝或失败时，
 * 均先恢复宿主浮点环境，再返回或运行参考路径。
 *
 * Return: 本块写回全部完成时为 true；非法参数、块号或访存失败时为 false。
 */
bool gpgpu_tensor_execute(GPGPUExecContext *ctx, GPGPULane *lane, uint32_t kind,
			  uint32_t format, uint64_t command, uint64_t block, uint64_t k_base,
			  bool native_enabled, GPGPUTensorStats *stats);

#endif /* HW_GPGPU_COMPUTE_TENSOR_H */
