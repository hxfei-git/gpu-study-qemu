/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host CPU work below supplies inputs and verifies GPU outputs only. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpgpu.h"
#include "vector_add.h"
#include "matmul.h"
#include "relu.h"

#define TRY(call) do { \
    rc = (call); \
    if (rc) { \
        fprintf(stderr, "%s: %s\n", #call, strerror(-rc)); \
        goto out; \
    } \
} while (0)

static int verify(const char *name, const float *actual,
                  const float *reference, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (!isfinite(actual[i]) ||
            fabsf(actual[i] - reference[i]) >
            0.00001f * fmaxf(1.0f, fabsf(reference[i]))) {
            fprintf(stderr, "%s[%zu]: GPU=%g CPU=%g\n",
                    name, i, actual[i], reference[i]);
            return -1;
        }
    }
    printf("%s: %zu/%zu values PASS\n", name, count, count);
    return 0;
}

static void release(GpgpuContext *ctx, GpgpuKernel *kernel,
                    GpgpuDevicePtr *addresses, size_t count)
{
    size_t i;

    if (kernel) {
        gpgpuUnloadKernel(ctx, kernel);
    }
    for (i = 0; i < count; i++) {
        if (addresses[i]) {
            gpgpuFree(ctx, addresses[i]);
        }
    }
}

static int vector_add_demo(GpgpuContext *ctx)
{
    const size_t count = 17003; /* Two 64 KiB DMA chunks and tail threads. */
    const size_t bytes = count * sizeof(float);
    float *a = malloc(bytes), *b = malloc(bytes), *out = malloc(bytes);
    float *reference = malloc(bytes);
    GpgpuDevicePtr addresses[3] = { 0 };
    GpgpuKernel *kernel = NULL;
    uint32_t args[4];
    GpgpuDim3 block = { 64, 1, 1 }, grid = { (count + 63) / 64, 1, 1 };
    size_t i;
    int rc = -1;

    if (!a || !b || !out || !reference) {
        goto out;
    }
    for (i = 0; i < count; i++) {
        a[i] = ((int)(i % 41) - 20) * 0.25f;
        b[i] = ((int)(i % 29) - 14) * 0.125f;
        out[i] = 123456.5f;
        reference[i] = a[i] + b[i];
    }
    TRY(gpgpuMalloc(ctx, &addresses[0], bytes));
    TRY(gpgpuMalloc(ctx, &addresses[1], bytes));
    TRY(gpgpuMalloc(ctx, &addresses[2], bytes));
    TRY(gpgpuMemcpy(ctx, addresses[0], a, bytes, GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuMemcpy(ctx, addresses[1], b, bytes, GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuMemcpy(ctx, addresses[2], out, bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuLoadKernel(ctx, &kernel, vector_add_code,
                        sizeof(vector_add_code)));
    args[0] = addresses[0];
    args[1] = addresses[1];
    args[2] = addresses[2];
    args[3] = count;
    TRY(gpgpuLaunchKernel(ctx, kernel, grid, block, args, sizeof(args), 0));
    TRY(gpgpuDeviceSynchronize(ctx));
    TRY(gpgpuMemcpy(ctx, addresses[2], out, bytes,
                    GPGPU_MEMCPY_DEVICE_TO_HOST));
    rc = verify("vector add (grid=266x1x1, block=64x1x1)",
                out, reference, count);
out:
    release(ctx, kernel, addresses, 3);
    free(a);
    free(b);
    free(out);
    free(reference);
    return rc;
}

static int matmul_demo(GpgpuContext *ctx)
{
    const uint32_t rows = 19, cols = 23, inner = 17;
    const size_t a_bytes = rows * inner * sizeof(float);
    const size_t b_bytes = inner * cols * sizeof(float);
    const size_t out_bytes = rows * cols * sizeof(float);
    float *a = malloc(a_bytes), *b = malloc(b_bytes);
    float *out = malloc(out_bytes), *reference = malloc(out_bytes);
    GpgpuDevicePtr addresses[3] = { 0 };
    GpgpuKernel *kernel = NULL;
    uint32_t args[6];
    GpgpuDim3 block = { 8, 8, 1 }, grid = { 3, 3, 1 };
    uint32_t row, col, k;
    size_t i;
    int rc = -1;

    if (!a || !b || !out || !reference) {
        goto out;
    }
    for (i = 0; i < rows * inner; i++) {
        a[i] = ((int)(i % 13) - 6) * 0.125f;
    }
    for (i = 0; i < inner * cols; i++) {
        b[i] = ((int)(i % 11) - 5) * 0.25f;
    }
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            float sum = 0;

            for (k = 0; k < inner; k++) {
                sum += a[row * inner + k] * b[k * cols + col];
            }
            reference[row * cols + col] = sum;
            out[row * cols + col] = 123456.5f;
        }
    }
    TRY(gpgpuMalloc(ctx, &addresses[0], a_bytes));
    TRY(gpgpuMalloc(ctx, &addresses[1], b_bytes));
    TRY(gpgpuMalloc(ctx, &addresses[2], out_bytes));
    TRY(gpgpuMemcpy(ctx, addresses[0], a, a_bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuMemcpy(ctx, addresses[1], b, b_bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuMemcpy(ctx, addresses[2], out, out_bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuLoadKernel(ctx, &kernel, matmul_code, sizeof(matmul_code)));
    args[0] = addresses[0];
    args[1] = addresses[1];
    args[2] = addresses[2];
    args[3] = rows;
    args[4] = cols;
    args[5] = inner;
    TRY(gpgpuLaunchKernel(ctx, kernel, grid, block, args, sizeof(args), 0));
    TRY(gpgpuMemcpy(ctx, addresses[2], out, out_bytes,
                    GPGPU_MEMCPY_DEVICE_TO_HOST));
    rc = verify("matmul (19x17 * 17x23, grid=3x3x1, block=8x8x1)",
                out, reference, rows * cols);
out:
    release(ctx, kernel, addresses, 3);
    free(a);
    free(b);
    free(out);
    free(reference);
    return rc;
}

static int relu_demo(GpgpuContext *ctx)
{
    const size_t count = 513, bytes = count * sizeof(float);
    float *input = malloc(bytes), *out = malloc(bytes);
    float *reference = malloc(bytes);
    GpgpuDevicePtr addresses[2] = { 0 };
    GpgpuKernel *kernel = NULL;
    uint32_t args[3];
    GpgpuDim3 block = { 96, 1, 1 }, grid = { 6, 1, 1 };
    size_t i;
    int rc = -1;

    if (!input || !out || !reference) {
        goto out;
    }
    for (i = 0; i < count; i++) {
        input[i] = ((int)(i % 53) - 26) * 0.25f;
        reference[i] = fmaxf(0.0f, input[i]);
        out[i] = 123456.5f;
    }
    input[0] = -0.0f;
    reference[0] = 0.0f;
    TRY(gpgpuMalloc(ctx, &addresses[0], bytes));
    TRY(gpgpuMalloc(ctx, &addresses[1], bytes));
    TRY(gpgpuMemcpy(ctx, addresses[0], input, bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuMemcpy(ctx, addresses[1], out, bytes,
                    GPGPU_MEMCPY_HOST_TO_DEVICE));
    TRY(gpgpuLoadKernel(ctx, &kernel, relu_code, sizeof(relu_code)));
    args[0] = addresses[0];
    args[1] = addresses[1];
    args[2] = count;
    TRY(gpgpuLaunchKernel(ctx, kernel, grid, block, args, sizeof(args), 0));
    TRY(gpgpuMemcpy(ctx, addresses[1], out, bytes,
                    GPGPU_MEMCPY_DEVICE_TO_HOST));
    rc = verify("ReLU (grid=6x1x1, block=96x1x1)", out, reference, count);
    if (!rc && signbit(out[0])) {
        fprintf(stderr, "ReLU negative zero did not become positive zero\n");
        rc = -1;
    }
out:
    release(ctx, kernel, addresses, 2);
    free(input);
    free(out);
    free(reference);
    return rc;
}

int main(int argc, char **argv)
{
    GpgpuContext *ctx = NULL;
    struct gpgpu_stats before, after;
    struct gpgpu_info info;
    int rc = 0;

    TRY(gpgpuInit(&ctx, argc > 1 ? argv[1] : NULL));
    TRY(gpgpuGetInfo(ctx, &info));
    printf("GPGPU ABI=%u VRAM=%u staging=%u caps=0x%x\n",
           info.abi_version, info.vram_size, info.staging_size, info.caps);
    TRY(gpgpuGetStats(ctx, &before));
    TRY(vector_add_demo(ctx));
    TRY(matmul_demo(ctx));
    TRY(relu_demo(ctx));
    TRY(gpgpuGetStats(ctx, &after));
    printf("MSI-X deltas: kernel=%" PRIu64 " DMA=%" PRIu64 " error=%" PRIu64
           "\n", (uint64_t)(after.kernel_irqs - before.kernel_irqs),
           (uint64_t)(after.dma_irqs - before.dma_irqs),
           (uint64_t)(after.error_irqs - before.error_irqs));
    if (after.kernel_irqs - before.kernel_irqs != 3 ||
        after.dma_irqs <= before.dma_irqs ||
        after.error_irqs != before.error_irqs) {
        fprintf(stderr, "missing completion IRQ or unexpected device error\n");
        rc = -1;
        goto out;
    }
    puts("GPGPU demos: 3/3 PASS");
out:
    gpgpuDestroy(ctx);
    return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
