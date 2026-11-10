// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 张量块：经用户访存执行有界矩阵乘加与布局传输
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__FAST_MATH__)
#include <fenv.h>
#include <immintrin.h>
#endif
#include "hw/gpgpu/mem/gpuvm.h"
#include "gpgpu/gpgpu_tensor_abi.h"
#include "tensor.h"

/* 前缀中的输入地址个数，不解释调用者的私有槽。 */
#define TENSOR_INPUTS ((GPGPU_TENSOR_OUT - GPGPU_TENSOR_IN0) / GPGPU_TENSOR_SLOT_BYTES)
/* 前缀中的维度槽数，单位：u64。 */
#define TENSOR_DIMS ((GPGPU_TENSOR_PRIVATE0 - GPGPU_TENSOR_DIMS) / GPGPU_TENSOR_SLOT_BYTES)
/* Linear/BMM 与卷积之间较大的写回行数和行宽，单位：元素。 */
#define TENSOR_ROW_MAX MAX_CONST(GPGPU_TENSOR_TILE_M, GPGPU_TENSOR_TILE_N)
/* 一次矩阵输入向量的最大元素数，覆盖 N 与 K。 */
#define TENSOR_VECTOR_MAX MAX_CONST(TENSOR_ROW_MAX, GPGPU_TENSOR_TILE_K)

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__FAST_MATH__)
/*
 * 仅为局部函数生成 AVX2/FMA，
 * 不改变其他设备代码的指令集。
 */
#define TENSOR_NATIVE_X86 1
/* 一个 AVX2 向量的 FP32 元素数。 */
#define TENSOR_NATIVE_WIDTH 8
/* 局部 SIMD 行宽向上补齐，补位不对应 GPU 访问。 */
#define TENSOR_NATIVE_COLUMNS \
	((GPGPU_TENSOR_TILE_N + TENSOR_NATIVE_WIDTH - 1) / TENSOR_NATIVE_WIDTH * \
	 TENSOR_NATIVE_WIDTH)
/* MXCSR 的六个异常状态位。 */
#define TENSOR_MXCSR_FLAGS 0x003fU
/* MXCSR 的 precision/inexact 状态位。 */
#define TENSOR_MXCSR_INEXACT (1U << 5)
/* MXCSR 的全部异常屏蔽位。 */
#define TENSOR_MXCSR_MASKS 0x1f80U
/* MXCSR 的舍入控制位；清零表示 RNE。 */
#define TENSOR_MXCSR_ROUND 0x6000U
/* MXCSR 的 DAZ 控制位，必须关闭以保留 IEEE 输入语义。 */
#define TENSOR_MXCSR_DAZ (1U << 6)
/* MXCSR 的 FTZ 控制位，必须关闭以检测真实的微小结果。 */
#define TENSOR_MXCSR_FTZ (1U << 15)
#else
/* 非 x86_64 或 fast-math 构建只使用 SoftFloat。 */
#define TENSOR_NATIVE_X86 0
#endif

/**
 * struct TensorCommand - 通用前缀的局部快照
 * @in: 输入 GPU 地址，未使用项允许为零。
 * @out: 输出 GPU 地址，按实际元素宽度对齐。
 * @dims: 维度或元素 stride，按类别解释；不含调用者私有槽。
 * @elements: 完整输出逻辑元素数，不是工作块数。
 */
typedef struct TensorCommand {
	uint64_t in[TENSOR_INPUTS];
	uint64_t out;
	uint64_t dims[TENSOR_DIMS];
	uint64_t elements;
} TensorCommand;

/**
 * struct TensorMatrix - 矩阵维度、块坐标与有界输入快照
 * @format: GPGPU_TENSOR_* 矩阵格式。
 * @m: 完整逻辑行数；卷积为 Hout*Wout。
 * @n: 完整逻辑列数；卷积为组内 Cout。
 * @k: 完整归约元素数。
 * @batch: 当前批次编号。
 * @group: 当前卷积分组编号；其他格式为零。
 * @m_base: 本块在完整 M 轴的起始元素编号。
 * @n_base: 本块起始列；卷积已加上组的通道起点。
 * @k_base: 本块归约起点，零表示不读取旧输出。
 * @a_stride: A 的 batch/M/K 元素 stride，零表示广播。
 * @b_stride: BMM 的 batch/K/N 元素 stride，零表示广播。
 * @input_plane: 卷积单输入通道的空间元素数。
 * @kernel_plane: 卷积核的 Kh*Kw 元素数。
 * @cin_group: 每个卷积分组的输入通道数。
 * @rows: 本块有效 M 元素数，不超过 TILE_M。
 * @cols: 本块有效 N 元素数，不超过 TILE_N。
 * @depth: 本块有效 K 元素数，不超过 TILE_K。
 * @last: 本块已覆盖剩余 K，允许加入偏置。
 * @has_bias: 本块需要读取和加入偏置。
 * @out_addr: 实际写回行的 GPU 地址；卷积的一行沿空间轴。
 * @a: A 的 FP32 原始位。
 * @b: B 的 FP32 原始位；W8 在输入读完后反量化填入。
 * @seed: 旧 C 的 FP32 原始位，首 K 块保持正零。
 * @scale: W8 各列 scale 的 FP32 原始位。
 * @bias: 各列偏置的 FP32 原始位。
 * @w8: 真正从 GPU 读到的有符号 INT8 权重，不是 FP32 替代数据。
 */
typedef struct TensorMatrix {
	uint32_t format;
	uint64_t m;
	uint64_t n;
	uint64_t k;
	uint64_t batch;
	uint64_t group;
	uint64_t m_base;
	uint64_t n_base;
	uint64_t k_base;
	uint64_t a_stride[3];
	uint64_t b_stride[3];
	uint64_t input_plane;
	uint64_t kernel_plane;
	uint64_t cin_group;
	unsigned int rows;
	unsigned int cols;
	unsigned int depth;
	bool last;
	bool has_bias;
	uint64_t out_addr[TENSOR_ROW_MAX];
	uint32_t a[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_K];
	uint32_t b[GPGPU_TENSOR_TILE_K][GPGPU_TENSOR_TILE_N];
	uint32_t seed[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N];
	uint32_t scale[GPGPU_TENSOR_TILE_N];
	uint32_t bias[GPGPU_TENSOR_TILE_N];
	int8_t w8[GPGPU_TENSOR_TILE_K][GPGPU_TENSOR_TILE_N];
} TensorMatrix;

/**
 * struct TensorLayout - 一个有界布局块的输入地址
 * @first: 本块首个逻辑元素编号。
 * @out: 连续输出的 GPU 地址。
 * @input_plane: NEAREST 输入单平面的空间元素数。
 * @output_plane: NEAREST 输出单平面的空间元素数。
 * @concat_span: CONCAT 每个 outer 的输出元素数。
 * @count: 本块有效元素数，不超过 BLOCK_ELEMENTS。
 * @input_size: 单个输入元素的字节数，为 4 或 8。
 * @output_size: 单个输出元素的字节数，为 4 或 8。
 * @convert: 执行有符号 INT64 到 FP32 转换，否则只搬运原始位。
 * @addr: 按逻辑元素顺序排列的 GPU 输入地址，不推导未访问空洞。
 */
typedef struct TensorLayout {
	uint64_t first;
	uint64_t out;
	uint64_t input_plane;
	uint64_t output_plane;
	uint64_t concat_span;
	unsigned int count;
	unsigned int input_size;
	unsigned int output_size;
	bool convert;
	uint64_t addr[GPGPU_TENSOR_BLOCK_ELEMENTS];
} TensorLayout;

/* 除数由 ABI 常量或已检查的非零维度提供，不以 value+divisor-1 求上取整。 */
static uint64_t tensor_ceil_div(uint64_t value, uint64_t divisor)
{
	return value / divisor + !!(value % divisor);
}

/* 三个非负维度的乘积必须能完整放入 u64。 */
static uint32_t tensor_product3(uint64_t a, uint64_t b, uint64_t c, uint64_t *result)
{
	if (umul64_overflow(a, b, result) || umul64_overflow(*result, c, result))
		return GPGPU_VM_FAULT_RANGE;
	return 0;
}

/*
 * tensor_index3() - 检查三维元素 stride 的仿射索引
 *
 * 寻址调用者传入非负坐标与元素 stride，零 stride 保留为广播：
 *
 *   x/y/z ─分别乘 sx/sy/sz─▶ 三个元素偏移 ─求和─▶ 源元素索引
 *
 * 例：[1,2,3]、stride=[12,4,1] 得到索引 23，之后才乘元素字节数。
 * 1. 从 x*sx 开始，每项乘法和累加都检查溢出，失败即返回 RANGE；
 * 2. 完整索引交给调用者，不检查字节地址或内存权限。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_index3(uint64_t x, uint64_t sx, uint64_t y, uint64_t sy,
			      uint64_t z, uint64_t sz, uint64_t *result)
{
	uint64_t term;

	/*
	 * 1. 输出参数可与一个输入字段同址。
	 *    入参均已按值保存。
	 */
	if (umul64_overflow(x, sx, result) || umul64_overflow(y, sy, &term) ||
	    uadd64_overflow(*result, term, result) || umul64_overflow(z, sz, &term) ||
	    uadd64_overflow(*result, term, result))
		return GPGPU_VM_FAULT_RANGE;

	/* 2. 元素索引与字节地址分开，不能遗漏后续的元素宽度换算。 */
	return 0;
}

/* 数值或地址检查失败也归属到现有的来源槽，保留该槽的首次故障。 */
static uint32_t tensor_range_error(GPGPUExecContext *ctx, uint64_t addr, uint32_t access)
{
	const GPGPUKernelParams *p = &ctx->params;

	gpgpu_vm_record_fault(ctx->device, p->vmid, p->source_slot, addr, access,
			      GPGPU_VM_PHASE_DATA, GPGPU_VM_FAULT_RANGE);
	return GPGPU_EXEC_ERR_MEMORY;
}

/*
 * tensor_user_rw() - 保留元素对齐后提交真实的用户访存请求
 *
 * 参数读取、矩阵行和布局块均经本函数，不能取得 VRAM 的宿主指针。
 * 用户 API 对大于 8 字节的块不检查元素对齐，故必须在此检查；小块还要
 * 避免把两个合法的 FP32 元素合并成一个不按 8 字节对齐的请求。
 * 例：地址 0x1004 的两个 FP32 元素应发出两个 4B 请求，而不是一个 8B 请求。
 *
 * 1. 检查元素对齐、完整字节范围和长度，否则记录 RANGE；
 * 2. 大块直接提交；不满足 API 标量规则的小块按元素拆分，逐请求统计；
 * 3. 首个失败立即返回，保留 API 已完成的页或前面的请求。
 *
 * 不探测权限、不回滚数据，权限和保护区仍由 gpgpu_user_rw() 检查。
 * Return: 0 表示全部完成，否则为 GPGPU_EXEC_ERR_MEMORY。
 */
static uint32_t tensor_user_rw(GPGPUExecContext *ctx, uint64_t addr, void *buf, size_t len,
			       unsigned int element_size, uint32_t access)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t *bytes = buf;

	/* 1. 块长超过 8B 也必须保留原元素的 4B/8B 对齐。 */
	if (!len || len % element_size || (addr & (element_size - 1)) ||
	    len - 1 > UINT64_MAX - addr)
		return tensor_range_error(ctx, addr, access);

	/* 2. 统计的是实际 API 请求，包含 API 返回失败的请求。 */
	while (len) {
		size_t chunk = len;

		if (chunk <= sizeof(uint64_t) &&
		    (!is_power_of_2(chunk) || (addr & (chunk - 1))))
			chunk = element_size;
		if (ctx->profile) {
			if (access == GPGPU_VM_READ) {
				ctx->profile->data_reads++;
				ctx->profile->read_bytes += chunk;
			} else {
				ctx->profile->data_writes++;
				ctx->profile->write_bytes += chunk;
			}
		}

		/* 3. 后页或后请求失败不回退前面的访问。 */
		if (!gpgpu_user_rw(ctx->device, GPGPU_MEM_EXECUTOR, p->vmid, p->source_slot,
				   addr, bytes, chunk, access, GPGPU_VM_PHASE_DATA))
			return GPGPU_EXEC_ERR_MEMORY;
		len -= chunk;
		if (len) {
			addr += chunk;
			bytes += chunk;
		}
	}
	return 0;
}

/*
 * tensor_address() - 把元素索引安全地换算成 GPU 字节地址
 *
 *   index ─乘 size─▶ 字节偏移 ─加 base─▶ GPU 地址
 *   GPU 地址 ─加 size-1─▶ 元素末字节
 *
 * 例：base=0x1000、index=3、size=4 得到 [0x100c,0x100f]。
 * 1. 检查元素基址对齐、乘加和末字节范围，失败记录来源槽的 RANGE；
 * 2. 返回起始 GPU 地址，实际权限和保护区检查留给用户访存。
 * Return: 0 表示成功，否则为 GPGPU_EXEC_ERR_MEMORY。
 */
static uint32_t tensor_address(GPGPUExecContext *ctx, uint64_t base, uint64_t index,
			       unsigned int size, uint32_t access, uint64_t *addr)
{
	uint64_t offset;

	/* 1. 即使后续会合并成长块，也不能放宽单个元素的对齐。 */
	if ((base & (size - 1)) || umul64_overflow(index, size, &offset) ||
	    uadd64_overflow(base, offset, addr) || *addr > UINT64_MAX - (size - 1))
		return tensor_range_error(ctx, base, access);

	/* 2. 不把用户地址翻译成可绕过 GPUVM 的宿主指针。 */
	return 0;
}

/*
 * tensor_gather() - 只合并逐项证明相邻的输入地址
 *
 * 矩阵向量和布局块均传入已生成的有界地址表。
 * 输出字节按逻辑元素顺序排列。
 * 例：0x1000、0x1008、0x1004 的三个 FP32 不能按首尾跨度读成连续区间。
 *
 * 1. 从当前项向后逐对检查地址差是否恰好等于元素宽度；
 * 2. 只读取这一段真实连续的元素，失败立即停止，不读取后续项。
 *
 * 本函数不做浮点解释，NaN 和负零的原始位不变。
 * Return: 0 表示全部输入读完，否则为执行错误码。
 */
static uint32_t tensor_gather(GPGPUExecContext *ctx, const uint64_t *addr, unsigned int count,
			      unsigned int size, uint8_t *raw)
{
	unsigned int first = 0;

	while (first < count) {
		unsigned int end = first + 1;
		uint32_t error;

		/* 1. 每一对都检查，不跨过 stride、padding 或其他地址空洞。 */
		while (end < count && addr[end - 1] <= UINT64_MAX - size &&
		       addr[end] == addr[end - 1] + size)
			end++;

		/* 2. 只统计并提交这一段实际访问。 */
		error = tensor_user_rw(ctx, addr[first], raw + first * size,
				       (end - first) * size, size, GPGPU_VM_READ);
		if (error)
			return error;
		first = end;
	}
	return 0;
}

/*
 * tensor_vector_read() - 把一个有界元素 stride 向量读入字节缓冲
 *
 * 矩阵读取调用，count 不超过 TENSOR_VECTOR_MAX。stride 的单位是元素。
 * 例：FP32 的 index=3、stride=2 访问 base+12、base+20，而不是读取中间空洞。
 *
 * 1. 检查每项的元素索引和 GPU 地址，溢出时不提交本向量；
 * 2. 按地址表聚合真正相邻的请求，零 stride 则重复访问同一元素。
 *
 * 不解释字节中的 FP32 或 INT8，计算发生在全部输入读取之后。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_vector_read(GPGPUExecContext *ctx, uint64_t base, uint64_t index,
				   uint64_t stride, unsigned int count, unsigned int size,
				   uint8_t *raw)
{
	uint64_t addr[TENSOR_VECTOR_MAX];

	/* 1. 不以首尾索引的差代替逐项地址检查。 */
	for (unsigned int i = 0; i < count; i++) {
		uint64_t element;
		uint32_t error;

		if (tensor_index3(i, stride, index, 1, 0, 0, &element))
			return tensor_range_error(ctx, base, GPGPU_VM_READ);
		error = tensor_address(ctx, base, element, size, GPGPU_VM_READ, &addr[i]);
		if (error)
			return error;
	}

	/* 2. 空归约不发请求；其他向量只访问地址表中的元素。 */
	return tensor_gather(ctx, addr, count, size, raw);
}

/*
 * tensor_command_load() - 从 GPU 地址读取固定参数前缀
 *
 * 公共入口在类别/格式检查之后调用，不使用评估器操作编号或扩展字段。
 * 1. 按 8B 对齐要求读取固定 PARAM_BYTES；失败不解释不完整前缀；
 * 2. 从字节缓冲逐个解码小端 u64，忽略 opaque、工作项数和私有槽。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_command_load(GPGPUExecContext *ctx, uint64_t command, TensorCommand *cmd)
{
	uint8_t raw[GPGPU_TENSOR_PARAM_BYTES];
	uint32_t error;

	/* 1. 读到局部字节数组，不把 GPU 数据直接强转成宿主结构体。 */
	error = tensor_user_rw(ctx, command, raw, sizeof(raw), GPGPU_TENSOR_SLOT_BYTES,
			       GPGPU_VM_READ);
	if (error)
		return error;

	/* 2. 只解码通用布局规定的字段。 */
	for (unsigned int i = 0; i < TENSOR_INPUTS; i++)
		cmd->in[i] = ldq_le_p(raw + GPGPU_TENSOR_IN0 + i * GPGPU_TENSOR_SLOT_BYTES);
	for (unsigned int i = 0; i < TENSOR_DIMS; i++)
		cmd->dims[i] = ldq_le_p(raw + GPGPU_TENSOR_DIMS + i * GPGPU_TENSOR_SLOT_BYTES);
	cmd->out = ldq_le_p(raw + GPGPU_TENSOR_OUT);
	cmd->elements = ldq_le_p(raw + GPGPU_TENSOR_ELEMENTS);
	return 0;
}

/*
 * tensor_matrix_block() - 从线性块号得到独立的 batch/group/M/N 坐标
 *
 * 各矩阵格式先设置 m/n/k；卷积的 n 是组内通道数，因此不能跨组共享 A。
 * 块号展开次序为 batch、group、ceil(M/8)、ceil(N/32)，N 块变化最快。
 * 例：M=9、N=33 时，每组有 2*2 个块，块 3 的起点为 M=8、N=32。
 *
 * 1. 检查总块数和 block 范围，不把越界工作项当作空块；
 * 2. 拆出坐标和边缘块大小；
 * 3. 检查 K 起点，初块从零累加，末块由剩余 K 决定。
 *
 * 不要求 k_base 是 TILE_K 的倍数；ABI 只规定起点与最多处理的 K 数。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_matrix_block(TensorMatrix *t, uint64_t batches, uint64_t groups,
				    uint64_t block, uint64_t k_base)
{
	uint64_t m_blocks;
	uint64_t n_blocks;
	uint64_t per_group;
	uint64_t per_batch;
	uint64_t total;
	uint64_t local;

	/* 1. 零 M/N 没有可执行的块；总 K 为零则仍可以输出零加偏置。 */
	if (!t->m || !t->n || !batches || !groups)
		return GPGPU_VM_FAULT_RANGE;
	m_blocks = tensor_ceil_div(t->m, GPGPU_TENSOR_TILE_M);
	n_blocks = tensor_ceil_div(t->n, GPGPU_TENSOR_TILE_N);
	if (umul64_overflow(m_blocks, n_blocks, &per_group) ||
	    umul64_overflow(per_group, groups, &per_batch) ||
	    umul64_overflow(per_batch, batches, &total) || block >= total)
		return GPGPU_VM_FAULT_RANGE;

	/*
	 * 2. 先拆组，再求组内 N 块。
	 *    不同组的卷积通道不能合入同一块。
	 */
	t->batch = block / per_batch;
	t->group = (block % per_batch) / per_group;
	local = block % per_group;
	if (umul64_overflow(local / n_blocks, GPGPU_TENSOR_TILE_M, &t->m_base) ||
	    umul64_overflow(local % n_blocks, GPGPU_TENSOR_TILE_N, &t->n_base))
		return GPGPU_VM_FAULT_RANGE;
	t->rows = MIN(t->m - t->m_base, GPGPU_TENSOR_TILE_M);
	t->cols = MIN(t->n - t->n_base, GPGPU_TENSOR_TILE_N);

	/* 3. 非空归约不接受 k_base==K，空归约只接受唯一的起点 0。 */
	if ((!t->k && k_base) || (t->k && k_base >= t->k))
		return GPGPU_VM_FAULT_RANGE;
	t->k_base = k_base;
	t->depth = MIN(t->k - k_base, GPGPU_TENSOR_TILE_K);
	t->last = t->depth == t->k - k_base;
	return 0;
}

/*
 * tensor_linear_shape() - 检查 Linear 的 [M,K] 输入与 [N,K] 权重
 *
 * 1. 检查三个维度及其完整乘积，ELEMENTS 必须恰好等于 M*N；
 * 2. 展开 M/N/K 块，保存连续 K stride，只在末块采用 IN3 的 bias。
 *
 * 例：M=9、N=33、K=40 时，输出 ELEMENTS 是 297，不是四个 M/N 工作块。
 * 此处不读取 FP32 或 W8 权重，两者共享相同的逻辑形状。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_linear_shape(const TensorCommand *cmd, TensorMatrix *t,
				    uint64_t block, uint64_t k_base)
{
	uint64_t elements;
	uint64_t unused;
	uint32_t error;

	/* 1. 维度按 ABI 的 u64 解释，不使用工作项数替代完整输出元素数。 */
	t->m = cmd->dims[0];
	t->n = cmd->dims[1];
	t->k = cmd->dims[2];
	if (umul64_overflow(t->m, t->n, &elements) || elements != cmd->elements ||
	    umul64_overflow(t->m, t->k, &unused) || umul64_overflow(t->n, t->k, &unused))
		return GPGPU_VM_FAULT_RANGE;

	/* 2. 首块从零累加，末块才读取并加 bias。 */
	error = tensor_matrix_block(t, 1, 1, block, k_base);
	if (error)
		return error;
	t->a_stride[1] = t->k;
	t->a_stride[2] = 1;
	t->has_bias = t->last && cmd->in[3];
	return 0;
}

/*
 * tensor_bmm_shape() - 检查批次矩阵和显式元素 stride
 *
 * A 的逻辑维度是 [B,M,K]，B 是 [B,K,N]。D10 非零才使用 D4..9；
 * 显式 stride 为零必须保留，不能替换成紧凑 stride。
 *
 * 1. 检查维度、输出数和逻辑输入乘积；
 * 2. 选择显式或紧凑 stride，并检查非空输入的最大仿射索引；
 * 3. 展开 batch/M/N 块和 K 起点。
 *
 * 不检查未访问地址的权限，也不读取 stride 间的空洞。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_bmm_shape(const TensorCommand *cmd, TensorMatrix *t,
				 uint64_t block, uint64_t k_base)
{
	const uint64_t *d = cmd->dims;
	uint64_t elements;
	uint64_t unused;

	/* 1. 逻辑输入即使广播，维度乘积也不能在计算中回绕。 */
	t->m = d[1];
	t->n = d[2];
	t->k = d[3];
	if (!d[0] || !t->m || !t->n ||
	    tensor_product3(d[0], t->m, t->n, &elements) || elements != cmd->elements ||
	    tensor_product3(d[0], t->m, t->k, &unused) ||
	    tensor_product3(d[0], t->k, t->n, &unused))
		return GPGPU_VM_FAULT_RANGE;

	/* 2. 六个 stride 按 ABI 的非负 u64 元素数解释，保留合法的零 stride。 */
	if (d[10]) {
		for (unsigned int i = 0; i < 3; i++) {
			t->a_stride[i] = d[4 + i];
			t->b_stride[i] = d[7 + i];
		}
	} else {
		if (umul64_overflow(t->m, t->k, &t->a_stride[0]) ||
		    umul64_overflow(t->k, t->n, &t->b_stride[0]))
			return GPGPU_VM_FAULT_RANGE;
		t->a_stride[1] = t->k;
		t->a_stride[2] = 1;
		t->b_stride[1] = t->n;
		t->b_stride[2] = 1;
	}
	if (t->k &&
	    (tensor_index3(d[0] - 1, t->a_stride[0], t->m - 1, t->a_stride[1],
			   t->k - 1, t->a_stride[2], &unused) ||
	     tensor_index3(d[0] - 1, t->b_stride[0], t->k - 1, t->b_stride[1],
			   t->n - 1, t->b_stride[2], &unused)))
		return GPGPU_VM_FAULT_RANGE;

	/* 3. BMM 没有独立 bias 输入。 */
	return tensor_matrix_block(t, d[0], 1, block, k_base);
}

/*
 * tensor_conv_axis() - 检查单个卷积轴的完整输出长度
 *
 *   input+2*pad ─减去 ((kernel-1)*dilation+1)─▶ 可滑动长度
 *   可滑动长度 ─除 stride、加 1─▶ output
 *
 * 例：input=7、kernel=3、dilation=2、pad=1、stride=2，得到 output=3。
 * 1. 检查正维度和中间乘加，padding 后仍容不下卷积核时拒绝；
 * 2. 用整数下取整核对传入的输出长度，不读取任何数据。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_conv_axis(uint64_t input, uint64_t output, uint64_t kernel,
				 uint64_t stride, uint64_t pad, uint64_t dilation)
{
	uint64_t padded;
	uint64_t effective;

	/* 1. 先保护中间值，再相减，避免无符号下溢。 */
	if (!input || !output || !kernel || !stride || !dilation ||
	    umul64_overflow(pad, 2, &padded) || uadd64_overflow(input, padded, &padded) ||
	    umul64_overflow(kernel - 1, dilation, &effective) ||
	    uadd64_overflow(effective, 1, &effective) || padded < effective)
		return GPGPU_VM_FAULT_RANGE;

	/* 2. effective 至少为 1，故此处最后的 +1 也不会回绕。 */
	return (padded - effective) / stride + 1 == output ? 0 : GPGPU_VM_FAULT_RANGE;
}

/*
 * tensor_conv_shape() - 建立 NCHW 分组卷积的块坐标
 *
 * DIMS 的 Cin/Hin/Win/Cout/Hout/Wout/Kh/Kw/stride/pad/dilation/groups
 * 完全来自通用 ABI，batch 由 ELEMENTS/(Cout*Hout*Wout) 推导。
 * 例：Cin=Cout=4、groups=4 时，每组 K=Kh*Kw，只读取自己的一个输入通道。
 *
 * 1. 检查组整除关系、输出轴公式和所有完整维度乘积；
 * 2. 按 batch/group/空间块/组内通道块展开；
 * 3. 把组内通道起点换算成全局 Cout 起点，只在最后 K 块使用 bias。
 *
 * 不生成完整 im2col，也不把其他组的输入放入本块的 A。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_conv_shape(const TensorCommand *cmd, TensorMatrix *t,
				  uint64_t block, uint64_t k_base)
{
	const uint64_t *d = cmd->dims;
	uint64_t image_elements;
	uint64_t batches;
	uint64_t unused;
	uint32_t error;

	/* 1. pad 允许为零，其他空间参数及组数均须为正。 */
	if (!d[0] || !d[3] || !d[14] || d[0] % d[14] || d[3] % d[14] ||
	    tensor_conv_axis(d[1], d[4], d[6], d[8], d[10], d[12]) ||
	    tensor_conv_axis(d[2], d[5], d[7], d[9], d[11], d[13]))
		return GPGPU_VM_FAULT_RANGE;
	t->cin_group = d[0] / d[14];
	t->n = d[3] / d[14];
	if (umul64_overflow(d[4], d[5], &t->m) ||
	    umul64_overflow(d[1], d[2], &t->input_plane) ||
	    umul64_overflow(d[6], d[7], &t->kernel_plane) ||
	    umul64_overflow(t->cin_group, t->kernel_plane, &t->k) ||
	    umul64_overflow(d[3], t->m, &image_elements) || !image_elements ||
	    !cmd->elements || cmd->elements % image_elements)
		return GPGPU_VM_FAULT_RANGE;
	batches = cmd->elements / image_elements;
	if (tensor_product3(batches, d[0], t->input_plane, &unused) ||
	    umul64_overflow(d[3], t->k, &unused))
		return GPGPU_VM_FAULT_RANGE;

	/* 2. 每组独立切 N 块，组尾不足 32 列也不会借用下一组通道。 */
	error = tensor_matrix_block(t, batches, d[14], block, k_base);
	if (error)
		return error;

	/* 3. 全局输出通道只用于权重、bias 和 NCHW 输出寻址。 */
	if (tensor_index3(t->group, t->n, t->n_base, 1, 0, 0, &t->n_base))
		return GPGPU_VM_FAULT_RANGE;
	t->has_bias = t->last && cmd->in[2];
	return 0;
}

/*
 * tensor_matrix_output() - 保存真实连续输出行的 GPU 地址
 *
 * Linear/BMM 按 [B,M,N] 写 N 元素；卷积按 [B,Cout,Hout*Wout] 写空间元素。
 * 例：卷积的 (batch=1, channel=2, space=3) 索引为 (1*Cout+2)*Hout*Wout+3。
 *
 * 1. 按格式计算每行的元素起点，所有乘加都检查回绕；
 * 2. 转成按 4B 对齐的 GPU 地址，不提前读写输出或探测页权限。
 *
 * 输出的跨页失败与前行保留语义由逐行读写实现。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_matrix_output(GPGPUExecContext *ctx, const TensorCommand *cmd,
				     TensorMatrix *t)
{
	bool conv = t->format == GPGPU_TENSOR_CONV;
	unsigned int rows = conv ? t->cols : t->rows;

	for (unsigned int row = 0; row < rows; row++) {
		uint64_t index;
		uint32_t error;

		/*
		 * 1. 卷积沿空间轴形成连续行。
		 *    相邻输出通道不视为连续。
		 */
		if (conv) {
			if (tensor_index3(t->batch, cmd->dims[3], t->n_base + row, 1,
					   0, 0, &index) ||
			    tensor_index3(index, t->m, t->m_base, 1, 0, 0, &index))
				return tensor_range_error(ctx, cmd->out, GPGPU_VM_WRITE);
		} else {
			if (tensor_index3(t->batch, t->m, t->m_base + row, 1, 0, 0, &index) ||
			    tensor_index3(index, t->n, t->n_base, 1, 0, 0, &index))
				return tensor_range_error(ctx, cmd->out, GPGPU_VM_WRITE);
		}

		/* 2. 长块也保留 FP32 元素对齐。 */
		error = tensor_address(ctx, cmd->out, index, sizeof(uint32_t),
				       GPGPU_VM_WRITE, &t->out_addr[row]);
		if (error)
			return error;
	}
	return 0;
}

/*
 * tensor_conv_a() - 从 NCHW 原地址收集本块的有界卷积 patch
 *
 * K 依次展开组内输入通道、Kh、Kw；只为本次最多 8*32 项生成地址。
 * 例：groups=4、group=2、Cin/group=3 时，K 对应的通道起点是 6。
 *
 * 1. 按输出空间位置和 stride/pad/dilation 计算输入坐标；padding 直接填零；
 * 2. 仅把实际在输入范围内的地址加入紧凑地址表；
 * 3. 读完本行真实输入，再恢复到 A 的逻辑 K 位置；
 *    失败则不计算，也不写输出。
 *
 * 不读取 padding 的伪地址，不离线生成 im2col；所有代价在当前指令内。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_conv_a(GPGPUExecContext *ctx, const TensorCommand *cmd, TensorMatrix *t)
{
	const uint64_t *d = cmd->dims;

	for (unsigned int row = 0; row < t->rows; row++) {
		uint64_t addr[GPGPU_TENSOR_TILE_K];
		unsigned int target[GPGPU_TENSOR_TILE_K];
		uint8_t raw[GPGPU_TENSOR_TILE_K * sizeof(uint32_t)];
		uint64_t space = t->m_base + row;
		uint64_t oh = space / d[5];
		uint64_t ow = space % d[5];
		unsigned int count = 0;
		uint32_t error;

		for (unsigned int k = 0; k < t->depth; k++) {
			uint64_t reduction = t->k_base + k;
			uint64_t channel = reduction / t->kernel_plane;
			uint64_t kernel = reduction % t->kernel_plane;
			uint64_t iy;
			uint64_t ix;
			uint64_t index;

			/*
			 * 1. 先用无符号加法形成带 pad 的坐标，
			 *    再判断能否减去 pad。
			 */
			t->a[row][k] = float32_zero;
			if (tensor_index3(oh, d[8], kernel / d[7], d[12], 0, 0, &iy) ||
			    tensor_index3(ow, d[9], kernel % d[7], d[13], 0, 0, &ix))
				return tensor_range_error(ctx, cmd->in[0], GPGPU_VM_READ);
			if (iy < d[10] || ix < d[11])
				continue;
			iy -= d[10];
			ix -= d[11];
			if (iy >= d[1] || ix >= d[2])
				continue;

			/*
			 * 2. batch/Cin 与组内通道参与 NCHW 索引。
			 *    不能跨组借用 A。
			 */
			if (tensor_index3(t->batch, d[0], t->group, t->cin_group,
					   channel, 1, &index) ||
			    tensor_index3(index, t->input_plane, iy, d[2], ix, 1, &index))
				return tensor_range_error(ctx, cmd->in[0], GPGPU_VM_READ);
			error = tensor_address(ctx, cmd->in[0], index, sizeof(uint32_t),
					       GPGPU_VM_READ, &addr[count]);
			if (error)
				return error;
			target[count++] = k;
		}

		/*
		 * 3. 地址表可为空，也可含非连续访问。
		 *    无论哪种情况，都不读取空洞。
		 */
		error = tensor_gather(ctx, addr, count, sizeof(uint32_t), raw);
		if (error)
			return error;
		for (unsigned int i = 0; i < count; i++)
			t->a[row][target[i]] = ldl_le_p(raw + i * sizeof(uint32_t));
	}
	return 0;
}

/*
 * tensor_matrix_a() - 读取本块的左矩阵输入
 *
 * Linear/BMM 按逻辑 [batch,M,K] 寻址，Conv 则交给有界 patch gather。
 * 例：A 的元素 stride=[12,4,1] 时，batch=1、M=2、K=3 对应索引 23。
 *
 * 1. 卷积分派到原 NCHW gather，不把它当作紧凑二维矩阵；
 * 2. 其他格式逐 M 行形成元素索引，保留显式零 K stride；
 * 3. 把用户访存返回的小端字节解码为 FP32 原始位，不做浮点运算。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_matrix_a(GPGPUExecContext *ctx, const TensorCommand *cmd, TensorMatrix *t)
{
	/* 1. 卷积的组和 padding 规则由专用 gather 完成。 */
	if (t->format == GPGPU_TENSOR_CONV)
		return tensor_conv_a(ctx, cmd, t);
	for (unsigned int row = 0; row < t->rows; row++) {
		uint8_t raw[GPGPU_TENSOR_TILE_K * sizeof(uint32_t)];
		uint64_t index;
		uint32_t error;

		/* 2. 坐标先换成元素索引，再由向量读取处理 GPU 字节地址。 */
		if (tensor_index3(t->batch, t->a_stride[0], t->m_base + row, t->a_stride[1],
				   t->k_base, t->a_stride[2], &index))
			return tensor_range_error(ctx, cmd->in[0], GPGPU_VM_READ);
		error = tensor_vector_read(ctx, cmd->in[0], index, t->a_stride[2], t->depth,
					   sizeof(uint32_t), raw);
		if (error)
			return error;

		/* 3. 共享内存的多字节值先读字节，再显式解码小端。 */
		for (unsigned int k = 0; k < t->depth; k++)
			t->a[row][k] = ldl_le_p(raw + k * sizeof(uint32_t));
	}
	return 0;
}

/*
 * tensor_matrix_b() - 读取权重或 BMM 的右矩阵，整理为局部 [K,N]
 *
 * Linear/Conv 的源权重是 [N,K]，BMM 的源是 [batch,K,N]。
 * W8 不按 FP32 读取，不在读取阶段反量化。
 *
 * 1. BMM 逐 K 读取带显式 N stride 的向量；
 * 2. Linear/Conv 逐输出通道读取连续 K，并在局部转置；
 * 3. W8 只保存有符号字节，等其他输入全部读完后才乘 scale。
 *
 * 本函数只移动和解码字节，不执行浮点运算。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_matrix_b(GPGPUExecContext *ctx, const TensorCommand *cmd, TensorMatrix *t)
{
	if (t->format == GPGPU_TENSOR_BMM) {
		for (unsigned int k = 0; k < t->depth; k++) {
			uint8_t raw[GPGPU_TENSOR_TILE_N * sizeof(uint32_t)];
			uint64_t index;
			uint32_t error;

			/*
			 * 1. 六个 stride 的零值均保留原义。
			 *    不读取广播地址之间的跨度。
			 */
			if (tensor_index3(t->batch, t->b_stride[0], t->k_base + k,
					   t->b_stride[1], t->n_base, t->b_stride[2], &index))
				return tensor_range_error(ctx, cmd->in[1], GPGPU_VM_READ);
			error = tensor_vector_read(ctx, cmd->in[1], index, t->b_stride[2], t->cols,
						   sizeof(uint32_t), raw);
			if (error)
				return error;
			for (unsigned int col = 0; col < t->cols; col++)
				t->b[k][col] = ldl_le_p(raw + col * sizeof(uint32_t));
		}
		return 0;
	}

	for (unsigned int col = 0; col < t->cols; col++) {
		uint8_t raw[GPGPU_TENSOR_TILE_K * sizeof(uint32_t)];
		unsigned int size = t->format == GPGPU_TENSOR_LINEAR_W8 ? 1 : sizeof(uint32_t);
		uint64_t index;
		uint32_t error;

		/* 2. Conv 的 n_base 已含组偏移，K 只覆盖本组输入通道。 */
		if (tensor_index3(t->n_base + col, t->k, t->k_base, 1, 0, 0, &index))
			return tensor_range_error(ctx, cmd->in[1], GPGPU_VM_READ);
		error = tensor_vector_read(ctx, cmd->in[1], index, 1, t->depth, size, raw);
		if (error)
			return error;
		for (unsigned int k = 0; k < t->depth; k++) {
			/* 3. INT8 转成 FP32 及逐项乘 scale 均留到计算阶段。 */
			if (t->format == GPGPU_TENSOR_LINEAR_W8)
				t->w8[k][col] = ldsb_p(raw + k);
			else
				t->b[k][col] = ldl_le_p(raw + k * sizeof(uint32_t));
		}
	}
	return 0;
}

/*
 * tensor_matrix_load() - 在任何浮点运算和输出写入之前读完本块输入
 *
 * 1. 读取 A、B/INT8 权重和可选 W8 scale；未提供 scale 时按 1 解释；
 * 2. 非初始 K 块读取旧 C，初始块的局部 seed 保持正零；
 * 3. 仅最后 K 块读取可选 bias，读失败不进入任何计算或输出写回。
 *
 * 输出地址只保存用户地址；本函数不缓存宿主后备存储指针。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_matrix_load(GPGPUExecContext *ctx, const TensorCommand *cmd, TensorMatrix *t)
{
	uint8_t raw[TENSOR_ROW_MAX * sizeof(uint32_t)];
	bool conv = t->format == GPGPU_TENSOR_CONV;
	unsigned int rows = conv ? t->cols : t->rows;
	unsigned int cols = conv ? t->rows : t->cols;
	uint32_t error;

	/* 1. 全部输入快照有固定上限，不把完整矩阵复制到 CPU。 */
	if (t->depth) {
		error = tensor_matrix_a(ctx, cmd, t);
		if (error)
			return error;
		error = tensor_matrix_b(ctx, cmd, t);
		if (error)
			return error;
		if (t->format == GPGPU_TENSOR_LINEAR_W8) {
			for (unsigned int col = 0; col < t->cols; col++)
				t->scale[col] = float32_one;
			if (cmd->in[2]) {
				error = tensor_vector_read(ctx, cmd->in[2], t->n_base, 1, t->cols,
							   sizeof(uint32_t), raw);
				if (error)
					return error;
				for (unsigned int col = 0; col < t->cols; col++)
					t->scale[col] = ldl_le_p(raw + col * sizeof(uint32_t));
			}
		}
	}

	/* 2. seed 在块创建时清零；不以新 C 的地址去读取初始垃圾数据。 */
	if (t->k_base) {
		for (unsigned int row = 0; row < rows; row++) {
			error = tensor_user_rw(ctx, t->out_addr[row], raw,
					       cols * sizeof(uint32_t), sizeof(uint32_t),
					       GPGPU_VM_READ);
			if (error)
				return error;
			for (unsigned int col = 0; col < cols; col++) {
				if (conv)
					t->seed[col][row] = ldl_le_p(raw + col * sizeof(uint32_t));
				else
					t->seed[row][col] = ldl_le_p(raw + col * sizeof(uint32_t));
			}
		}
	}

	/* 3. Conv 的 bias 在 IN2，Linear 的 bias 在 IN3，BMM 不加 bias。 */
	if (t->has_bias) {
		uint64_t base = cmd->in[conv ? 2 : 3];

		error = tensor_vector_read(ctx, base, t->n_base, 1, t->cols, sizeof(uint32_t), raw);
		if (error)
			return error;
		for (unsigned int col = 0; col < t->cols; col++)
			t->bias[col] = ldl_le_p(raw + col * sizeof(uint32_t));
	}
	return 0;
}

/* 每行只提取一次最大编码指数；255 表示存在 NaN/Inf，空 K 块返回 0。 */
static unsigned int tensor_w8_a_exp_max(const TensorMatrix *t, unsigned int row)
{
	unsigned int a_exp_max = 0;

	for (unsigned int k = 0; k < t->depth; k++)
		a_exp_max = MAX(a_exp_max, (t->a[row][k] >> 23) & 255);
	return a_exp_max;
}

/*
 * tensor_w8_compensated_valid() - 用本行最大指数判断一个 W8 输出能否补偿
 *
 * 标量 Kernel 使用同一组 ABI 指数界限，预筛不执行浮点运算：
 *
 *   FP32 位     31       30..23       22..0
 *   含义        符号     编码指数 E   小数
 *
 * 例：A=scale=1 时两指数均为 127，指数和 254 在共享安全界内。
 * 1. 检查本输出的 seed、scale 指数；大有限值及 NaN/Inf 直接拒绝；
 * 2. 用本行最大 A 指数检查非有限值及指数和，不依赖其他输出列。
 *    scale 指数非负，因此最大指数通过等价于该行每个 A 指数通过。
 *
 * 本函数不反量化、不试算补偿，避免拒绝路径泄漏浮点异常。
 * Return: 可以直接执行补偿时为 true，否则必须从原 seed 严格逐 K FMA。
 */
static bool tensor_w8_compensated_valid(const TensorMatrix *t, unsigned int row, unsigned int col,
				      unsigned int a_exp_max)
{
	unsigned int seed_exp = (t->seed[row][col] >> 23) & 255;
	unsigned int scale_exp = (t->scale[col] >> 23) & 255;

	/* 1. 同一输出列只使用自己的 seed 和 scale 选择数值路径。 */
	if (seed_exp > GPGPU_TENSOR_W8_SAFE_EXP_MAX ||
	    scale_exp > GPGPU_TENSOR_W8_SAFE_EXP_MAX)
		return false;

	/* 2. 最大指数为 255 时，该行至少有一个 NaN/Inf。 */
	return a_exp_max != 255 && a_exp_max + scale_exp <= GPGPU_TENSOR_W8_SAFE_PRODUCT_EXP_SUM;
}

/* 收集并清除局部异常，避免 sticky inexact 触发宿主 hardfloat。 */
static float32 tensor_soft_result(float32 value, float_status *soft, int *flags)
{
	*flags |= get_float_exception_flags(soft);
	set_float_exception_flags(0, soft);
	return value;
}

/*
 * tensor_w8_compensated() - 用 FP32 TwoProduct/TwoSum 累加一个固定 K 块
 *
 * 整数预筛通过后才调用，权重已独立反量化。各步与标量 Kernel 对齐：
 *
 *   A*W ─TwoProduct─▶ product + product_error
 *   sum+product ─TwoSum─▶ next + sum_error
 *   correction += sum_error+product_error；sum=next
 *
 * 例：大乘积约 -1058 后加小项，低位先存到 correction，
 * 而不是每项都丢给大 sum 舍入。
 * 1. 独立乘法取得 product，再用 FMA 恢复乘积低位；
 * 2. 用 TwoSum 恢复加法低位，与乘积低位一起加到 correction；
 * 3. 块末只发布一个 FP32 sum+correction。
 *
 * 本函数不改变反量化、块边界、偏置或访存；极值回退在调用前决定。
 * Return: 按本块 FP32 补偿公式舍入的输出，异常并入 flags。
 */
static float32 tensor_w8_compensated(const TensorMatrix *t, unsigned int row, unsigned int col,
				    float_status *soft, int *flags)
{
	float32 sum = t->seed[row][col];
	float32 correction = float32_zero;

	set_float_exception_flags(0, soft);
	for (unsigned int k = 0; k < t->depth; k++) {
		float32 a = t->a[row][k];
		float32 b = t->b[k][col];
		float32 product;
		float32 negative;
		float32 product_error;
		float32 next;
		float32 part;
		float32 left;
		float32 right;
		float32 error;

		/* 1. 权重已先舍入；乘法低位只补回本次 A*W 的误差。 */
		product = tensor_soft_result(float32_mul(a, b, soft), soft, flags);
		negative = tensor_soft_result(float32_mul(product, float32_chs(float32_one), soft),
					     soft, flags);
		product_error = tensor_soft_result(float32_muladd(a, b, negative, 0, soft),
						  soft, flags);

		/* 2. 不假设 sum 与 product 的绝对值大小关系。 */
		next = tensor_soft_result(float32_add(sum, product, soft), soft, flags);
		part = tensor_soft_result(float32_sub(next, sum, soft), soft, flags);
		left = tensor_soft_result(float32_sub(next, part, soft), soft, flags);
		left = tensor_soft_result(float32_sub(sum, left, soft), soft, flags);
		right = tensor_soft_result(float32_sub(product, part, soft), soft, flags);
		error = tensor_soft_result(float32_add(left, right, soft), soft, flags);
		error = tensor_soft_result(float32_add(error, product_error, soft), soft, flags);
		correction = tensor_soft_result(float32_add(correction, error, soft), soft, flags);
		sum = next;
	}

	/* 3. 低位不跨指令保存，后续 K 块只读取该 FP32 结果。 */
	return tensor_soft_result(float32_add(sum, correction, soft), soft, flags);
}

/*
 * tensor_matrix_reference() - 完整 SoftFloat 参考路径
 *
 * SoftFloat 在 sticky inexact 已置位时可能走 hardfloat。
 * 每次运算都清局部异常，再单独汇总。
 * 因此参考路径不依赖宿主舍入、FTZ/DAZ 或异常环境。
 *
 * 1. W8 的每项先精确转 FP32，再用独立 FP32 乘法乘该输出通道的 scale；
 * 2. 每个输出从自己的 seed 开始；安全 W8 块做补偿，其他情况严格逐 K FMA；
 * 3. 末块再做一次独立 FP32 bias 加法，把所有异常并入当前 Lane。
 *
 * 不访问 GPU，不改变 PC/fcsr；native 尝试失败时从原 seed 完整重算。
 */
static void tensor_matrix_reference(TensorMatrix *t,
				    uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N],
				    float_status *status)
{
	float_status soft = *status;
	int flags = 0;

	/* 1. 保留反量化乘法的独立舍入，不能把 scale 移到最终输出上。 */
	if (t->format == GPGPU_TENSOR_LINEAR_W8) {
		for (unsigned int k = 0; k < t->depth; k++) {
			for (unsigned int col = 0; col < t->cols; col++) {
				float32 weight;

				set_float_exception_flags(0, &soft);
				weight = int32_to_float32(t->w8[k][col], &soft);
				flags |= get_float_exception_flags(&soft);
				set_float_exception_flags(0, &soft);
				t->b[k][col] = float32_mul(weight, t->scale[col], &soft);
				flags |= get_float_exception_flags(&soft);
			}
		}
	}

	for (unsigned int row = 0; row < t->rows; row++) {
		unsigned int a_exp_max = t->format == GPGPU_TENSOR_LINEAR_W8 ?
					 tensor_w8_a_exp_max(t, row) : 0;

		for (unsigned int col = 0; col < t->cols; col++) {
			float32 acc = t->seed[row][col];

			/* 2. 不符合指数界时，直接用原 seed 逐 K FMA。 */
			if (t->format == GPGPU_TENSOR_LINEAR_W8 &&
			    tensor_w8_compensated_valid(t, row, col, a_exp_max)) {
				acc = tensor_w8_compensated(t, row, col, &soft, &flags);
			} else {
				for (unsigned int k = 0; k < t->depth; k++) {
					set_float_exception_flags(0, &soft);
					acc = float32_muladd(t->a[row][k], t->b[k][col],
							    acc, 0, &soft);
					flags |= get_float_exception_flags(&soft);
				}
			}

			/*
			 * 3. 偏置只在末块加入，
			 *    不与最后的 FMA 合并。
			 */
			if (t->has_bias) {
				set_float_exception_flags(0, &soft);
				acc = float32_add(acc, t->bias[col], &soft);
				flags |= get_float_exception_flags(&soft);
			}
			result[row][col] = acc;
		}
	}
	float_raise(flags, status);
}

#if TENSOR_NATIVE_X86
/*
 * native 只接收正规有限值或带符号零。
 * NaN、无穷和子正规数交给参考路径。
 */
static bool tensor_native_inputs_valid(const TensorMatrix *t)
{
	for (unsigned int row = 0; row < t->rows; row++) {
		unsigned int a_exp_max = t->format == GPGPU_TENSOR_LINEAR_W8 ?
					 tensor_w8_a_exp_max(t, row) : 0;

		for (unsigned int k = 0; k < t->depth; k++) {
			if (!float32_is_zero_or_normal(t->a[row][k]))
				return false;
		}
		for (unsigned int col = 0; col < t->cols; col++) {
			if (!float32_is_zero_or_normal(t->seed[row][col]))
				return false;
			if (t->format == GPGPU_TENSOR_LINEAR_W8 &&
			    !tensor_w8_compensated_valid(t, row, col, a_exp_max))
				return false;
		}
	}
	for (unsigned int col = 0; col < t->cols; col++) {
		if (t->has_bias && !float32_is_zero_or_normal(t->bias[col]))
			return false;
		if (t->format == GPGPU_TENSOR_LINEAR_W8) {
			if (t->depth && !float32_is_zero_or_normal(t->scale[col]))
				return false;
		} else {
			for (unsigned int k = 0; k < t->depth; k++) {
				if (!float32_is_zero_or_normal(t->b[k][col]))
					return false;
			}
		}
	}
	return true;
}

/*
 * tensor_native_vector_valid() - 逐分量检查 FP32 的原始位分类
 *
 *   FP32 位     31       30..23       22..0
 *   含义        符号     指数         小数
 *
 * 例：0x8000_0000 是负零，去掉符号后仍为零，必须保留为合法输入/结果。
 * 1. 提取指数与去符号值，只用整数 SIMD，不引入新的浮点异常；
 * 2. 接受指数非零且非全一的正规数，或去符号后为零的值。
 *
 * 每个中间向量都要检查，不能只检查可能已经掩盖异常的最终结果。
 * Return: 全部分量是正规有限值或零时为 true。
 */
static bool __attribute__((target("avx2,fma"))) tensor_native_vector_valid(__m256 value)
{
	__m256i bits = _mm256_castps_si256(value);
	__m256i zero = _mm256_setzero_si256();
	__m256i exponent;
	__m256i magnitude;
	__m256i normal;
	__m256i valid;

	/* 1. 原始位检查不会让 NaN 或子正规数参与 host 浮点比较。 */
	exponent = _mm256_and_si256(bits, _mm256_set1_epi32(0x7f800000));
	magnitude = _mm256_and_si256(bits, _mm256_set1_epi32(0x7fffffff));

	/* 2. 保留带符号零，其余指数为零或全一的值均拒绝。 */
	normal = _mm256_andnot_si256(
		_mm256_cmpeq_epi32(exponent, _mm256_set1_epi32(0x7f800000)),
		_mm256_cmpgt_epi32(exponent, zero));
	valid = _mm256_or_si256(normal, _mm256_cmpeq_epi32(magnitude, zero));
	return _mm256_movemask_epi8(valid) == -1;
}

/*
 * tensor_native_kernel_ok() - 在有界局部数组内跨 N 输出执行 SIMD
 *
 * 只有能力检查完成且宿主环境已设为 RNE、关闭 FTZ/DAZ、屏蔽异常时调用。
 * noinline 把浮点计算固定在环境设置和恢复之间，不能依赖编译器默认的
 * FENV_ACCESS；本函数内没有宿主标量浮点表达式或外部数学库。
 *
 * 1. 把真实列复制到补零的局部行，尾部补位不读取额外 GPU 元素；
 * 2. W8 逐项 FP32 乘 scale，每次舍入后检查中间值；
 * 3. SIMD 各列按相同 K 顺序计算；W8 与软件路径同公式补偿，
 *    其他格式仍逐 K FMA，块末独立加 bias；
 * 4. 只复制真实输出列，异常标志仍由调用者在恢复宿主环境前检查。
 *
 * 子正规或非有限中间值立即拒绝；调用者从原 seed 运行完整参考路径。
 * Return: 所有局部结果满足数值分类条件时为 true。
 */
static bool __attribute__((target("avx2,fma"), noinline))
tensor_native_kernel_ok(const TensorMatrix *t,
			uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N])
{
	uint32_t b[GPGPU_TENSOR_TILE_K][TENSOR_NATIVE_COLUMNS] = { 0 };
	uint32_t c[GPGPU_TENSOR_TILE_M][TENSOR_NATIVE_COLUMNS] = { 0 };
	uint32_t scale[TENSOR_NATIVE_COLUMNS] = { 0 };
	uint32_t bias[TENSOR_NATIVE_COLUMNS] = { 0 };

	/* 1. 局部补位均为零；不存在从 GPU 多读满向量的行为。 */
	for (unsigned int col = 0; col < t->cols; col++) {
		scale[col] = t->scale[col];
		bias[col] = t->bias[col];
		for (unsigned int k = 0; k < t->depth; k++) {
			if (t->format == GPGPU_TENSOR_LINEAR_W8)
				b[k][col] = (uint32_t)(int32_t)t->w8[k][col];
			else
				b[k][col] = t->b[k][col];
		}
	}
	for (unsigned int row = 0; row < t->rows; row++)
		memcpy(c[row], t->seed[row], t->cols * sizeof(uint32_t));

	/* 2. INT8->FP32 精确；mul_ps 的结果先舍入，不能并入后续 FMA。 */
	if (t->format == GPGPU_TENSOR_LINEAR_W8) {
		for (unsigned int k = 0; k < t->depth; k++) {
			for (unsigned int col = 0; col < t->cols; col += TENSOR_NATIVE_WIDTH) {
				__m256i integer = _mm256_loadu_si256((const __m256i *)&b[k][col]);
				__m256 factor = _mm256_castsi256_ps(
					_mm256_loadu_si256((const __m256i *)&scale[col]));
				__m256 weight = _mm256_mul_ps(_mm256_cvtepi32_ps(integer), factor);

				if (!tensor_native_vector_valid(weight))
					return false;
				_mm256_storeu_si256((__m256i *)&b[k][col],
						     _mm256_castps_si256(weight));
			}
		}
	}

	/* 3. 向量的各分量是独立 N 输出，不做水平求和或 K 的重排。 */
	for (unsigned int row = 0; row < t->rows; row++) {
		for (unsigned int col = 0; col < t->cols; col += TENSOR_NATIVE_WIDTH) {
			__m256 acc = _mm256_castsi256_ps(
				_mm256_loadu_si256((const __m256i *)&c[row][col]));

			if (t->format == GPGPU_TENSOR_LINEAR_W8) {
				__m256 correction = _mm256_setzero_ps();
				__m256 sign = _mm256_castsi256_ps(_mm256_set1_epi32(INT32_MIN));

				for (unsigned int k = 0; k < t->depth; k++) {
					__m256 a = _mm256_castsi256_ps(
						_mm256_set1_epi32(t->a[row][k]));
					__m256 weight = _mm256_castsi256_ps(
						_mm256_loadu_si256((const __m256i *)&b[k][col]));
					__m256 product = _mm256_mul_ps(a, weight);
					__m256 product_error = _mm256_fmadd_ps(
						a, weight, _mm256_xor_ps(product, sign));
					__m256 next = _mm256_add_ps(acc, product);
					__m256 part = _mm256_sub_ps(next, acc);
					__m256 back = _mm256_sub_ps(next, part);
					__m256 left = _mm256_sub_ps(acc, back);
					__m256 right = _mm256_sub_ps(product, part);
					__m256 sum_error = _mm256_add_ps(left, right);
					__m256 error = _mm256_add_ps(sum_error, product_error);

					correction = _mm256_add_ps(correction, error);
					if (!tensor_native_vector_valid(product) ||
					    !tensor_native_vector_valid(product_error) ||
					    !tensor_native_vector_valid(next) ||
					    !tensor_native_vector_valid(part) ||
					    !tensor_native_vector_valid(back) ||
					    !tensor_native_vector_valid(left) ||
					    !tensor_native_vector_valid(right) ||
					    !tensor_native_vector_valid(sum_error) ||
					    !tensor_native_vector_valid(error) ||
					    !tensor_native_vector_valid(correction))
						return false;
					acc = next;
				}
				acc = _mm256_add_ps(acc, correction);
				if (!tensor_native_vector_valid(acc))
					return false;
			} else {
				for (unsigned int k = 0; k < t->depth; k++) {
					__m256 a = _mm256_castsi256_ps(
						_mm256_set1_epi32(t->a[row][k]));
					__m256 weight = _mm256_castsi256_ps(
						_mm256_loadu_si256((const __m256i *)&b[k][col]));

					acc = _mm256_fmadd_ps(a, weight, acc);
					if (!tensor_native_vector_valid(acc))
						return false;
				}
			}
			if (t->has_bias) {
				__m256 offset = _mm256_castsi256_ps(
					_mm256_loadu_si256((const __m256i *)&bias[col]));

				acc = _mm256_add_ps(acc, offset);
				if (!tensor_native_vector_valid(acc))
					return false;
			}
			_mm256_storeu_si256((__m256i *)&c[row][col], _mm256_castps_si256(acc));
		}
	}

	/*
	 * 4. 分类通过仍须检查宿主异常。
	 *    underflow 到零等情况仍需拒绝。
	 */
	for (unsigned int row = 0; row < t->rows; row++)
		memcpy(result[row], c[row], t->cols * sizeof(uint32_t));
	return true;
}

/*
 * tensor_native_ok() - 保存宿主环境、尝试 SIMD，并只提交可证明一致的异常
 *
 * 每次调用只计算当前 Lane 的一个块。
 * 不得把其他 Lane 的异常混入其 fp_status。
 * RNE 下，输入及每步结果均为正规有限值或零，
 * 且仅有 inexact 标志时，数值及异常并集
 * 与逐次 FP32 SoftFloat 一致。其他条件均回退。
 *
 * 1. 检查 CPU 能力、Lane 舍入/微小数配置和输入分类；
 * 2. 保存 fenv 与原始 MXCSR，屏蔽异常、清标志、设 RNE，并关闭 FTZ/DAZ；
 * 3. 调用不可内联的局部 SIMD 核心；非 inexact 异常均拒绝；
 * 4. 成功或失败都恢复 fenv 和精确的原 MXCSR，成功才并入本 Lane 的 inexact。
 *
 * 不在修改宿主环境期间访问 GPU，不把失败尝试的结果或异常提交给 Lane。
 * Return: SIMD 数值、异常和环境恢复均满足条件时为 true；
 * 否则使用参考路径。
 */
static bool tensor_native_ok(const TensorMatrix *t,
			     uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N],
			     float_status *status)
{
	fenv_t saved;
	fenv_t held;
	unsigned int original;
	unsigned int control;
	unsigned int flags = 0;
	int exceptions = 0;
	bool accepted = false;
	bool restored;

	/* 1. CPU 能力检查在进入任何 target("avx2,fma") 函数之前完成。 */
	if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") ||
	    get_float_rounding_mode(status) != float_round_nearest_even ||
	    get_float_detect_tininess(status) != float_tininess_after_rounding ||
	    get_flush_inputs_to_zero(status) || get_flush_to_zero(status) ||
	    !tensor_native_inputs_valid(t))
		return false;

	/* 2. fegetenv 失败时尚未改环境；其余失败统一走 restore。 */
	original = _mm_getcsr();
	if (fegetenv(&saved))
		return false;
	if (feholdexcept(&held) || fesetround(FE_TONEAREST))
		goto restore;
	control = original & ~(TENSOR_MXCSR_FLAGS | TENSOR_MXCSR_ROUND |
			       TENSOR_MXCSR_DAZ | TENSOR_MXCSR_FTZ);
	_mm_setcsr(control | TENSOR_MXCSR_MASKS);

	/* 3. 也检查 MXCSR 的 denormal 位，不能只依赖 FE_ALL_EXCEPT 的覆盖范围。 */
	if (!tensor_native_kernel_ok(t, result))
		goto restore;
	exceptions = fetestexcept(FE_ALL_EXCEPT);
	flags = _mm_getcsr() & TENSOR_MXCSR_FLAGS;
	accepted = !(exceptions & ~FE_INEXACT) && !(flags & ~TENSOR_MXCSR_INEXACT);

restore:
	/*
	 * 4. fesetenv 不重抛本次异常。
	 *    MXCSR 精确恢复原位，包含 DAZ。
	 */
	restored = !fesetenv(&saved);
	_mm_setcsr(original);
	if (!accepted || !restored)
		return false;
	if ((exceptions & FE_INEXACT) || (flags & TENSOR_MXCSR_INEXACT))
		float_raise(float_flag_inexact, status);
	return true;
}
#else
/* 不支持局部 AVX2/FMA 的宿主始终使用完整 SoftFloat，不暴露 x86 类型。 */
static bool tensor_native_ok(const TensorMatrix *t,
			     uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N],
			     float_status *status)
{
	(void)t;
	(void)result;
	(void)status;
	return false;
}
#endif

/*
 * tensor_matrix_store() - 按真实连续行发布矩阵结果
 *
 * 1. 把一行真实输出编码成小端字节；卷积从局部 [M,N] 转回 NCHW 空间行；
 * 2. 写完当前行才访问下一行，首个失败立即停止。
 *
 * 不回滚之前的行；当前行跨页时也保留 gpgpu_user_rw() 已完成的部分。
 * Return: 0 表示全部发布，否则为执行错误码。
 */
static uint32_t tensor_matrix_store(GPGPUExecContext *ctx, const TensorMatrix *t,
				    const uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N])
{
	bool conv = t->format == GPGPU_TENSOR_CONV;
	unsigned int rows = conv ? t->cols : t->rows;
	unsigned int cols = conv ? t->rows : t->cols;

	for (unsigned int row = 0; row < rows; row++) {
		uint8_t raw[TENSOR_ROW_MAX * sizeof(uint32_t)];
		uint32_t error;

		/* 1. 不向 GPU 写入局部 SIMD 的补位或未使用列。 */
		for (unsigned int col = 0; col < cols; col++) {
			uint32_t value = conv ? result[col][row] : result[row][col];

			stl_le_p(raw + col * sizeof(uint32_t), value);
		}

		/* 2. 后行错误保留先前的写入，调用者随后停止其余 Lane。 */
		error = tensor_user_rw(ctx, t->out_addr[row], raw, cols * sizeof(uint32_t),
				       sizeof(uint32_t), GPGPU_VM_WRITE);
		if (error)
			return error;
	}
	return 0;
}

/*
 * tensor_matrix_execute() - 组织一条矩阵指令的检查、快照、计算与发布
 *
 * 1. 检查格式对应的完整维度、工作块和 K 起点，再建立输出行地址；
 * 2. 读完全部 A/B/scale/旧 C/bias，任一失败均不做浮点运算或输出写入；
 * 3. 尝试安全 SIMD，否则从原 seed 运行 SoftFloat，并累计本地计算量；
 * 4. 逐行写回，失败保留本次已经完成的计算量和先前行。
 *
 * 不修改 PC、active 或 fcsr，失败原因交由公共入口设置到 ctx->error。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_matrix_execute(GPGPUExecContext *ctx, GPGPULane *lane,
				      const TensorCommand *cmd, uint32_t format,
				      uint64_t command, uint64_t block, uint64_t k_base,
				      bool native_enabled, GPGPUTensorStats *stats)
{
	TensorMatrix tile = { 0 };
	uint32_t result[GPGPU_TENSOR_TILE_M][GPGPU_TENSOR_TILE_N];
	uint32_t error;
	bool native;

	/* 1. 格式已经由公共入口约束，不解释任何评估器 opcode。 */
	tile.format = format;
	switch (format) {
	case GPGPU_TENSOR_LINEAR_F32:
	case GPGPU_TENSOR_LINEAR_W8:
		error = tensor_linear_shape(cmd, &tile, block, k_base);
		break;
	case GPGPU_TENSOR_BMM:
		error = tensor_bmm_shape(cmd, &tile, block, k_base);
		break;
	case GPGPU_TENSOR_CONV:
		error = tensor_conv_shape(cmd, &tile, block, k_base);
		break;
	default:
		return tensor_range_error(ctx, command, GPGPU_VM_READ);
	}
	if (error)
		return tensor_range_error(ctx, command, GPGPU_VM_READ);
	error = tensor_matrix_output(ctx, cmd, &tile);
	if (error)
		return error;

	/* 2. 输出写入和数值计算都在输入快照完整之后。 */
	error = tensor_matrix_load(ctx, cmd, &tile);
	if (error)
		return error;

	/* 3. 拒绝 native 时不保留尝试的结果或异常。 */
	native = native_enabled && tensor_native_ok(&tile, result, &lane->fp_status);
	if (!native)
		tensor_matrix_reference(&tile, result, &lane->fp_status);
	if (stats) {
		stats->macs += (uint64_t)tile.rows * tile.cols * tile.depth;
		if (native)
			stats->native_tiles++;
		else
			stats->fallback_tiles++;
	}

	/* 4. 计算统计不冒充实际的内存请求数。 */
	return tensor_matrix_store(ctx, &tile, result);
}

/*
 * tensor_copy_shape() - 检查 COPY 的模式和最多七维元素 stride 映射
 *
 * D2 起先放 rank 个 shape，再放 rank 个 stride；rank=0 表示连续输入。
 * 例：shape=[2,3]、stride=[1,2] 时，逻辑索引 4 的坐标是 [1,1]，源索引为 3。
 *
 * 1. 选择输入/输出元素宽度，I64->F32 仍读取完整有符号 64 位；
 * 2. 检查秩、shape 乘积与 ELEMENTS 相等，并检查最大仿射索引。
 *
 * 不读取不使用的维度槽，零 stride 保留为广播。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_copy_shape(const TensorCommand *cmd, TensorLayout *layout)
{
	const uint64_t *d = cmd->dims;
	uint64_t elements = 1;
	uint64_t maximum = 0;
	unsigned int rank;

	/* 1. 原始位搬运不经过任何浮点运算。 */
	switch (d[0]) {
	case GPGPU_TENSOR_COPY_F32:
		layout->input_size = sizeof(uint32_t);
		layout->output_size = sizeof(uint32_t);
		break;
	case GPGPU_TENSOR_COPY_I64_F32:
		layout->input_size = sizeof(uint64_t);
		layout->output_size = sizeof(uint32_t);
		layout->convert = true;
		break;
	case GPGPU_TENSOR_COPY_I64:
		layout->input_size = sizeof(uint64_t);
		layout->output_size = sizeof(uint64_t);
		break;
	default:
		return GPGPU_VM_FAULT_RANGE;
	}

	/* 2. 秩七正好使用 D2..D15，不访问前缀外的私有字段。 */
	if (d[1] > GPGPU_TENSOR_MAX_RANK)
		return GPGPU_VM_FAULT_RANGE;
	rank = d[1];
	if (!rank)
		return 0;
	for (unsigned int i = 0; i < rank; i++) {
		uint64_t shape = d[2 + i];
		uint64_t stride = d[2 + rank + i];
		uint64_t term;

		if (!shape || umul64_overflow(elements, shape, &elements) ||
		    umul64_overflow(shape - 1, stride, &term) ||
		    uadd64_overflow(maximum, term, &maximum))
			return GPGPU_VM_FAULT_RANGE;
	}
	return elements == cmd->elements ? 0 : GPGPU_VM_FAULT_RANGE;
}

/*
 * tensor_copy_index() - 从行优先逻辑索引恢复实际 stride 索引
 *
 *   logical ─从末轴依次对 shape 取余/整除─▶ 各轴坐标
 *   各轴坐标 ─乘元素 stride、求和─▶ index
 *
 * 例：logical=4、shape=[2,3]、stride=[1,2] 得到坐标 [1,1]，index=3。
 * 1. 连续模式直接保留索引；其余模式从最内轴向外解码坐标；
 * 2. 逐轴检查乘法与累加，地址换算留给调用者。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_copy_index(const TensorCommand *cmd, uint64_t logical, uint64_t *index)
{
	unsigned int rank = cmd->dims[1];

	/* 1. rank 已在 shape 检查中限制为 0 或 1..7。 */
	if (!rank) {
		*index = logical;
		return 0;
	}
	*index = 0;
	for (unsigned int remaining = rank; remaining; remaining--) {
		unsigned int axis = remaining - 1;
		uint64_t shape = cmd->dims[2 + axis];
		uint64_t coordinate = logical % shape;
		uint64_t term;

		logical /= shape;

		/*
		 * 2. 零 stride 轴贡献零偏移。
		 *    不能按紧凑 shape 推导其跨度。
		 */
		if (umul64_overflow(coordinate, cmd->dims[2 + rank + axis], &term) ||
		    uadd64_overflow(*index, term, index))
			return GPGPU_VM_FAULT_RANGE;
	}
	return 0;
}

/*
 * tensor_concat_shape() - 检查 [outer,axis,inner] 的拼接形状
 *
 * 1. 检查 inner、输出轴宽、最多六个输入和完整输出平面；
 * 2. 核对输入轴宽之和，允许某个输入轴宽为零，不访问它的地址。
 *
 * 例：inner=2、输入轴宽=[1,2] 时，一个 outer 平面包含 6 个 FP32 元素。
 * 不把拼接轴的元素数当作原始字节数，输入和输出均固定为 FP32 原始位。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_concat_shape(const TensorCommand *cmd, TensorLayout *layout)
{
	const uint64_t *d = cmd->dims;
	uint64_t width = 0;

	/* 1. 槽数和乘积先检查，之后才使用输入轴宽数组和除数。 */
	if (!d[0] || !d[1] || !d[2] || d[2] > TENSOR_INPUTS ||
	    umul64_overflow(d[0], d[1], &layout->concat_span) ||
	    cmd->elements % layout->concat_span)
		return GPGPU_VM_FAULT_RANGE;

	/* 2. 零轴宽也参与和的检查，但不会被逐项寻址选择。 */
	for (unsigned int i = 0; i < d[2]; i++) {
		if (uadd64_overflow(width, d[3 + i], &width))
			return GPGPU_VM_FAULT_RANGE;
	}
	layout->input_size = sizeof(uint32_t);
	layout->output_size = sizeof(uint32_t);
	return width == d[1] ? 0 : GPGPU_VM_FAULT_RANGE;
}

/*
 * tensor_concat_index() - 从连续输出坐标选择输入及其连续张量索引
 *
 * 输出逻辑形状为 [outer,axis,inner]。先从 axis 中扣除输入轴宽，再保留
 * 相同 outer 和 inner 坐标；STACK 可用每个输入轴宽为 1 表示。
 *
 * 1. 在最多六个输入中找到覆盖本输出轴坐标的输入；
 * 2. 以 (outer*input_width+input_axis)*inner+tail 得到源元素索引。
 *
 * 例：inner=2、轴宽=[1,2] 时，输出索引 4 对应输入 1 的源索引 2。
 * Return: 找到输入且索引不溢出时为 0，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_concat_index(const TensorCommand *cmd, const TensorLayout *layout,
				    uint64_t logical, unsigned int *input, uint64_t *index)
{
	const uint64_t *d = cmd->dims;
	uint64_t outer = logical / layout->concat_span;
	uint64_t axis = (logical / d[0]) % d[1];
	uint64_t tail = logical % d[0];

	/*
	 * 1. 不选择零轴宽输入。
	 *    即使地址为零，也不产生访问。
	 */
	for (unsigned int i = 0; i < d[2]; i++) {
		uint64_t width = d[3 + i];

		if (axis >= width) {
			axis -= width;
			continue;
		}

		/* 2. 不用输出的 axis 宽度作为输入行距。 */
		*input = i;
		if (tensor_index3(outer, width, axis, 1, 0, 0, index) ||
		    tensor_index3(*index, d[0], tail, 1, 0, 0, index))
			return GPGPU_VM_FAULT_RANGE;
		return 0;
	}
	return GPGPU_VM_FAULT_RANGE;
}

/*
 * tensor_nearest_shape() - 检查保留 N*C 平面数的 NCHW 最近邻形状
 *
 * 1. 检查 Hin/Win/Hout/Wout 为正，并核对 ELEMENTS 是完整输出平面的倍数；
 * 2. 检查相同平面数对应的完整输入元素数，固定输入/输出宽度为 FP32。
 *
 * 例：Hin*Win=6、Hout*Wout=12、ELEMENTS=24 时，两端都包含两个 N*C 平面。
 * 这里不使用 float 比例，逐项坐标换算在寻址时完成。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_nearest_shape(const TensorCommand *cmd, TensorLayout *layout)
{
	const uint64_t *d = cmd->dims;
	uint64_t unused;
	uint64_t planes;

	/* 1. 确认平面大小非零且不回绕后，才用它作为除数。 */
	if (!d[0] || !d[1] || !d[2] || !d[3] ||
	    umul64_overflow(d[0], d[1], &layout->input_plane) ||
	    umul64_overflow(d[2], d[3], &layout->output_plane) ||
	    cmd->elements % layout->output_plane)
		return GPGPU_VM_FAULT_RANGE;

	/* 2. N 和 C 不另行猜测，二者乘积由完整输出元素数推导。 */
	planes = cmd->elements / layout->output_plane;
	if (umul64_overflow(planes, layout->input_plane, &unused))
		return GPGPU_VM_FAULT_RANGE;
	layout->input_size = sizeof(uint32_t);
	layout->output_size = sizeof(uint32_t);
	return 0;
}

/*
 * tensor_nearest_index() - 按整数比例映射 NCHW 最近邻坐标
 *
 * 输出与输入保留相同的 N*C 平面号，只有 H/W 坐标缩放。
 * 例：Hin=2、Hout=3 时，输出行 0/1/2 分别读取输入行 0/0/1。
 *
 * 1. 从连续输出索引拆出平面号、输出 y/x；
 * 2. 用 floor(y*Hin/Hout)、floor(x*Win/Wout) 得到输入坐标并合成源索引。
 *
 * 坐标乘法溢出作为错误，不用宿主浮点比例近似替代。
 * Return: 0 表示成功，否则为 GPGPU_VM_FAULT_RANGE。
 */
static uint32_t tensor_nearest_index(const TensorCommand *cmd, const TensorLayout *layout,
				     uint64_t logical, uint64_t *index)
{
	const uint64_t *d = cmd->dims;
	uint64_t plane = logical / layout->output_plane;
	uint64_t position = logical % layout->output_plane;
	uint64_t y;
	uint64_t x;

	/* 1. 宽度是 Wout，不能按 Hin/Win 拆输出索引。 */
	y = position / d[3];
	x = position % d[3];

	/* 2. 不使用 float，也不读取相邻但未被 nearest 选择的像素。 */
	if (umul64_overflow(y, d[0], &y) || umul64_overflow(x, d[1], &x))
		return GPGPU_VM_FAULT_RANGE;
	y /= d[2];
	x /= d[3];
	return tensor_index3(plane, layout->input_plane, y, d[1], x, 1, index);
}

/*
 * tensor_layout_prepare() - 检查布局块并生成每个实际输入地址
 *
 * 1. 检查完整 ELEMENTS、线性块号和格式专用维度；
 * 2. 为最多 32 个元素逐项选择源输入和 stride 坐标，检查所有地址；
 * 3. 生成连续输出起点，保留输出元素的 4B 或 8B 对齐。
 *
 * 只做整数寻址，不探测未访问范围的权限，不修改任何 GPU 数据。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_layout_prepare(GPGPUExecContext *ctx, const TensorCommand *cmd,
				      uint32_t format, uint64_t command, uint64_t block,
				      TensorLayout *layout)
{
	uint32_t error;

	/* 1. 最后一个不足 32 元素的块有效，块尾以外的块号必须报错。 */
	if (!cmd->elements ||
	    block >= tensor_ceil_div(cmd->elements, GPGPU_TENSOR_BLOCK_ELEMENTS) ||
	    umul64_overflow(block, GPGPU_TENSOR_BLOCK_ELEMENTS, &layout->first))
		return tensor_range_error(ctx, command, GPGPU_VM_READ);
	layout->count = MIN(cmd->elements - layout->first, GPGPU_TENSOR_BLOCK_ELEMENTS);
	switch (format) {
	case GPGPU_TENSOR_COPY:
		error = tensor_copy_shape(cmd, layout);
		break;
	case GPGPU_TENSOR_CONCAT:
		error = tensor_concat_shape(cmd, layout);
		break;
	case GPGPU_TENSOR_NEAREST:
		error = tensor_nearest_shape(cmd, layout);
		break;
	default:
		error = GPGPU_VM_FAULT_RANGE;
		break;
	}
	if (error)
		return tensor_range_error(ctx, command, GPGPU_VM_READ);

	/* 2. 即使源地址不按顺序出现，也只读取实际映射到的元素。 */
	for (unsigned int i = 0; i < layout->count; i++) {
		uint64_t logical = layout->first + i;
		uint64_t index;
		unsigned int input = 0;

		switch (format) {
		case GPGPU_TENSOR_COPY:
			error = tensor_copy_index(cmd, logical, &index);
			break;
		case GPGPU_TENSOR_CONCAT:
			error = tensor_concat_index(cmd, layout, logical, &input, &index);
			break;
		case GPGPU_TENSOR_NEAREST:
			error = tensor_nearest_index(cmd, layout, logical, &index);
			break;
		default:
			error = GPGPU_VM_FAULT_RANGE;
			break;
		}
		if (error)
			return tensor_range_error(ctx, command, GPGPU_VM_READ);
		error = tensor_address(ctx, cmd->in[input], index, layout->input_size,
				       GPGPU_VM_READ, &layout->addr[i]);
		if (error)
			return error;
	}

	/* 3. I64 原始位输出不能因为大块 API 不查对齐而接受 4B 对齐地址。 */
	return tensor_address(ctx, cmd->out, layout->first, layout->output_size,
			      GPGPU_VM_WRITE, &layout->out);
}

/*
 * tensor_layout_execute() - 先读完有界布局块，再转换或逐位复制并写连续输出
 *
 * 1. 检查映射并读完所有真实输入；
 *    源/目标重叠时，也不覆盖本块尚未读取的输入；
 * 2. 原始位模式直接复制字节；I64->F32 每项用清异常的 SoftFloat 状态转换；
 * 3. 记录局部处理元素数，再提交连续输出；
 *    失败不回滚 API 已完成的部分。
 *
 * 原始 FP32 复制包含 NaN payload 和负零，不做浮点赋值或 NaN 规范化。
 * Return: 0 表示成功，否则为执行错误码。
 */
static uint32_t tensor_layout_execute(GPGPUExecContext *ctx, GPGPULane *lane,
				      const TensorCommand *cmd, uint32_t format,
				      uint64_t command, uint64_t block, GPGPUTensorStats *stats)
{
	TensorLayout layout = { 0 };
	uint8_t input[GPGPU_TENSOR_BLOCK_ELEMENTS * sizeof(uint64_t)];
	uint8_t output[GPGPU_TENSOR_BLOCK_ELEMENTS * sizeof(uint64_t)];
	uint32_t error;

	/* 1. 全部输入都在第一次输出写入之前读取，失败不做转换。 */
	error = tensor_layout_prepare(ctx, cmd, format, command, block, &layout);
	if (error)
		return error;
	error = tensor_gather(ctx, layout.addr, layout.count, layout.input_size, input);
	if (error)
		return error;

	/* 2. 每次清局部 sticky inexact，避免 SoftFloat 改走宿主 hardfloat。 */
	if (layout.convert) {
		float_status soft = lane->fp_status;
		int flags = 0;

		for (unsigned int i = 0; i < layout.count; i++) {
			int64_t value = (int64_t)ldq_le_p(input + i * sizeof(uint64_t));
			float32 converted;

			set_float_exception_flags(0, &soft);
			converted = int64_to_float32(value, &soft);
			flags |= get_float_exception_flags(&soft);
			stl_le_p(output + i * sizeof(uint32_t), converted);
		}
		float_raise(flags, &lane->fp_status);
	} else {
		memcpy(output, input, layout.count * layout.output_size);
	}

	/* 3. 逻辑处理量与 profile 中的请求次数分别累计。 */
	if (stats)
		stats->layout_elements += layout.count;
	return tensor_user_rw(ctx, layout.out, output, layout.count * layout.output_size,
			      layout.output_size, GPGPU_VM_WRITE);
}

bool gpgpu_tensor_execute(GPGPUExecContext *ctx, GPGPULane *lane, uint32_t kind,
			  uint32_t format, uint64_t command, uint64_t block, uint64_t k_base,
			  bool native_enabled, GPGPUTensorStats *stats)
{
	TensorCommand cmd;
	uint32_t error;

	/* 非法类别/格式不读取参数；布局的第三源值必须为零。 */
	if ((kind != GPGPU_TENSOR_KIND_MATRIX && kind != GPGPU_TENSOR_KIND_LAYOUT) ||
	    (kind == GPGPU_TENSOR_KIND_MATRIX && format > GPGPU_TENSOR_CONV) ||
	    (kind == GPGPU_TENSOR_KIND_LAYOUT && (format > GPGPU_TENSOR_NEAREST || k_base)))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_INSTRUCTION);
	error = tensor_command_load(ctx, command, &cmd);
	if (error)
		return gpgpu_exec_fail(ctx, error);
	if (kind == GPGPU_TENSOR_KIND_MATRIX)
		error = tensor_matrix_execute(ctx, lane, &cmd, format, command, block, k_base,
					      native_enabled, stats);
	else
		error = tensor_layout_execute(ctx, lane, &cmd, format, command, block, stats);
	if (error)
		return gpgpu_exec_fail(ctx, error);
	return true;
}
