/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 张量块扩展：受限矩阵乘加与布局传输的通用参数前缀
 *
 * 计算线程传入自己的 GPR，不直接传宿主机指针：
 *
 *   rs1：GPU 参数地址 ─用户访存─▶ 本前缀 ─逐行读取─▶ 有界局部数组
 *   rs2：线性块号       rs3：K 起点（矩阵）       └─计算/重排─▶ GPU 输出
 *
 * 所有字段为小端 u64，地址与派发参数同域；参数地址按 8 字节对齐。
 * 前缀不含模型名称，也不使用评估程序的操作编号。数学指令固定 RNE，
 * 每个输出按 K 递增逐次 FP32 FMA，W8 先逐项 FP32 乘 scale；末块加 bias。
 * 所有输入先读完再算，输出逐行发布；后行故障不回退先前的写入。
 *
 * 指令采用 custom-0 的 R4 编码：rd 必须为 0，funct3 选择类别，
 * fmt[26:25] 选择类别内格式，rs3[31:27] 是第三个 GPR 编号。
 * 矩阵指令的 rs3 为 K 起点，布局指令的 rs3 必须为 x0。
 *
 * 矩阵 DIMS：
 *   LINEAR：M,N,K；输入 0=X、1=W[N,K]、2=可选 W8 scale[N]、3=可选 bias[N]。
 *   BMM：B,M,N,K，D4..6 为 A 元素 stride，D7..9 为 B 元素 stride，
 *        D10 非零才采用这些 stride；否则按紧凑 [B,M,K]、[B,K,N]。
 *   CONV：Cin,Hin,Win,Cout,Hout,Wout,Kh,Kw,Sh,Sw,Ph,Pw,Dh,Dw,groups；
 *         输入 0=NCHW、1=卷积权重、2=可选 bias；总元素数用于推导 batch。
 *         每组独立划分输出通道块，M 轴为每张图的空间位置。
 *
 * 布局 DIMS：
 *   COPY：D0 为 COPY_*，D1 为秩；D2 起依次保存 shape 和元素 stride。
 *         秩 0 表示连续；非连续输入最多 7 维；输出始终连续。
 *   CONCAT：D0=inner、D1=输出轴宽、D2=输入数，D3..8 为各输入轴宽；
 *           输入与输出均为 FP32，STACK 也可按轴宽 1 表示。
 *   NEAREST：D0=Hin、D1=Win、D2=Hout、D3=Wout；输入与输出均为 NCHW FP32。
 *
 * 布局每块最多处理 BLOCK_ELEMENTS 个逻辑元素；先按原 stride 读入有界
 * 字节缓冲，再写连续输出，只有确实相邻的地址才合并成块访问。
 */
#ifndef GPGPU_TENSOR_ABI_H
#define GPGPU_TENSOR_ABI_H

/* R4 custom-0 主操作码。 */
#define GPGPU_TENSOR_OPCODE 0x0b
/* funct3=0：受限矩阵乘加，固定 RNE。 */
#define GPGPU_TENSOR_KIND_MATRIX 0
/* funct3=1：受限布局传输。 */
#define GPGPU_TENSOR_KIND_LAYOUT 1
/* 矩阵 fmt=0：FP32 线性投影。 */
#define GPGPU_TENSOR_LINEAR_F32 0
/* 矩阵 fmt=1：INT8 权重、逐输出行 scale、FP32 计算。 */
#define GPGPU_TENSOR_LINEAR_W8 1
/* 矩阵 fmt=2：FP32 批次矩阵乘。 */
#define GPGPU_TENSOR_BMM 2
/* 矩阵 fmt=3：FP32 NCHW 分组卷积。 */
#define GPGPU_TENSOR_CONV 3
/* 布局 fmt=0：复制或 INT64 到 FP32 转换。 */
#define GPGPU_TENSOR_COPY 0
/* 布局 fmt=1：FP32 拼接。 */
#define GPGPU_TENSOR_CONCAT 1
/* 布局 fmt=2：FP32 nearest 上采样。 */
#define GPGPU_TENSOR_NEAREST 2

/* 每条矩阵指令最多处理的行数。 */
#define GPGPU_TENSOR_TILE_M 8
/* 每条矩阵指令最多处理的输出列数。 */
#define GPGPU_TENSOR_TILE_N 32
/* 每条矩阵指令最多处理的归约元素数。 */
#define GPGPU_TENSOR_TILE_K 32
/* 每条布局指令最多处理的逻辑元素数。 */
#define GPGPU_TENSOR_BLOCK_ELEMENTS 32
/* 布局输入允许的最大秩。 */
#define GPGPU_TENSOR_MAX_RANK 7
/* 通用参数前缀的字节数。 */
#define GPGPU_TENSOR_PARAM_BYTES 224
/* 每个地址或整数字段的字节数。 */
#define GPGPU_TENSOR_SLOT_BYTES 8
/* 由调用者使用的标识字段；指令不解释。 */
#define GPGPU_TENSOR_OPAQUE 0
/* 输入 0 的 GPU 地址字节偏移；未使用时允许为 0。 */
#define GPGPU_TENSOR_IN0 8
/* 输入 1 的 GPU 地址字节偏移。 */
#define GPGPU_TENSOR_IN1 16
/* 输入 2 的 GPU 地址字节偏移；可选权重 scale 或 bias。 */
#define GPGPU_TENSOR_IN2 24
/* 输入 3 的 GPU 地址字节偏移；Linear 的可选 bias。 */
#define GPGPU_TENSOR_IN3 32
/* 输入 4 的 GPU 地址字节偏移。 */
#define GPGPU_TENSOR_IN4 40
/* 输入 5 的 GPU 地址字节偏移。 */
#define GPGPU_TENSOR_IN5 48
/* 连续输出的 GPU 地址；CONV 按 NCHW 解释。 */
#define GPGPU_TENSOR_OUT 56
/* 调用者的工作项计数；指令不解释。 */
#define GPGPU_TENSOR_WORK_COUNT 64
/* 16 个维度/stride 字段的首字节偏移。 */
#define GPGPU_TENSOR_DIMS 72
/* 调用者私有槽，不由指令解释。 */
#define GPGPU_TENSOR_PRIVATE0 200
/* 完整输出的逻辑元素数；不是分块后的工作项数。 */
#define GPGPU_TENSOR_ELEMENTS 208
/* 调用者私有槽；评估 Kernel 用于选择块指令后端。 */
#define GPGPU_TENSOR_PRIVATE1 216

/* COPY 的 FP32 原始位搬运。 */
#define GPGPU_TENSOR_COPY_F32 0
/* COPY 的完整有符号 INT64 到 FP32 转换，固定 RNE。 */
#define GPGPU_TENSOR_COPY_I64_F32 1
/* COPY 的 INT64 原始位搬运。 */
#define GPGPU_TENSOR_COPY_I64 2

#endif /* GPGPU_TENSOR_ABI_H */
