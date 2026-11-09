/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 算子评估参数：客体回放器与手写 RV64 Kernel 共用的偏移和操作编号
 *
 * 参数前缀复用通用张量块 ABI，不改变 job 文件格式。
 * 数学常量和操作编号仍由本组 Kernel 解释：
 *
 *   RT 参数槽 0：512 字节参数块的 GPU 地址
 *   RT 参数槽 1：当前 tile 的首个全局工作项编号
 *       → Kernel 从参数块取得数组地址、真实 shape 和数学常量
 *       → 标量 Lane 计算一个输出，块 Lane 执行一组有界张量指令
 *       → 复合算子仍由回放器顺序派发原标量步骤
 *
 * 所有偏移单位为字节。地址、编号和维度按小端 uint64_t 保存；浮点参数
 * 只使用对应槽的低 32 位。C 和预处理后的汇编都引用本头，不另抄偏移。
 */
#ifndef GPGPU_EVAL_ABI_H
#define GPGPU_EVAL_ABI_H

#include "gpgpu_tensor_abi.h"

/* 参数块字节数，覆盖全部维度、暂存地址与数学常量。 */
#define GPGPU_EVAL_PARAM_BYTES 512
/* 每个地址、维度或常量槽占用的字节数。 */
#define GPGPU_EVAL_SLOT_BYTES GPGPU_TENSOR_SLOT_BYTES
/* RT 参数中命令块 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_RT_COMMAND 0
/* RT 参数中本次 tile 首个全局工作项编号的字节偏移。 */
#define GPGPU_EVAL_RT_BASE 8
/* 操作编号所在的字节偏移；张量指令不解释此槽。 */
#define GPGPU_EVAL_OP GPGPU_TENSOR_OPAQUE
/* 第 0 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN0 GPGPU_TENSOR_IN0
/* 第 1 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN1 GPGPU_TENSOR_IN1
/* 第 2 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN2 GPGPU_TENSOR_IN2
/* 第 3 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN3 GPGPU_TENSOR_IN3
/* 第 4 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN4 GPGPU_TENSOR_IN4
/* 第 5 个只读数组 GPU 地址的字节偏移。 */
#define GPGPU_EVAL_IN5 GPGPU_TENSOR_IN5
/* 本步输出数组的 GPU 地址。 */
#define GPGPU_EVAL_OUT GPGPU_TENSOR_OUT
/* 本步实际工作项数；尾部 padding Lane 直接退出。 */
#define GPGPU_EVAL_COUNT GPGPU_TENSOR_WORK_COUNT
/* 算子维度及属性槽的起点，每槽 8 字节，共 16 槽。 */
#define GPGPU_EVAL_DIMS GPGPU_TENSOR_DIMS
/* 第 0 个属性槽的字节偏移；各算子的含义见 model_eval.S 入口注释。 */
#define GPGPU_EVAL_D0 GPGPU_EVAL_DIMS
/* 第 1 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D1 80
/* 第 2 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D2 88
/* 第 3 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D3 96
/* 第 4 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D4 104
/* 第 5 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D5 112
/* 第 6 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D6 120
/* 第 7 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D7 128
/* 第 8 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D8 136
/* 第 9 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D9 144
/* 第 10 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D10 152
/* 第 11 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D11 160
/* 第 12 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D12 168
/* 第 13 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D13 176
/* 第 14 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D14 184
/* 第 15 个属性槽的字节偏移。 */
#define GPGPU_EVAL_D15 192
/* 第一个内部暂存数组的 GPU 地址；通用指令不解释。 */
#define GPGPU_EVAL_AUX0 GPGPU_TENSOR_PRIVATE0
/* 复合算子的第二个 scratch 地址；块基本算子保存完整输出逻辑元素数。 */
#define GPGPU_EVAL_AUX1 GPGPU_TENSOR_ELEMENTS
/* 复合算子的第三个 scratch 地址；仅支持块的基本算子解释为后端标志。 */
#define GPGPU_EVAL_AUX2 GPGPU_TENSOR_PRIVATE1

/* 支持块的基本算子保持原标量路径。 */
#define GPGPU_EVAL_BACKEND_SCALAR 0
/* 支持块的基本算子使用通用张量指令；其余算子不解释此标志。 */
#define GPGPU_EVAL_BACKEND_BLOCK 1

/* 下列常量各用一个 8 字节槽，低 32 位保存 FP32 原始位。 */
/* 常量 1。 */
#define GPGPU_EVAL_ONE 256
/* ln(2) 的高位部分。 */
#define GPGPU_EVAL_LN2_HI 264
/* ln(2) 的低位修正。 */
#define GPGPU_EVAL_LN2_LO 272
/* log2(e)，用于指数范围缩减。 */
#define GPGPU_EVAL_LOG2E 280
/* 指数多项式二次项系数 1/2。 */
#define GPGPU_EVAL_C2 288
/* 指数多项式三次项系数 1/6。 */
#define GPGPU_EVAL_C3 296
/* 指数多项式四次项系数 1/24。 */
#define GPGPU_EVAL_C4 304
/* 指数多项式五次项系数 1/120。 */
#define GPGPU_EVAL_C5 312
/* 指数多项式六次项系数 1/720。 */
#define GPGPU_EVAL_C6 320
/* 指数内核的有限输入下界，绝对误差在用例中单独核对。 */
#define GPGPU_EVAL_EXP_LO 328
/* 指数内核的有限输入上界。 */
#define GPGPU_EVAL_EXP_HI 336
/* 正零。 */
#define GPGPU_EVAL_ZERO 344
/* 常量 -1。 */
#define GPGPU_EVAL_NEGONE 352
/* 负无穷，用作最大值归约的初值。 */
#define GPGPU_EVAL_NEGINF 360
/* 常量 2。 */
#define GPGPU_EVAL_TWO 368
/* 1/(2*pi)，用于三角函数范围缩减。 */
#define GPGPU_EVAL_INV_TWO_PI 376
/* 2*pi 的高位部分；取 6.28125，使本次位置范围内的整数乘积精确。 */
#define GPGPU_EVAL_TWO_PI_HI 384
/* 2*pi 减去高位部分的低位修正。 */
#define GPGPU_EVAL_TWO_PI_LO 392
/* pi 的 FP32 近似值。 */
#define GPGPU_EVAL_PI 400
/* pi/2 的 FP32 近似值。 */
#define GPGPU_EVAL_HALF_PI 408
/* 三角多项式的正系数 1/7!，符号由 Horner 步骤决定。 */
#define GPGPU_EVAL_INV_FACT7 416
/* 三角多项式的正系数 1/8!。 */
#define GPGPU_EVAL_INV_FACT8 424
/* 三角多项式的正系数 1/9!。 */
#define GPGPU_EVAL_INV_FACT9 432
/* 三角多项式的正系数 1/10!。 */
#define GPGPU_EVAL_INV_FACT10 440
/* 三角多项式的正系数 1/11!。 */
#define GPGPU_EVAL_INV_FACT11 448
/* 三角多项式的正系数 1/12!。 */
#define GPGPU_EVAL_INV_FACT12 456
/* 三角多项式的正系数 1/13!。 */
#define GPGPU_EVAL_INV_FACT13 464
/* 三角多项式的正系数 1/14!。 */
#define GPGPU_EVAL_INV_FACT14 472

/* arange/full 的 D2 类型：FP32，D0/D1 的低 32 位保存浮点值。 */
#define GPGPU_EVAL_TYPE_F32 0
/* arange/full 的 D2 类型：INT64，D0/D1 保存完整整数值。 */
#define GPGPU_EVAL_TYPE_I64 1
/* copy 的 D0 模式：逐元素复制 FP32 原始位。 */
#define GPGPU_EVAL_COPY_F32 GPGPU_TENSOR_COPY_F32
/* copy 的 D0 模式：INT64 转 FP32；本组位置输入须在 signed32 范围内。 */
#define GPGPU_EVAL_COPY_I64_TO_F32 GPGPU_TENSOR_COPY_I64_F32
/* copy 的 D0 模式：逐元素复制 INT64 原始位。 */
#define GPGPU_EVAL_COPY_I64 GPGPU_TENSOR_COPY_I64

/* FP32 X[M,K] 与按 NK 保存的 FP32 权重做线性变换。 */
#define GPGPU_EVAL_LINEAR_F32 1
/* INT8 NK 权重逐输出行反量化，再以 FP32 乘加。 */
#define GPGPU_EVAL_LINEAR_W8 2
/* FP32 批量矩阵乘法。 */
#define GPGPU_EVAL_BMM 3
/* 从 INT8 词嵌入矩阵读取指定行并反量化。 */
#define GPGPU_EVAL_EMBEDDING_W8 4
/* 两个 FP32 数组逐元素相加。 */
#define GPGPU_EVAL_ADD 5
/* 两个 FP32 数组逐元素相乘。 */
#define GPGPU_EVAL_MUL 6
/* FP32 数组乘标量。 */
#define GPGPU_EVAL_MUL_SCALAR 7
/* FP32 SiLU。 */
#define GPGPU_EVAL_SILU 8
/* FP32 Sigmoid。 */
#define GPGPU_EVAL_SIGMOID 9
/* 沿一个维度求最大值。 */
#define GPGPU_EVAL_REDUCE_MAX 10
/* 沿一个维度求和。 */
#define GPGPU_EVAL_REDUCE_SUM 11
/* 行平方和除以宽度、加 epsilon，再取倒数平方根。 */
#define GPGPU_EVAL_REDUCE_RMS 12
/* 减去对应行最大值，再计算指数。 */
#define GPGPU_EVAL_EXP_SHIFT 13
/* 各元素除以对应行的归约和。 */
#define GPGPU_EVAL_DIV_ROW 14
/* 输入乘行倒数范数及逐通道 gamma。 */
#define GPGPU_EVAL_RMS_APPLY 15
/* 使用冻结的均值、方差、gamma、beta 做推理态归一化。 */
#define GPGPU_EVAL_BATCHNORM 16
/* NCHW 输入与 OIHW 权重的二维卷积，支持分组。 */
#define GPGPU_EVAL_CONV2D 17
/* 二维最大池化。 */
#define GPGPU_EVAL_MAXPOOL2D 18
/* 二维 nearest 上采样。 */
#define GPGPU_EVAL_NEAREST2D 19
/* 沿一个维度拼接输入。 */
#define GPGPU_EVAL_CONCAT 20
/* 按给定布局复制数据。 */
#define GPGPU_EVAL_COPY 21
/* split-half RoPE，cos/sin 由模型配置和位置生成。 */
#define GPGPU_EVAL_ROPE 22
/* 对 DFL 概率沿分布维度做带编号权重的求和。 */
#define GPGPU_EVAL_DFL_PROJECT 23
/* 按锚点及步长解码中心、宽高。 */
#define GPGPU_EVAL_BOX_DECODE 24
/* FP32 数组加标量。 */
#define GPGPU_EVAL_ADD_SCALAR 25
/* 两个 FP32 数组逐元素相减。 */
#define GPGPU_EVAL_SUB 26
/* 两个 FP32 数组逐元素相除。 */
#define GPGPU_EVAL_DIV 27
/* FP32 正弦，用于真实位置频率张量。 */
#define GPGPU_EVAL_SIN 28
/* FP32 余弦，用于真实位置频率张量。 */
#define GPGPU_EVAL_COS 29
/* 以起点、步长及全局编号生成 FP32 或 INT64 序列。 */
#define GPGPU_EVAL_ARANGE 30
/* 将 FP32 或 INT64 填充值写入每个工作项。 */
#define GPGPU_EVAL_FULL 31

/* 以下编号只供回放器组织多次派发，不直接交给解释器执行。 */
/* Softmax：最大值、指数、求和、除法。 */
#define GPGPU_EVAL_SOFTMAX 128
/* RMSNorm：行倒数范数与逐元素缩放。 */
#define GPGPU_EVAL_RMSNORM 129
/* DFL：Softmax 后沿分布维做投影。 */
#define GPGPU_EVAL_DFL 130

#endif /* GPGPU_EVAL_ABI_H */
