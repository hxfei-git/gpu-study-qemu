/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIBGPGPU_H
#define LIBGPGPU_H

#include <stddef.h>
#include <stdint.h>
#include "gpgpu_uapi.h"

typedef struct GpgpuContext GpgpuContext;
typedef struct GpgpuKernel GpgpuKernel;
typedef uint32_t GpgpuDevicePtr;

typedef struct GpgpuDim3 {
    uint32_t x, y, z;
} GpgpuDim3;

typedef enum GpgpuMemcpyKind {
    GPGPU_MEMCPY_HOST_TO_DEVICE = GPGPU_COPY_H2D,
    GPGPU_MEMCPY_DEVICE_TO_HOST = GPGPU_COPY_D2H,
} GpgpuMemcpyKind;

/* 各调用同步执行，返回零或负 errno 错误码。 */
int gpgpuInit(GpgpuContext **out, const char *device);
void gpgpuDestroy(GpgpuContext *ctx);
int gpgpuMalloc(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes);
int gpgpuFree(GpgpuContext *ctx, GpgpuDevicePtr address);
int gpgpuMemcpy(GpgpuContext *ctx, GpgpuDevicePtr device_address,
                void *host, size_t bytes, GpgpuMemcpyKind kind);
int gpgpuLoadKernel(GpgpuContext *ctx, GpgpuKernel **out,
                   const void *code, size_t bytes);
int gpgpuUnloadKernel(GpgpuContext *ctx, GpgpuKernel *kernel);
/* Args 由小端 32 位字段紧密排列而成，a0 指向该参数块。 */
int gpgpuLaunchKernel(GpgpuContext *ctx, GpgpuKernel *kernel,
                      GpgpuDim3 grid, GpgpuDim3 block,
                      const void *args, size_t args_bytes,
                      uint32_t shared_bytes);
int gpgpuDeviceSynchronize(GpgpuContext *ctx);
int gpgpuGetInfo(GpgpuContext *ctx, struct gpgpu_info *info);
int gpgpuGetStats(GpgpuContext *ctx, struct gpgpu_stats *stats);

#endif /* LIBGPGPU_H */
