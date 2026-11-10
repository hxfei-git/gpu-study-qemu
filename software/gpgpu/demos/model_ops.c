// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 模型算子：共享布局检查、数学常量、后端选择与同步分块派发
 */
#include <endian.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "model_ops.h"

/* 单调时钟每秒的纳秒数。 */
#define MODEL_NS_PER_SECOND UINT64_C(1000000000)

/* 命令槽按共享字节偏移访问，命令在 Host 中使用本机字节序。 */
static uint64_t command_get(const uint64_t *words, size_t offset)
{
	return words[offset / GPGPU_EVAL_SLOT_BYTES];
}

/* 上传前只修改本机字节序命令，设备字节序由上传函数统一处理。 */
static void command_set(uint64_t *words, size_t offset, uint64_t value)
{
	words[offset / GPGPU_EVAL_SLOT_BYTES] = value;
}

/* 无符号乘法可表示时写入结果。 */
static bool multiply_ok(uint64_t a, uint64_t b, uint64_t *result)
{
	if (a && b > UINT64_MAX / a)
		return false;
	*result = a * b;
	return true;
}

/* 无符号加法可表示时写入结果。 */
static bool add_ok(uint64_t a, uint64_t b, uint64_t *result)
{
	if (b > UINT64_MAX - a)
		return false;
	*result = a + b;
	return true;
}

/* 必需输入须覆盖完整元素范围，且声明长度按元素宽度对齐。 */
static bool input_size_ok(const uint64_t *input_bytes, unsigned int index, uint64_t elements,
			  uint64_t element_bytes)
{
	uint64_t bytes;

	return index < GPGPU_MODEL_INPUTS && elements &&
		multiply_ok(elements, element_bytes, &bytes) && input_bytes[index] >= bytes &&
		!(input_bytes[index] % element_bytes);
}

/* 连续 shape 的正维度乘积必须可表示。 */
static bool shape_product_ok(const uint64_t *shape, unsigned int rank, uint64_t *elements)
{
	uint64_t product = 1;

	for (unsigned int i = 0; i < rank; i++)
		if (!shape[i] || !multiply_ok(product, shape[i], &product))
			return false;
	*elements = product;
	return true;
}

/*
 * storage_span_ok() - 计算非负元素 stride 的最小可读跨度
 *
 * BMM、Copy 和 RoPE 共用，不能把广播的零 stride 当作连续数组长度。
 * 例：shape=[2,3]、stride=[0,2]，最大下标为 4，至少需要 5 个元素。
 *
 * 1. 累加各轴末项偏移 (shape[i]-1)*stride[i]，乘加溢出时拒绝；
 * 2. 最大下标加一得到跨度，字节换算交给 input_size_ok()。
 *
 * Return: 所有正维度及跨度可表示时为 true。
 */
static bool storage_span_ok(const uint64_t *shape, const uint64_t *stride, unsigned int rank,
			    uint64_t *span)
{
	uint64_t last = 0;

	/* 1. 零 stride 合法，未使用轴的 stride 不扩大访问范围。 */
	for (unsigned int i = 0; i < rank; i++) {
		uint64_t part;

		if (!shape[i] || !multiply_ok(shape[i] - 1, stride[i], &part) ||
		    !add_ok(last, part, &last))
			return false;
	}

	/* 2. 单个元素同样占一个槽，不返回零长度。 */
	return add_ok(last, 1, span);
}

/* 完整元素数按 width 归约后，行数必须仍能按 inner 划分。 */
static bool rows_ok(uint64_t count, uint64_t width, uint64_t inner, uint64_t *rows)
{
	if (!width || !inner || count % width)
		return false;
	*rows = count / width;
	return *rows && !(*rows % inner);
}

/*
 * window_size_ok() - 核对卷积或池化的一维输出及坐标运算
 *
 * 有效核宽为 dilation*(kernel-1)+1。卷积按 floor，池化可选择 floor
 * 或 ceil；ceil 的末窗口不能完全落在右侧 padding。
 * 例：input=5、kernel=2、stride=2、padding=0，floor 输出 2，ceil 输出 3。
 *
 * 1. 检查正维度，并计算有效核宽和补齐输入长度，所有乘加须可表示；
 * 2. 推导输出数并与声明值比较，ceil 允许首个不完整窗口；
 * 3. 限制坐标正值，避免减 padding 后的负坐标回绕成有效下标。
 *
 * 只检查几何，不读取输入，也不处理浮点 padding 值。
 *
 * Return: 输出公式和实际坐标运算均成立时为 true。
 */
static bool window_size_ok(uint64_t input, uint64_t kernel, uint64_t stride, uint64_t padding,
			   uint64_t dilation, uint64_t output, bool ceil_mode)
{
	uint64_t effective, padded, numerator, expected, last, right;

	/* 1. RV64 以回绕值表示负坐标，不能让正坐标跨过符号边界。 */
	if (!input || !kernel || !stride || !dilation || !output || input > INT64_MAX ||
	    stride > INT64_MAX || padding > INT64_MAX || output > INT64_MAX ||
	    !multiply_ok(kernel - 1, dilation, &effective) || !add_ok(effective, 1, &effective) ||
	    effective > INT64_MAX || !multiply_ok(padding, 2, &padded) ||
	    !add_ok(input, padded, &padded) || padded > INT64_MAX ||
	    !add_ok(input, padding, &right))
		return false;

	/* 2. ceil 模式可保留一个短于有效核宽的首窗口。 */
	if (padded < effective) {
		if (!ceil_mode || effective - padded >= stride)
			return false;
		expected = 1;
	} else {
		numerator = padded - effective;
		expected = numerator / stride;
		if (ceil_mode && numerator % stride && !add_ok(expected, 1, &expected))
			return false;
		if (!add_ok(expected, 1, &expected))
			return false;
	}
	if (ceil_mode) {
		if (!multiply_ok(expected - 1, stride, &last))
			return false;
		if (last >= right)
			expected--;
	}
	if (expected != output)
		return false;

	/* 3. 最后一个核元素的正坐标也不得发生回绕。 */
	return multiply_ok(output - 1, stride, &last) && add_ok(last, effective - 1, &last) &&
		last <= INT64_MAX;
}

bool gpgpu_model_layout_valid(const uint64_t *words, const uint64_t *input_bytes,
			      GpgpuModelLayout *layout)
{
	GpgpuModelLayout result = { 0 };
	uint64_t d[GPGPU_MODEL_DIMS];
	uint64_t shape[4], count, op, a, b, n, rows;

	if (!words || !input_bytes || !layout)
		return false;
	count = command_get(words, GPGPU_EVAL_COUNT);
	if (!count)
		return false;
	op = command_get(words, GPGPU_EVAL_OP);
	for (unsigned int i = 0; i < GPGPU_MODEL_DIMS; i++)
		d[i] = command_get(words, GPGPU_EVAL_DIMS + i * GPGPU_EVAL_SLOT_BYTES);
	result.work_count = count;

	switch (op) {
	case GPGPU_EVAL_LINEAR_F32:
	case GPGPU_EVAL_LINEAR_W8:
		if (!d[0] || !d[1] || !d[2] || !multiply_ok(d[0], d[1], &a) || a != count ||
		    !multiply_ok(d[0], d[2], &a) || !multiply_ok(d[1], d[2], &b) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)) ||
		    !input_size_ok(input_bytes, 1, b,
				   op == GPGPU_EVAL_LINEAR_W8 ? sizeof(int8_t) : sizeof(float)) ||
		    (op == GPGPU_EVAL_LINEAR_W8 &&
		     !input_size_ok(input_bytes, 2, d[1], sizeof(float))) ||
		    (input_bytes[3] && !input_size_ok(input_bytes, 3, d[1], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_BMM:
		shape[0] = d[0];
		shape[1] = d[1];
		shape[2] = d[2];
		if (!shape_product_ok(shape, 3, &a) || a != count || !d[3] || d[10] > 1)
			return false;
		shape[2] = d[3];
		if (d[10]) {
			if (!storage_span_ok(shape, d + 4, 3, &a))
				return false;
		} else if (!shape_product_ok(shape, 3, &a)) {
			return false;
		}
		shape[1] = d[3];
		shape[2] = d[2];
		if (d[10]) {
			if (!storage_span_ok(shape, d + 7, 3, &b))
				return false;
		} else if (!shape_product_ok(shape, 3, &b)) {
			return false;
		}
		if (!input_size_ok(input_bytes, 0, a, sizeof(float)) ||
		    !input_size_ok(input_bytes, 1, b, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_EMBEDDING_W8:
		if (!d[0] || count % d[0] || !input_bytes[1] || input_bytes[1] % d[0])
			return false;
		result.vocabulary = input_bytes[1] / d[0];
		if (!input_size_ok(input_bytes, 0, count / d[0], sizeof(int64_t)) ||
		    !input_size_ok(input_bytes, 2, result.vocabulary, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_ADD:
	case GPGPU_EVAL_MUL:
	case GPGPU_EVAL_SUB:
	case GPGPU_EVAL_DIV:
		if (!d[0] || !d[1] || count % d[0] || count % d[1] ||
		    !input_size_ok(input_bytes, 0, d[0], sizeof(float)) ||
		    !input_size_ok(input_bytes, 1, d[1], sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_MUL_SCALAR:
	case GPGPU_EVAL_ADD_SCALAR:
	case GPGPU_EVAL_SILU:
	case GPGPU_EVAL_SIGMOID:
	case GPGPU_EVAL_SIN:
	case GPGPU_EVAL_COS:
		if (!input_size_ok(input_bytes, 0, count, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_REDUCE_MAX:
	case GPGPU_EVAL_REDUCE_SUM:
	case GPGPU_EVAL_REDUCE_RMS:
	case GPGPU_EVAL_DFL_PROJECT:
		if (!d[0] || !d[1] || count % d[1] || !multiply_ok(count, d[0], &a) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_EXP_SHIFT:
	case GPGPU_EVAL_DIV_ROW:
	case GPGPU_EVAL_RMS_APPLY:
	case GPGPU_EVAL_SOFTMAX:
	case GPGPU_EVAL_RMSNORM:
		if (!rows_ok(count, d[0], d[1], &rows) ||
		    !input_size_ok(input_bytes, 0, count, sizeof(float)))
			return false;
		if ((op == GPGPU_EVAL_EXP_SHIFT || op == GPGPU_EVAL_DIV_ROW) &&
		    !input_size_ok(input_bytes, 1, rows, sizeof(float)))
			return false;
		if ((op == GPGPU_EVAL_RMS_APPLY || op == GPGPU_EVAL_RMSNORM) &&
		    !input_size_ok(input_bytes, 1, d[0], sizeof(float)))
			return false;
		if (op == GPGPU_EVAL_RMS_APPLY &&
		    !input_size_ok(input_bytes, 2, rows, sizeof(float)))
			return false;
		if (op == GPGPU_EVAL_SOFTMAX || op == GPGPU_EVAL_RMSNORM)
			result.rows = rows;
		break;
	case GPGPU_EVAL_DFL:
		if (!d[0] || !d[1] || count % d[1] ||
		    !multiply_ok(count, d[0], &result.work_count) ||
		    !input_size_ok(input_bytes, 0, result.work_count, sizeof(float)))
			return false;
		result.rows = count;
		break;
	case GPGPU_EVAL_BATCHNORM:
		if (!d[0] || !d[1] || !multiply_ok(d[0], d[1], &a) || count % a ||
		    !input_size_ok(input_bytes, 0, count, sizeof(float)))
			return false;
		for (unsigned int i = 1; i <= 4; i++)
			if (!input_size_ok(input_bytes, i, d[0], sizeof(float)))
				return false;
		break;
	case GPGPU_EVAL_CONV2D:
		if (!d[0] || !d[14] || d[0] % d[14] || d[3] % d[14] ||
		    !window_size_ok(d[1], d[6], d[8], d[10], d[12], d[4], false) ||
		    !window_size_ok(d[2], d[7], d[9], d[11], d[13], d[5], false))
			return false;
		shape[0] = d[3];
		shape[1] = d[4];
		shape[2] = d[5];
		if (!shape_product_ok(shape, 3, &a) || count % a)
			return false;
		n = count / a;
		shape[0] = n;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		if (!shape_product_ok(shape, 4, &a) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)))
			return false;
		shape[0] = d[3];
		shape[1] = d[0] / d[14];
		shape[2] = d[6];
		shape[3] = d[7];
		if (!shape_product_ok(shape, 4, &a) ||
		    !input_size_ok(input_bytes, 1, a, sizeof(float)) ||
		    (input_bytes[2] && !input_size_ok(input_bytes, 2, d[3], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_MAXPOOL2D:
		if (!(window_size_ok(d[1], d[5], d[7], d[9], d[11], d[3], false) &&
		      window_size_ok(d[2], d[6], d[8], d[10], d[12], d[4], false)) &&
		    !(window_size_ok(d[1], d[5], d[7], d[9], d[11], d[3], true) &&
		      window_size_ok(d[2], d[6], d[8], d[10], d[12], d[4], true)))
			return false;
		shape[0] = d[0];
		shape[1] = d[3];
		shape[2] = d[4];
		if (!shape_product_ok(shape, 3, &a) || count % a)
			return false;
		shape[0] = count / a;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		if (!shape_product_ok(shape, 4, &a) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_NEAREST2D:
		if (!d[0] || !d[1] || !d[2] || !d[3] || !multiply_ok(d[2], d[3], &a) || count % a ||
		    !multiply_ok(d[2] - 1, d[0], &b) || !multiply_ok(d[3] - 1, d[1], &b))
			return false;
		n = count / a;
		if (!multiply_ok(n, d[0], &a) || !multiply_ok(a, d[1], &a) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_CONCAT:
		if (!d[0] || !d[1] || !d[2] || d[2] > GPGPU_MODEL_INPUTS ||
		    !multiply_ok(d[0], d[1], &a) || count % a)
			return false;
		n = count / a;
		b = 0;
		for (unsigned int i = 0; i < d[2]; i++)
			if (!d[3 + i] || !add_ok(b, d[3 + i], &b) ||
			    !multiply_ok(n, d[3 + i], &a) || !multiply_ok(a, d[0], &a) ||
			    !input_size_ok(input_bytes, i, a, sizeof(float)))
				return false;
		if (b != d[1])
			return false;
		break;
	case GPGPU_EVAL_COPY:
		if (d[0] > GPGPU_EVAL_COPY_I64 || d[1] > GPGPU_TENSOR_MAX_RANK)
			return false;
		result.integer_output = d[0] == GPGPU_EVAL_COPY_I64;
		a = count;
		if (d[1] &&
		    (!shape_product_ok(d + 2, d[1], &b) || b != count ||
		     !storage_span_ok(d + 2, d + 2 + d[1], d[1], &a)))
			return false;
		if (!input_size_ok(input_bytes, 0, a,
				   d[0] == GPGPU_EVAL_COPY_F32 ? sizeof(float) : sizeof(int64_t)))
			return false;
		break;
	case GPGPU_EVAL_ROPE:
		if (!d[0] || !d[1] || !d[2] || (d[2] & 1) || d[7] > 1 ||
		    !multiply_ok(d[0], d[1], &a) || !multiply_ok(a, d[2], &a) || count % a)
			return false;
		n = count / a;
		shape[0] = n;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		a = count;
		if (d[7] && !storage_span_ok(shape, d + 3, 4, &a))
			return false;
		if (!multiply_ok(n, d[1], &b) || !multiply_ok(b, d[2], &b) ||
		    !input_size_ok(input_bytes, 0, a, sizeof(float)) ||
		    !input_size_ok(input_bytes, 1, b, sizeof(float)) ||
		    !input_size_ok(input_bytes, 2, b, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_BOX_DECODE:
		if (!d[0] || d[1] > 1 || !multiply_ok(d[0], 4, &a) || count % a)
			return false;
		n = count / a;
		if (!multiply_ok(n, 2, &b) || !multiply_ok(b, d[0], &b) ||
		    !input_size_ok(input_bytes, 0, count, sizeof(float)) ||
		    !input_size_ok(input_bytes, 1, b, sizeof(float)) ||
		    (input_bytes[2] && !input_size_ok(input_bytes, 2, d[0], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_ARANGE:
	case GPGPU_EVAL_FULL:
		if (d[2] != GPGPU_EVAL_TYPE_F32 && d[2] != GPGPU_EVAL_TYPE_I64)
			return false;
		result.integer_output = d[2] == GPGPU_EVAL_TYPE_I64;
		break;
	default:
		return false;
	}

	if (!multiply_ok(count, result.integer_output ? sizeof(int64_t) : sizeof(float),
			 &result.output_bytes))
		return false;
	if (result.rows) {
		if (!multiply_ok(result.rows, sizeof(float), &result.scratch_bytes[0]))
			return false;
		if ((op == GPGPU_EVAL_SOFTMAX || op == GPGPU_EVAL_DFL) &&
		    !multiply_ok(result.work_count, sizeof(float), &result.scratch_bytes[1]))
			return false;
		if (op == GPGPU_EVAL_DFL)
			result.scratch_bytes[2] = result.scratch_bytes[1];
	}
	*layout = result;
	return true;
}

/* 按共享正块宽向上取整，避免 count+width-1 的加法溢出。 */
static uint64_t blocks_for(uint64_t count, uint64_t width)
{
	return count / width + !!(count % width);
}

/* 单调时钟失败必须上报，不能把计时无效的执行报告为成功。 */
static uint64_t clock_ns(GpgpuModelOpStats *stats)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value)) {
		stats->clock_error = true;
		return 0;
	}
	return (uint64_t)value.tv_sec * MODEL_NS_PER_SECOND + value.tv_nsec;
}

/* 参数上传、派发和完整算子的耗时使用同一个单调时钟。 */
static uint64_t elapsed_ns(GpgpuModelOpStats *stats, uint64_t start)
{
	uint64_t end = clock_ns(stats);

	return end >= start ? end - start : 0;
}

/* FP32 位不经过整数数值转换，命令槽高 32 位保持零。 */
static void constant_set(uint64_t *words, size_t offset, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	command_set(words, offset, bits);
}

void gpgpu_model_constants(uint64_t *words)
{
	constant_set(words, GPGPU_EVAL_ONE, 1.0f);
	constant_set(words, GPGPU_EVAL_LN2_HI, 0.693145751953125f);
	constant_set(words, GPGPU_EVAL_LN2_LO, 1.428606765330187045e-6f);
	constant_set(words, GPGPU_EVAL_LOG2E, 1.4426950408889634074f);
	constant_set(words, GPGPU_EVAL_C2, 1.0f / 2.0f);
	constant_set(words, GPGPU_EVAL_C3, 1.0f / 6.0f);
	constant_set(words, GPGPU_EVAL_C4, 1.0f / 24.0f);
	constant_set(words, GPGPU_EVAL_C5, 1.0f / 120.0f);
	constant_set(words, GPGPU_EVAL_C6, 1.0f / 720.0f);
	constant_set(words, GPGPU_EVAL_EXP_LO, -80.0f);
	constant_set(words, GPGPU_EVAL_EXP_HI, 80.0f);
	constant_set(words, GPGPU_EVAL_ZERO, 0.0f);
	constant_set(words, GPGPU_EVAL_NEGONE, -1.0f);
	constant_set(words, GPGPU_EVAL_NEGINF, -INFINITY);
	constant_set(words, GPGPU_EVAL_TWO, 2.0f);
	constant_set(words, GPGPU_EVAL_INV_TWO_PI, 0.15915494309189533577f);
	constant_set(words, GPGPU_EVAL_TWO_PI_HI, 6.28125f);
	constant_set(words, GPGPU_EVAL_TWO_PI_LO, 0.0019353071795864769253f);
	constant_set(words, GPGPU_EVAL_PI, 3.14159265358979323846f);
	constant_set(words, GPGPU_EVAL_HALF_PI, 1.57079632679489661923f);
	constant_set(words, GPGPU_EVAL_INV_FACT7, 1.0f / 5040.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT8, 1.0f / 40320.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT9, 1.0f / 362880.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT10, 1.0f / 3628800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT11, 1.0f / 39916800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT12, 1.0f / 479001600.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT13, 1.0f / 6227020800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT14, (float)(1.0 / 87178291200.0));
}

bool gpgpu_model_launch_valid(const uint64_t *words, uint64_t backend, uint64_t *launch_count,
			      uint64_t *actual_backend)
{
	uint64_t count, launch, actual = GPGPU_EVAL_BACKEND_SCALAR;
	uint64_t m = 0, n = 0, batches = 1, elements, blocks;
	bool matrix = false;

	if (!words || !launch_count || !actual_backend ||
	    (backend != GPGPU_EVAL_BACKEND_SCALAR && backend != GPGPU_EVAL_BACKEND_BLOCK))
		return false;
	count = command_get(words, GPGPU_EVAL_COUNT);
	if (!count)
		return false;
	launch = count;

	switch (command_get(words, GPGPU_EVAL_OP)) {
	case GPGPU_EVAL_LINEAR_F32:
	case GPGPU_EVAL_LINEAR_W8:
		if (backend != GPGPU_EVAL_BACKEND_BLOCK)
			break;
		m = command_get(words, GPGPU_EVAL_D0);
		n = command_get(words, GPGPU_EVAL_D1);
		if (!m || !n || !multiply_ok(m, n, &elements) || elements != count)
			return false;
		matrix = true;
		break;
	case GPGPU_EVAL_BMM:
		if (backend != GPGPU_EVAL_BACKEND_BLOCK)
			break;
		batches = command_get(words, GPGPU_EVAL_D0);
		m = command_get(words, GPGPU_EVAL_D1);
		n = command_get(words, GPGPU_EVAL_D2);
		if (!batches || !m || !n || !multiply_ok(batches, m, &elements) ||
		    !multiply_ok(elements, n, &elements) || elements != count)
			return false;
		matrix = true;
		break;
	case GPGPU_EVAL_CONV2D:
		if (backend != GPGPU_EVAL_BACKEND_BLOCK)
			break;
		n = command_get(words, GPGPU_EVAL_D3);
		blocks = command_get(words, GPGPU_EVAL_D14);
		if (!n || !blocks || n % blocks ||
		    !multiply_ok(command_get(words, GPGPU_EVAL_D4),
				 command_get(words, GPGPU_EVAL_D5), &m) ||
		    !m || !multiply_ok(n, m, &elements) || count % elements ||
		    !multiply_ok(count / elements, blocks, &batches))
			return false;
		n /= blocks;
		matrix = true;
		break;
	case GPGPU_EVAL_COPY:
	case GPGPU_EVAL_CONCAT:
	case GPGPU_EVAL_NEAREST2D:
		if (backend == GPGPU_EVAL_BACKEND_BLOCK) {
			launch = blocks_for(count, GPGPU_TENSOR_BLOCK_ELEMENTS);
			actual = GPGPU_EVAL_BACKEND_BLOCK;
		}
		break;
	case GPGPU_EVAL_EMBEDDING_W8:
	case GPGPU_EVAL_ADD:
	case GPGPU_EVAL_MUL:
	case GPGPU_EVAL_MUL_SCALAR:
	case GPGPU_EVAL_SILU:
	case GPGPU_EVAL_SIGMOID:
	case GPGPU_EVAL_REDUCE_MAX:
	case GPGPU_EVAL_REDUCE_SUM:
	case GPGPU_EVAL_REDUCE_RMS:
	case GPGPU_EVAL_EXP_SHIFT:
	case GPGPU_EVAL_DIV_ROW:
	case GPGPU_EVAL_RMS_APPLY:
	case GPGPU_EVAL_BATCHNORM:
	case GPGPU_EVAL_MAXPOOL2D:
	case GPGPU_EVAL_ROPE:
	case GPGPU_EVAL_DFL_PROJECT:
	case GPGPU_EVAL_BOX_DECODE:
	case GPGPU_EVAL_ADD_SCALAR:
	case GPGPU_EVAL_SUB:
	case GPGPU_EVAL_DIV:
	case GPGPU_EVAL_SIN:
	case GPGPU_EVAL_COS:
	case GPGPU_EVAL_ARANGE:
	case GPGPU_EVAL_FULL:
	case GPGPU_EVAL_SOFTMAX:
	case GPGPU_EVAL_RMSNORM:
	case GPGPU_EVAL_DFL:
		break;
	default:
		return false;
	}

	if (matrix) {
		if (!multiply_ok(batches, blocks_for(m, GPGPU_TENSOR_TILE_M), &blocks) ||
		    !multiply_ok(blocks, blocks_for(n, GPGPU_TENSOR_TILE_N), &launch))
			return false;
		actual = GPGPU_EVAL_BACKEND_BLOCK;
	}
	*launch_count = launch;
	*actual_backend = actual;
	return true;
}

bool gpgpu_model_tile_valid(uint64_t wanted, uint64_t *tile)
{
	uint64_t blocks, limit, aligned;

	if (!wanted || !tile || !GPGPU_MODEL_BLOCK_THREADS ||
	    GPGPU_MODEL_BLOCK_THREADS > GPGPU_MAX_BLOCK_THREADS)
		return false;
	blocks = GPGPU_MAX_TOTAL_THREADS / GPGPU_MODEL_BLOCK_THREADS;
	if (blocks > GPGPU_MAX_BLOCKS)
		blocks = GPGPU_MAX_BLOCKS;
	if (blocks > UINT32_MAX)
		blocks = UINT32_MAX;
	if (!blocks || !multiply_ok(blocks, GPGPU_MODEL_BLOCK_THREADS, &limit))
		return false;
	wanted = wanted < limit ? wanted : limit;
	blocks = blocks_for(wanted, GPGPU_MODEL_BLOCK_THREADS);
	if (!multiply_ok(blocks, GPGPU_MODEL_BLOCK_THREADS, &aligned) || aligned > limit)
		return false;
	*tile = aligned;
	return true;
}

/*
 * upload_command() - 上传一个完整的小端命令
 *
 * 每步派发前调用，避免设备看见只更新了一半的字段。
 *
 * 1. 将完整本机字节序模板编码为小端；
 * 2. 同步上传到复用的命令 BO，只有成功才累计参数上传统计。
 *
 * 不负责派发；上传失败前已经完成的传输不回退。
 *
 * Return: 上传返回的 KMT 状态。
 */
static GpgpuKmtStatus upload_command(GpgpuModelExecutor *executor, const uint64_t *words,
				     GpgpuModelOpStats *stats)
{
	uint64_t wire[GPGPU_MODEL_WORDS];
	uint64_t start = clock_ns(stats);
	GpgpuKmtStatus status;

	/* 1. 包括未使用槽在内，始终覆盖整个设备命令块。 */
	for (unsigned int i = 0; i < GPGPU_MODEL_WORDS; i++)
		wire[i] = htole64(words[i]);

	/* 2. 成功后才能复用 Host 参数，后续 tile 不再上传此命令。 */
	status = gpgpu_rt_upload_range(executor->rt, executor->command, 0, wire, sizeof(wire));
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	stats->params_ns += elapsed_ns(stats, start);
	stats->params_bytes += sizeof(wire);
	return GPGPU_KMT_SUCCESS;
}

/*
 * dispatch_step() - 执行一个 lowering 步骤的全部工作项
 *
 * 每步从本节点的完整模板复制命令，不继承上一步覆盖的输入输出。
 * BASE 只改变全局工作项编号，不偏移数组指针。
 * 例：tile=128、COUNT=300，BASE 为 0、128、256；尾部 300..319 不访存。
 *
 * 1. 复制完整模板，只覆盖 OP、COUNT 和本步必要的输入输出；
 * 2. 上传一次命令，再让 RT_BASE 逐 tile 前进；
 * 3. 记录派发尝试，每个 tile 同步完成后才发布下一个。
 *
 * 已完成的 tile 不回退；对象回收与未知状态由调用者处理。
 *
 * Return: 全部 tile 成功为 SUCCESS，否则保留上传或派发的错误。
 */
static GpgpuKmtStatus dispatch_step(GpgpuModelExecutor *executor, GpgpuModelOpStats *stats,
				    const uint64_t *template, uint64_t op, uint64_t count,
				    uint64_t in0, uint64_t in1, uint64_t in2, uint64_t output)
{
	uint64_t words[GPGPU_MODEL_WORDS];
	uint64_t args[GPGPU_MODEL_RT_ARGS] = { 0 };
	GpgpuKmtStatus status;

	/* 1. 后三个输入及全部 DIMS、常量和 scratch 始终来自本节点模板。 */
	memcpy(words, template, sizeof(words));
	command_set(words, GPGPU_EVAL_OP, op);
	command_set(words, GPGPU_EVAL_COUNT, count);
	command_set(words, GPGPU_EVAL_IN0, in0);
	command_set(words, GPGPU_EVAL_IN1, in1);
	command_set(words, GPGPU_EVAL_IN2, in2);
	command_set(words, GPGPU_EVAL_OUT, output);

	/* 2. 命令 BO 只在已经确认前一步完成后复用。 */
	status = upload_command(executor, words, stats);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	args[GPGPU_EVAL_RT_COMMAND / GPGPU_EVAL_SLOT_BYTES] = executor->command->gpu_address;
	for (uint64_t base = 0; base < count;) {
		uint64_t length = count - base > executor->tile ? executor->tile : count - base;
		uint64_t blocks = blocks_for(length, GPGPU_MODEL_BLOCK_THREADS);
		uint64_t start = clock_ns(stats);

		/* 3. 统计逻辑工作项和派发尝试，不计尾部补齐线程。 */
		args[GPGPU_EVAL_RT_BASE / GPGPU_EVAL_SLOT_BYTES] = base;
		stats->work_items += length;
		stats->tile_dispatches++;
		status = gpgpu_rt_dispatch(executor->rt, executor->kernel, args,
					   GPGPU_MODEL_RT_ARGS, (GpgpuRtDim3) { blocks, 1, 1 },
					   (GpgpuRtDim3) { GPGPU_MODEL_BLOCK_THREADS, 1, 1 },
					   executor->timeout_ms);
		stats->dispatch_ns += elapsed_ns(stats, start);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		base += length;
	}
	return GPGPU_KMT_SUCCESS;
}

/*
 * softmax_steps() - 顺序执行 Softmax 的四个标量步骤
 *
 * Softmax 与 DFL 共用，max 与 sum 复用 AUX0，指数数组保存在 AUX1。
 *
 * 1. 按 rows 归约输入最大值，再按 work_count 生成移位指数；
 * 2. 按 rows 对指数求和，再按 work_count 除以行和写入指定输出。
 *
 * 本函数不做 DFL 投影，也不分配 scratch；任一步失败立即停止。
 *
 * Return: 全部步骤成功为 SUCCESS，否则保留失败步骤的状态。
 */
static GpgpuKmtStatus softmax_steps(GpgpuModelExecutor *executor, GpgpuModelOpStats *stats,
				    const uint64_t *words, uint64_t rows, uint64_t work_count,
				    uint64_t output)
{
	uint64_t input = command_get(words, GPGPU_EVAL_IN0);
	uint64_t row_stats = command_get(words, GPGPU_EVAL_AUX0);
	uint64_t exponentials = command_get(words, GPGPU_EVAL_AUX1);
	GpgpuKmtStatus status;

	/* 1. 原输入保持不变，指数写入单独的中间数组。 */
	status = dispatch_step(executor, stats, words, GPGPU_EVAL_REDUCE_MAX, rows, input, 0, 0,
			       row_stats);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = dispatch_step(executor, stats, words, GPGPU_EVAL_EXP_SHIFT, work_count, input,
			       row_stats, 0, exponentials);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 2. 最大值不再使用后，才复用同一 scratch 存放行和。 */
	status = dispatch_step(executor, stats, words, GPGPU_EVAL_REDUCE_SUM, rows, exponentials, 0,
			       0, row_stats);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	return dispatch_step(executor, stats, words, GPGPU_EVAL_DIV_ROW, work_count, exponentials,
			     row_stats, 0, output);
}

GpgpuKmtStatus gpgpu_model_execute(GpgpuModelExecutor *executor, const uint64_t *words,
				   uint64_t rows, uint64_t work_count, uint64_t backend,
				   GpgpuModelOpStats *stats)
{
	uint64_t command[GPGPU_MODEL_WORDS];
	uint64_t input[GPGPU_MODEL_INPUTS];
	uint64_t count, op, output, launch_count, actual_backend, tile, start;
	GpgpuKmtStatus status;

	if (!stats)
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(stats, 0, sizeof(*stats));
	if (!executor || !executor->rt || !executor->kernel || !executor->command ||
	    executor->command->size < GPGPU_EVAL_PARAM_BYTES ||
	    !gpgpu_model_tile_valid(executor->tile, &tile) || tile != executor->tile ||
	    !gpgpu_model_launch_valid(words, backend, &launch_count, &actual_backend))
		return GPGPU_KMT_INVALID_PARAMETER;
	op = command_get(words, GPGPU_EVAL_OP);
	if ((op == GPGPU_EVAL_SOFTMAX || op == GPGPU_EVAL_RMSNORM || op == GPGPU_EVAL_DFL) &&
	    (!rows || !work_count))
		return GPGPU_KMT_INVALID_PARAMETER;

	start = clock_ns(stats);
	memcpy(command, words, sizeof(command));
	for (unsigned int i = 0; i < GPGPU_MODEL_INPUTS; i++)
		input[i] = command_get(command, GPGPU_EVAL_IN0 + i * GPGPU_EVAL_SLOT_BYTES);
	count = command_get(command, GPGPU_EVAL_COUNT);
	output = command_get(command, GPGPU_EVAL_OUT);
	switch (op) {
	case GPGPU_EVAL_SOFTMAX:
		status = softmax_steps(executor, stats, command, rows, work_count, output);
		break;
	case GPGPU_EVAL_RMSNORM:
		status = dispatch_step(executor, stats, command, GPGPU_EVAL_REDUCE_RMS, rows,
				       input[0], 0, 0, command_get(command, GPGPU_EVAL_AUX0));
		if (status == GPGPU_KMT_SUCCESS)
			status = dispatch_step(executor, stats, command, GPGPU_EVAL_RMS_APPLY,
					       count, input[0], input[1],
					       command_get(command, GPGPU_EVAL_AUX0), output);
		break;
	case GPGPU_EVAL_DFL:
		status = softmax_steps(executor, stats, command, rows, work_count,
				       command_get(command, GPGPU_EVAL_AUX2));
		if (status == GPGPU_KMT_SUCCESS)
			status = dispatch_step(executor, stats, command, GPGPU_EVAL_DFL_PROJECT,
					       count, command_get(command, GPGPU_EVAL_AUX2), 0, 0,
					       output);
		break;
	default:
		if (actual_backend == GPGPU_EVAL_BACKEND_BLOCK) {
			/* 仅块基本算子复用 AUX，复合步骤保留 scratch。 */
			command_set(command, GPGPU_EVAL_AUX1, count);
			command_set(command, GPGPU_EVAL_AUX2, GPGPU_EVAL_BACKEND_BLOCK);
		}
		status = dispatch_step(executor, stats, command, op, launch_count, input[0],
				       input[1], input[2], output);
		break;
	}
	stats->operator_ns = elapsed_ns(stats, start);
	if (stats->clock_error && status == GPGPU_KMT_SUCCESS)
		return GPGPU_KMT_ERROR;
	return status;
}
