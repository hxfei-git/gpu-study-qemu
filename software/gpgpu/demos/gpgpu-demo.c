/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 以下 Host CPU 代码只负责提供输入和核对 GPU 输出。 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include "gpgpu_kmt.h"
#include "vector_add.h"
#include "matmul.h"
#include "relu.h"

/* 执行示例步骤；出错时记录返回码并跳到统一资源释放路径。 */
#define TRY(call)                                                          \
	do {                                                               \
		rc = (call);                                               \
		if (rc) {                                                  \
			fprintf(stderr, "%s: %s\n", #call, strerror(-rc)); \
			goto out;                                          \
		}                                                          \
	} while (0)

/*
 * verify() - 逐元素校验示例计算结果
 *
 * 比较主机参考值与设备输出，报告不匹配位置与两端数值。
 */
static int verify(const char *name, const float *actual, const float *reference, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (!isfinite(actual[i]) ||
		    fabsf(actual[i] - reference[i]) > 0.00001f * fmaxf(1.0f, fabsf(reference[i]))) {
			fprintf(stderr, "%s[%zu]: GPU=%g CPU=%g\n", name, i, actual[i],
				reference[i]);
			return -1;
		}
	}
	printf("%s: %zu/%zu values PASS\n", name, count, count);
	return 0;
}

typedef struct DemoTask {
	GpgpuKmtMemory code, descriptor, args, staging, ring, progress, signal;
	GpgpuKmtMemory arrays[3];
	GpgpuKmtQueue queue;
	GpgpuKmtEvent event;
} DemoTask;

typedef struct DemoArray {
	float *data;
	size_t bytes;
} DemoArray;

typedef struct DemoDim3 {
	uint32_t x, y, z;
} DemoDim3;

/* 每个例子独占一组对象；卸载未确认时整个任务留存，供关闭流程重试。 */
static GpgpuKmtStatus release_task(DemoTask *task)
{
	GpgpuKmtStatus status;
	GpgpuKmtMemory *objects[] = {
		&task->code, &task->descriptor, &task->args, &task->staging,
		&task->ring, &task->progress, &task->signal,
		&task->arrays[0], &task->arrays[1], &task->arrays[2],
	};

	if (task->queue.context) {
		status = gpgpuKmtDrainQueue(&task->queue, 5000);
		if (status != GPGPU_KMT_SUCCESS && status != GPGPU_KMT_WAIT_FAILURE)
			return status;
		status = gpgpuKmtDestroyQueue(&task->queue);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	if (task->event.context) {
		status = gpgpuKmtDestroyEvent(&task->event);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
		if (!objects[i]->context)
			continue;
		status = gpgpuKmtFreeMemory(objects[i]);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	free(task);
	return GPGPU_KMT_SUCCESS;
}

/* 对象先绑定，随后用同一条 AQL 路径提交；最后一个数组保存输出。 */
static int run_kernel(GpgpuKmtContext *ctx, const void *code, size_t code_size,
                      DemoArray *arrays, size_t array_count, const uint32_t *dimensions,
                      size_t dimension_count, DemoDim3 grid, DemoDim3 block)
{
	DemoTask *t = calloc(1, sizeof(*t));
	GpgpuKmtStatus status = GPGPU_KMT_NO_MEMORY, cleanup;
	GpgpuKernelDescriptor descriptor;
	GpgpuAqlPacket packet = { 0 };
	size_t staging_size = code_size > sizeof(descriptor) ? code_size : sizeof(descriptor);
	size_t args_size = (array_count + dimension_count) * sizeof(uint32_t);
	uint32_t number, *args;
	GpgpuSignal *signal;

	if (!t)
		return -1;
#define KMT(call) do { status = (call); if (status != GPGPU_KMT_SUCCESS) { \
	fprintf(stderr, "%s: %s\n", #call, gpgpuKmtStatusName(status)); goto out; } } while (0)
	for (size_t i = 0; i < array_count; i++) {
		if (arrays[i].bytes > staging_size)
			staging_size = arrays[i].bytes;
		KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_VRAM, arrays[i].bytes, 4096,
		                        &t->arrays[i]));
	}
	KMT(gpgpuKmtAllocMemoryEx(ctx, GPGPU_MEMORY_VRAM, code_size, 4096,
	                          GPGPU_VM_READ | GPGPU_VM_EXEC, 0, &t->code));
	KMT(gpgpuKmtAllocMemoryEx(ctx, GPGPU_MEMORY_VRAM, sizeof(descriptor), 4096,
	                          GPGPU_VM_READ, 0, &t->descriptor));
	KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_HOST, args_size, 4096, &t->args));
	KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_HOST, staging_size, 4096, &t->staging));
	KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_HOST, 4096, 4096, &t->ring));
	KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_HOST, 4096, 4096, &t->progress));
	KMT(gpgpuKmtAllocMemory(ctx, GPGPU_MEMORY_HOST, sizeof(*signal), 4096, &t->signal));
	/* 参数传给 RV32 前逐个校验，不能让 64 位地址静默截断。 */
	if (t->code.gpu_address > UINT32_MAX || t->args.gpu_address > UINT32_MAX) {
		status = GPGPU_KMT_INVALID_PARAMETER;
		goto out;
	}
	args = t->args.cpu;
	for (size_t i = 0; i < array_count; i++) {
		if (t->arrays[i].gpu_address > UINT32_MAX) {
			status = GPGPU_KMT_INVALID_PARAMETER;
			goto out;
		}
		args[i] = htole32(t->arrays[i].gpu_address);
		memcpy(t->staging.cpu, arrays[i].data, arrays[i].bytes);
		KMT(gpgpuKmtCopyMemory(&t->staging, 0, &t->arrays[i], 0,
		                       arrays[i].bytes, GPGPU_COPY_H2D));
	}
	for (size_t i = 0; i < dimension_count; i++)
		args[array_count + i] = htole32(dimensions[i]);
	memcpy(t->staging.cpu, code, code_size);
	KMT(gpgpuKmtCopyMemory(&t->staging, 0, &t->code, 0, code_size, GPGPU_COPY_H2D));
	descriptor = (GpgpuKernelDescriptor) {
		.code_entry = htole32(t->code.gpu_address),
		.code_size = htole32(code_size),
		.kernarg_size = htole32(args_size),
		.kernel_abi_version = htole32(2),
	};
	memcpy(t->staging.cpu, &descriptor, sizeof(descriptor));
	KMT(gpgpuKmtCopyMemory(&t->staging, 0, &t->descriptor, 0,
	                       sizeof(descriptor), GPGPU_COPY_H2D));
	for (size_t i = 0; i < t->ring.size / sizeof(GpgpuAqlPacket); i++)
		((GpgpuAqlPacket *)t->ring.cpu)[i].full_header = htole32(GPGPU_AQL_INVALID);
	KMT(gpgpuKmtCreateQueue(ctx, &t->ring, 0, 4096, &t->progress, 0, 8, &t->queue));
	KMT(gpgpuKmtCreateEvent(&t->queue, &t->event));
	signal = t->signal.cpu;
	*signal = (GpgpuSignal) {
		.value = htole64(1), .event_id = htole32(t->event.id),
		.event_mailbox_ptr = htole64(t->event.mailbox_address),
	};
	packet.full_header = htole32(GPGPU_AQL_HEADER |
	                            ((block.y > 1 || grid.y > 1 ? 2u : 1u) << 16));
	packet.workgroup_size_x = htole16(block.x);
	packet.workgroup_size_y = htole16(block.y);
	packet.workgroup_size_z = htole16(block.z);
	packet.grid_size_x = htole32(grid.x * block.x);
	packet.grid_size_y = htole32(grid.y * block.y);
	packet.grid_size_z = htole32(grid.z * block.z);
	packet.kernel_object = htole64(t->descriptor.gpu_address);
	packet.kernarg_address = htole64(t->args.gpu_address);
	packet.completion_signal = htole64(t->signal.gpu_address);
	KMT(gpgpuKmtReserve(&t->queue, &number));
	KMT(gpgpuKmtPublish(&t->queue, number, &packet));
	KMT(gpgpuKmtWaitSignal(signal, &t->event, 30000));
	KMT(gpgpuKmtCopyMemory(&t->staging, 0, &t->arrays[array_count - 1], 0,
	                       arrays[array_count - 1].bytes, GPGPU_COPY_D2H));
	memcpy(arrays[array_count - 1].data, t->staging.cpu, arrays[array_count - 1].bytes);
out:
	cleanup = release_task(t);
	if (cleanup != GPGPU_KMT_SUCCESS)
		fprintf(stderr, "任务资源保留：%s\n", gpgpuKmtStatusName(cleanup));
	return status == GPGPU_KMT_SUCCESS && cleanup == GPGPU_KMT_SUCCESS ? 0 : -1;
#undef KMT
}

/*
 * vector_add_demo() - 运行 FP32 向量加法示例
 *
 * 分配并上传两个输入，启动 RV32 Kernel，复制结果后逐项比较。
 */
static int vector_add_demo(GpgpuKmtContext *ctx)
{
	/* 覆盖 64 KiB 分块下的两次 DMA 传输和末尾多出的线程。 */
	const size_t count = 17003;
	const size_t bytes = count * sizeof(float);
	float *a = malloc(bytes), *b = malloc(bytes), *out = malloc(bytes);
	float *reference = malloc(bytes);
	DemoDim3 block = { 64, 1, 1 }, grid = { (count + 63) / 64, 1, 1 };
	size_t i;
	int rc = -1;

	if (!a || !b || !out || !reference)
		goto out;
	for (i = 0; i < count; i++) {
		a[i] = ((int)(i % 41) - 20) * 0.25f;
		b[i] = ((int)(i % 29) - 14) * 0.125f;
		out[i] = 123456.5f;
		reference[i] = a[i] + b[i];
	}
	DemoArray arrays[] = { { a, bytes }, { b, bytes }, { out, bytes } };
	uint32_t dimensions[] = { count };
	TRY(run_kernel(ctx, vector_add_code, sizeof(vector_add_code), arrays, 3,
	               dimensions, 1, grid, block));
	rc = verify("vector add (grid=266x1x1, block=64x1x1)", out, reference, count);
out:
	free(a);
	free(b);
	free(out);
	free(reference);
	return rc;
}

/*
 * matmul_demo() - 运行 FP32 矩阵乘示例
 *
 * 准备矩阵和参数，按二维网格执行，再用 CPU 参考乘积验证输出。
 */
static int matmul_demo(GpgpuKmtContext *ctx)
{
	const uint32_t rows = 19, cols = 23, inner = 17;
	const size_t a_bytes = rows * inner * sizeof(float);
	const size_t b_bytes = inner * cols * sizeof(float);
	const size_t out_bytes = rows * cols * sizeof(float);
	float *a = malloc(a_bytes), *b = malloc(b_bytes);
	float *out = malloc(out_bytes), *reference = malloc(out_bytes);
	DemoDim3 block = { 8, 8, 1 }, grid = { 3, 3, 1 };
	uint32_t row, col, k;
	size_t i;
	int rc = -1;

	if (!a || !b || !out || !reference)
		goto out;
	for (i = 0; i < rows * inner; i++)
		a[i] = ((int)(i % 13) - 6) * 0.125f;
	for (i = 0; i < inner * cols; i++)
		b[i] = ((int)(i % 11) - 5) * 0.25f;
	for (row = 0; row < rows; row++) {
		for (col = 0; col < cols; col++) {
			float sum = 0;

			for (k = 0; k < inner; k++)
				sum += a[row * inner + k] * b[k * cols + col];
			reference[row * cols + col] = sum;
			out[row * cols + col] = 123456.5f;
		}
	}
	DemoArray arrays[] = { { a, a_bytes }, { b, b_bytes }, { out, out_bytes } };
	uint32_t dimensions[] = { rows, cols, inner };
	TRY(run_kernel(ctx, matmul_code, sizeof(matmul_code), arrays, 3,
	               dimensions, 3, grid, block));
	rc = verify("matmul (19x17 * 17x23, grid=3x3x1, block=8x8x1)", out, reference, rows * cols);
out:
	free(a);
	free(b);
	free(out);
	free(reference);
	return rc;
}

/*
 * relu_demo() - 运行 FP32 ReLU 示例
 *
 * 上传含正负值的数据，执行 Kernel 并逐元素核对截断结果。
 */
static int relu_demo(GpgpuKmtContext *ctx)
{
	const size_t count = 513, bytes = count * sizeof(float);
	float *input = malloc(bytes), *out = malloc(bytes);
	float *reference = malloc(bytes);
	DemoDim3 block = { 96, 1, 1 }, grid = { 6, 1, 1 };
	size_t i;
	int rc = -1;

	if (!input || !out || !reference)
		goto out;
	for (i = 0; i < count; i++) {
		input[i] = ((int)(i % 53) - 26) * 0.25f;
		reference[i] = fmaxf(0.0f, input[i]);
		out[i] = 123456.5f;
	}
	input[0] = -0.0f;
	reference[0] = 0.0f;
	DemoArray arrays[] = { { input, bytes }, { out, bytes } };
	uint32_t dimensions[] = { count };
	TRY(run_kernel(ctx, relu_code, sizeof(relu_code), arrays, 2,
	               dimensions, 1, grid, block));
	rc = verify("ReLU (grid=6x1x1, block=96x1x1)", out, reference, count);
	if (!rc && signbit(out[0])) {
		fprintf(stderr, "ReLU negative zero did not become positive zero\n");
		rc = -1;
	}
out:
	free(input);
	free(out);
	free(reference);
	return rc;
}

/* 顺序运行三个计算示例；数值比较或资源回收失败均返回非零。 */
int main(int argc, char **argv)
{
	GpgpuKmtContext *ctx = NULL;
	struct gpgpu_vm_info info;
	GpgpuKmtStatus status;
	int rc = EXIT_FAILURE;

	status = gpgpuKmtOpen(argc > 1 ? argv[1] : NULL, &ctx);
	if (status != GPGPU_KMT_SUCCESS) {
		fprintf(stderr, "打开 DRM 设备失败：%s\n", gpgpuKmtStatusName(status));
		return EXIT_FAILURE;
	}
	status = gpgpuKmtGetVmInfo(ctx, &info);
	if (status != GPGPU_KMT_SUCCESS)
		goto out;
	printf("GPGPU DRM ABI=%u GPUVM=%s\n", info.version, info.range_count ? "on" : "off");
	if (vector_add_demo(ctx) || matmul_demo(ctx) || relu_demo(ctx))
		goto out;
	rc = EXIT_SUCCESS;
out:
	status = gpgpuKmtClose(ctx);
	if (status != GPGPU_KMT_SUCCESS) {
		fprintf(stderr, "关闭设备失败，相关资源继续保留：%s\n", gpgpuKmtStatusName(status));
		rc = EXIT_FAILURE;
	}
	if (!rc)
		puts("GPGPU demos: 3/3 PASS");
	return rc;
}
