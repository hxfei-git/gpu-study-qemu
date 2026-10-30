// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 示例：向量加法、矩阵乘法与 ReLU
 *
 * 每个示例：
 *   1. Host 准备输入与 CPU 参考结果；
 *   2. 经 gpgpu_rt 装载 Kernel、上传输入、派发并下载输出；
 *   3. 逐元素比较。
 * 任一比较、提交、等待或资源回收失败，程序返回非零。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpgpu_rt.h"
#include "vector_add.h"
#include "matmul.h"
#include "relu.h"

/* 一次派发的等待上限，单位毫秒。 */
#define DEMO_TIMEOUT_MS 30000
/* 输出数组的初值，用来发现 Kernel 未写的元素。 */
#define DEMO_POISON 123456.5f

/* 一个输入或输出数组。 */
typedef struct DemoArray {
	float *host;
	size_t bytes;
} DemoArray;

/* 一个示例的 Kernel、数组与网格；最后一个数组是输出。 */
typedef struct DemoJob {
	const uint8_t *code;
	size_t code_size;
	DemoArray *arrays;
	uint32_t array_count;
	const uint32_t *scalars;
	uint32_t scalar_count;
	GpgpuRtDim3 grid;
	GpgpuRtDim3 block;
} DemoJob;

/* status 非成功时打印“what：原因”并返回 -1，成功返回 0。 */
static int check(GpgpuKmtStatus status, const char *what)
{
	if (status == GPGPU_KMT_SUCCESS)
		return 0;
	fprintf(stderr, "%s：%s\n", what, gpgpuKmtStatusString(status));
	return -1;
}

/* 相对误差 1e-5 以内视为相等；输出必须是有限值。 */
static int verify(const char *name, const float *actual, const float *expected, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (!isfinite(actual[i]) ||
		    fabsf(actual[i] - expected[i]) > 0.00001f * fmaxf(1.0f, fabsf(expected[i]))) {
			fprintf(stderr, "%s[%zu]: GPU=%g CPU=%g\n", name, i, actual[i], expected[i]);
			return -1;
		}
	}
	printf("%s: %zu/%zu values PASS\n", name, count, count);
	return 0;
}

/*
 * run_job() - 在 GPU 上执行一个示例，输出写回最后一个数组的 host 缓冲区
 *
 * 1. 建立运行时，暂存区取最大数组与代码中较大者；
 * 2. 装载 Kernel，为每个数组分配 VRAM 并上传；参数块依次为各数组地址与标量；
 * 3. 派发并下载输出；
 * 4. 回收运行时（失败也回收）。
 */
static int run_job(GpgpuKmtContext *ctx, const DemoJob *job)
{
	uint32_t args[GPGPU_RT_MAX_ARGS], arg_count = job->array_count + job->scalar_count;
	GpgpuKmtMemory *buffers[GPGPU_RT_MAX_ARGS];
	DemoArray *out = &job->arrays[job->array_count - 1];
	size_t staging = job->code_size;
	GpgpuRtKernel kernel;
	GpgpuRt *rt = NULL;
	int rc = -1;

	if (arg_count > GPGPU_RT_MAX_ARGS)
		return -1;

	/* 1. 暂存区要能容纳最大的一次上传、下载或代码。 */
	for (uint32_t i = 0; i < job->array_count; i++)
		staging = job->arrays[i].bytes > staging ? job->arrays[i].bytes : staging;
	if (check(gpgpu_rt_create(ctx, staging, &rt), "建立运行时"))
		return -1;

	/*
	 * 2. 第一次派发前必须分配完所有对象。参数块前 array_count 个是
	 *    数组的 32 位设备地址，后面接标量。
	 */
	if (check(gpgpu_rt_kernel_load(rt, job->code, job->code_size, arg_count, &kernel),
		  "装载 Kernel"))
		goto out;
	for (uint32_t i = 0; i < job->array_count; i++) {
		if (check(gpgpu_rt_buffer_alloc(rt, job->arrays[i].bytes, &buffers[i]), "分配数组") ||
		    check(gpgpu_rt_upload(rt, buffers[i], job->arrays[i].host, job->arrays[i].bytes),
			  "上传数组") ||
		    check(gpgpu_rt_arg_address(buffers[i], &args[i]), "数组地址"))
			goto out;
	}
	memcpy(&args[job->array_count], job->scalars, job->scalar_count * sizeof(uint32_t));

	/* 3. 派发是同步的，返回时输出已写完，再下载最后一个数组。 */
	if (check(gpgpu_rt_dispatch(rt, &kernel, args, arg_count, job->grid, job->block,
				    DEMO_TIMEOUT_MS),
		  "派发") ||
	    check(gpgpu_rt_download(rt, buffers[job->array_count - 1], out->host, out->bytes),
		  "下载输出"))
		goto out;
	rc = 0;
out:
	/* 4. 成败都回收运行时；回收失败也算示例失败。 */
	if (check(gpgpu_rt_destroy(rt), "回收运行时"))
		rc = -1;
	return rc;
}

/*
 * 17,003 个元素，不是 64 的整数倍：266 个工作组中最后一组只有 43 个
 * 线程对应有效元素，Kernel 须按 count 跳过其余线程。
 */
static int vector_add_demo(GpgpuKmtContext *ctx)
{
	const uint32_t count = 17003;
	const size_t bytes = count * sizeof(float);
	float *a = malloc(bytes), *b = malloc(bytes), *out = malloc(bytes), *ref = malloc(bytes);
	DemoArray arrays[] = { { a, bytes }, { b, bytes }, { out, bytes } };
	const uint32_t scalars[] = { count };
	DemoJob job = {
		.code = vector_add_code, .code_size = sizeof(vector_add_code),
		.arrays = arrays, .array_count = 3, .scalars = scalars, .scalar_count = 1,
		.grid = { (count + 63) / 64, 1, 1 }, .block = { 64, 1, 1 },
	};
	int rc = -1;

	if (!a || !b || !out || !ref)
		goto out;
	for (uint32_t i = 0; i < count; i++) {
		a[i] = ((int)(i % 41) - 20) * 0.25f;
		b[i] = ((int)(i % 29) - 14) * 0.125f;
		out[i] = DEMO_POISON;
		ref[i] = a[i] + b[i];
	}
	if (!run_job(ctx, &job))
		rc = verify("vector add (grid=266x1x1, block=64x1x1)", out, ref, count);
out:
	free(a);
	free(b);
	free(out);
	free(ref);
	return rc;
}

/* C[19][23] = A[19][17] · B[17][23]，二维网格。 */
static int matmul_demo(GpgpuKmtContext *ctx)
{
	const uint32_t rows = 19, cols = 23, inner = 17;
	const size_t a_bytes = rows * inner * sizeof(float);
	const size_t b_bytes = inner * cols * sizeof(float);
	const size_t out_bytes = rows * cols * sizeof(float);
	float *a = malloc(a_bytes), *b = malloc(b_bytes);
	float *out = malloc(out_bytes), *ref = malloc(out_bytes);
	DemoArray arrays[] = { { a, a_bytes }, { b, b_bytes }, { out, out_bytes } };
	const uint32_t scalars[] = { rows, cols, inner };
	DemoJob job = {
		.code = matmul_code, .code_size = sizeof(matmul_code),
		.arrays = arrays, .array_count = 3, .scalars = scalars, .scalar_count = 3,
		.grid = { 3, 3, 1 }, .block = { 8, 8, 1 },
	};
	int rc = -1;

	if (!a || !b || !out || !ref)
		goto out;
	for (uint32_t i = 0; i < rows * inner; i++)
		a[i] = ((int)(i % 13) - 6) * 0.125f;
	for (uint32_t i = 0; i < inner * cols; i++)
		b[i] = ((int)(i % 11) - 5) * 0.25f;
	for (uint32_t r = 0; r < rows; r++) {
		for (uint32_t c = 0; c < cols; c++) {
			float sum = 0;

			for (uint32_t k = 0; k < inner; k++)
				sum += a[r * inner + k] * b[k * cols + c];
			ref[r * cols + c] = sum;
			out[r * cols + c] = DEMO_POISON;
		}
	}
	if (!run_job(ctx, &job))
		rc = verify("matmul (19x17 * 17x23, grid=3x3x1, block=8x8x1)", out, ref,
			    rows * cols);
out:
	free(a);
	free(b);
	free(out);
	free(ref);
	return rc;
}

/* ReLU：负数与 -0.0 都应变为 +0.0。 */
static int relu_demo(GpgpuKmtContext *ctx)
{
	const uint32_t count = 513;
	const size_t bytes = count * sizeof(float);
	float *in = malloc(bytes), *out = malloc(bytes), *ref = malloc(bytes);
	DemoArray arrays[] = { { in, bytes }, { out, bytes } };
	const uint32_t scalars[] = { count };
	DemoJob job = {
		.code = relu_code, .code_size = sizeof(relu_code),
		.arrays = arrays, .array_count = 2, .scalars = scalars, .scalar_count = 1,
		.grid = { 6, 1, 1 }, .block = { 96, 1, 1 },
	};
	int rc = -1;

	if (!in || !out || !ref)
		goto out;
	for (uint32_t i = 0; i < count; i++) {
		in[i] = ((int)(i % 53) - 26) * 0.25f;
		ref[i] = fmaxf(0.0f, in[i]);
		out[i] = DEMO_POISON;
	}
	in[0] = -0.0f;
	ref[0] = 0.0f;
	if (!run_job(ctx, &job))
		rc = verify("ReLU (grid=6x1x1, block=96x1x1)", out, ref, count);
	if (!rc && signbit(out[0])) {
		fprintf(stderr, "ReLU negative zero did not become positive zero\n");
		rc = -1;
	}
out:
	free(in);
	free(out);
	free(ref);
	return rc;
}

int main(int argc, char **argv)
{
	GpgpuKmtContext *ctx = NULL;
	struct gpgpu_vm_info info;
	int rc = EXIT_FAILURE;

	if (check(gpgpuKmtOpen(argc > 1 ? argv[1] : NULL, &ctx), "打开 DRM 设备"))
		return EXIT_FAILURE;
	if (check(gpgpuKmtGetVmInfo(ctx, &info), "查询 GPUVM"))
		goto out;
	printf("GPGPU DRM ABI=%u GPUVM=%s\n", info.version, info.range_count ? "on" : "off");

	if (vector_add_demo(ctx) || matmul_demo(ctx) || relu_demo(ctx))
		goto out;
	rc = EXIT_SUCCESS;
out:
	if (check(gpgpuKmtClose(ctx), "关闭设备（相关资源继续保留）"))
		rc = EXIT_FAILURE;
	if (rc == EXIT_SUCCESS)
		puts("GPGPU demos: 3/3 PASS");
	return rc;
}
