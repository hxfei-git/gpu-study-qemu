/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 模型算子：回放器与常驻网络执行器共用的派发和复合算子 lowering
 *
 * 调用者准备完整的本机字节序命令，持有 RT、Kernel 和全部设备对象：
 *
 *   完整命令（真实 GPU 地址、DIMS、原始 COUNT、数学常量）
 *     -> 基本算子选择标量或张量块，复合算子拆为标量步骤
 *     -> 每步上传一次完整命令 -> RT_BASE 逐 tile 推进 -> 同步完成
 *
 * 布局检查不依赖设备地址，可先于数组分配执行。
 * 本模块不分配、释放或接管对象；失败前已完成的步骤不回退。
 */
#ifndef GPGPU_MODEL_OPS_H
#define GPGPU_MODEL_OPS_H

#include "gpgpu/gpgpu_eval_abi.h"
#include "gpgpu_rt.h"

/* 完整命令的 64 位槽数，包含维度、scratch 和数学常量。 */
#define GPGPU_MODEL_WORDS (GPGPU_EVAL_PARAM_BYTES / GPGPU_EVAL_SLOT_BYTES)
/* 完整命令中的输入地址槽数。 */
#define GPGPU_MODEL_INPUTS ((GPGPU_EVAL_IN5 - GPGPU_EVAL_IN0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* 完整命令中的维度及属性槽数。 */
#define GPGPU_MODEL_DIMS ((GPGPU_EVAL_D15 - GPGPU_EVAL_D0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* 复合算子可用的 scratch 地址槽数。 */
#define GPGPU_MODEL_SCRATCHES ((GPGPU_EVAL_AUX2 - GPGPU_EVAL_AUX0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* RT 接收的参数槽数：命令地址与 tile 起点。 */
#define GPGPU_MODEL_RT_ARGS ((GPGPU_EVAL_RT_BASE + GPGPU_EVAL_SLOT_BYTES) / GPGPU_EVAL_SLOT_BYTES)
/* 每个 X 方向工作组的线程数，所有模型执行入口共用此定义。 */
#define GPGPU_MODEL_BLOCK_THREADS 64u

/**
 * struct GpgpuModelExecutor - 调用者持有的串行算子执行环境
 * @rt: 已建立的运行时，同一实例的调用须串行执行。
 * @kernel: 已按 GPGPU_MODEL_RT_ARGS 装载的 model_eval Kernel。
 * @command: 至少 GPGPU_EVAL_PARAM_BYTES 字节的可写设备命令对象。
 * @tile: gpgpu_model_tile_valid() 返回的实际工作项上限，按工作组对齐。
 * @timeout_ms: 每次同步派发的等待上限，单位毫秒。
 *
 * 本结构仅借用对象；调用者保证它们及命令引用的数组在执行期间有效。
 */
typedef struct GpgpuModelExecutor {
	GpgpuRt *rt;
	const GpgpuRtKernel *kernel;
	GpgpuKmtMemory *command;
	uint64_t tile;
	uint32_t timeout_ms;
} GpgpuModelExecutor;

/**
 * struct GpgpuModelOpStats - 一次完整算子的统计，执行入口每次清零
 * @operator_ns: 全部步骤、参数上传和同步派发耗时，纳秒；不含验证回读。
 * @params_ns: 成功参数上传的累计耗时，纳秒。
 * @params_bytes: 成功参数上传的累计字节数。
 * @dispatch_ns: 同步派发尝试的累计耗时，纳秒。
 * @tile_dispatches: 全部步骤的派发尝试次数，包含失败的派发。
 * @work_items: 已尝试派发的逻辑工作项数，不含尾部补齐线程。
 * @clock_error: 单调时钟读取失败；此时执行入口不得返回成功。
 */
typedef struct GpgpuModelOpStats {
	uint64_t operator_ns;
	uint64_t params_ns;
	uint64_t params_bytes;
	uint64_t dispatch_ns;
	uint64_t tile_dispatches;
	uint64_t work_items;
	bool clock_error;
} GpgpuModelOpStats;

/**
 * struct GpgpuModelLayout - 按完整命令推导的最小数组长度与执行规模
 * @output_bytes: 连续完整输出至少需要的字节数。
 * @rows: 复合算子的归约行数；基本算子为零。
 * @work_count: 基本算子的完整 COUNT，或复合算子中间数组的元素数。
 * @vocabulary: embedding 的词表行数；其他算子为零。
 * @scratch_bytes: 各 scratch 至少需要的字节数；未使用的槽为零。
 * @integer_output: 输出为 INT64 时为 true，否则为 FP32。
 */
typedef struct GpgpuModelLayout {
	uint64_t output_bytes;
	uint64_t rows;
	uint64_t work_count;
	uint64_t vocabulary;
	uint64_t scratch_bytes[GPGPU_MODEL_SCRATCHES];
	bool integer_output;
} GpgpuModelLayout;

/**
 * gpgpu_model_layout_valid() - 检查完整逻辑布局并推导最小访问长度
 * @words: 本机字节序完整命令，COUNT 是非零的原始完整输出元素数。
 * @input_bytes: 六个输入各自可读的字节数；零表示该输入未提供。
 * @layout: 成功时写入推导结果，失败时保持原值。
 *
 * 只读取 OP、COUNT 和 DIMS，不依赖命令中的 GPU 地址，不读取数组数值。
 * 必需输入须足量；可选输入按 input_bytes 是否为零判断是否提供。
 * 调用者还须将 output_bytes/scratch_bytes 与实际声明区间比较，并检查
 * embedding token、INT64 转换等数值前提及所有设备地址的归属。
 *
 * Return: 已知操作、布局及所有长度计算有效时为 true。
 */
bool gpgpu_model_layout_valid(const uint64_t *words, const uint64_t *input_bytes,
			      GpgpuModelLayout *layout);

/**
 * gpgpu_model_constants() - 填写 Kernel 共用的数学常量
 * @words: 至少 GPGPU_MODEL_WORDS 槽的本机字节序命令，只覆盖常量槽。
 */
void gpgpu_model_constants(uint64_t *words);

/**
 * gpgpu_model_launch_valid() - 计算派发工作项数及实际后端
 * @words: 完整的本机字节序命令，COUNT 是原始完整输出元素数且非零。
 * @backend: 请求的 GPGPU_EVAL_BACKEND_SCALAR 或 GPGPU_EVAL_BACKEND_BLOCK。
 * @launch_count: 成功时写入基本算子工作项数；复合算子保留原始 COUNT。
 * @actual_backend: 成功时写入实际后端；不支持块的算子及复合算子
 *                  均保持标量。
 *
 * 调用者先用 gpgpu_model_layout_valid() 检查布局；本接口只计算派发量。
 * COUNT 为零的空队列准备不经过本接口。
 *
 * Return: 已知操作且派发计算有效时为 true；失败时不改输出。
 */
bool gpgpu_model_launch_valid(const uint64_t *words, uint64_t backend, uint64_t *launch_count,
			      uint64_t *actual_backend);

/**
 * gpgpu_model_tile_valid() - 按共享执行能力限制并对齐 tile
 * @wanted: 请求的工作项上限，须非零。
 * @tile: 成功时写入实际工作项上限，按 GPGPU_MODEL_BLOCK_THREADS 对齐。
 *
 * Return: 能容纳至少一个完整工作组时为 true；失败时不改输出。
 */
bool gpgpu_model_tile_valid(uint64_t wanted, uint64_t *tile);

/**
 * gpgpu_model_execute() - 同步执行完整算子或其全部标量 lowering 步骤
 * @executor: 借用的执行环境，调用者串行使用并负责全部对象寿命。
 * @words: 完整命令，六个输入、输出和三个 scratch 槽均为实际 GPU 地址；
 *         未使用的地址填零，DIMS 完整，COUNT 保留原始完整输出元素数。
 * @rows: 复合算子的归约行数，由调用者按完整布局验证。
 * @work_count: 复合算子中间数组的元素数，由调用者按完整布局验证。
 * @backend: 请求的后端；基本算子内部重算派发量，复合算子保持标量。
 * @stats: 本次统计，入口清零；失败时保留已经完成或尝试的部分。
 *
 * 每步从完整模板复制命令，仅块基本算子把 AUX1/AUX2 改为原始 COUNT 和
 * BLOCK。每步只上传一次参数，后续 tile 仅推进 RT_BASE，不偏移数组地址。
 * 失败不销毁 RT、Kernel 或 BO；调用者须保守标记未确认状态后再处理回收。
 *
 * Return: 全部步骤和计时成功时为 SUCCESS；参数非法为 INVALID_PARAMETER，
 * 时钟失败为 ERROR，否则保留上传或派发返回的错误。
 */
GpgpuKmtStatus gpgpu_model_execute(GpgpuModelExecutor *executor, const uint64_t *words,
				   uint64_t rows, uint64_t work_count, uint64_t backend,
				   GpgpuModelOpStats *stats);

#endif /* GPGPU_MODEL_OPS_H */
