/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIBGPGPU_H
#define LIBGPGPU_H

#include <stddef.h>
#include <stdint.h>
#include "gpgpu_uapi.h"

typedef struct GpgpuContext GpgpuContext;
typedef struct GpgpuKernel GpgpuKernel;
typedef uint32_t GpgpuDevicePtr;

/**
 * struct GpgpuDim3 - 工作组数或线程数的三维取值
 * @x: X 维大小，线性编号中变化最快。
 * @y: Y 维大小。
 * @z: Z 维大小。
 */
typedef struct GpgpuDim3 {
	uint32_t x;
	uint32_t y;
	uint32_t z;
} GpgpuDim3;

/**
 * enum GpgpuMemcpyKind - 同步运行时的 DMA 拷贝方向
 * @GPGPU_MEMCPY_HOST_TO_DEVICE: 应用缓冲区经暂存区复制到 VRAM。
 * @GPGPU_MEMCPY_DEVICE_TO_HOST: VRAM 经暂存区复制到应用缓冲区。
 */
typedef enum GpgpuMemcpyKind {
	GPGPU_MEMCPY_HOST_TO_DEVICE = GPGPU_COPY_H2D,
	GPGPU_MEMCPY_DEVICE_TO_HOST = GPGPU_COPY_D2H,
} GpgpuMemcpyKind;

/* 各调用同步执行，返回零或负 errno 错误码。 */
/* 建立同步运行时上下文。 */
int gpgpuInit(GpgpuContext **out, const char *device);
/* 销毁同步运行时上下文。 */
void gpgpuDestroy(GpgpuContext *ctx);
/* 线程安全地申请同步 VRAM。 */
int gpgpuMalloc(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes);
/* 线程安全地释放同步 VRAM。 */
int gpgpuFree(GpgpuContext *ctx, GpgpuDevicePtr address);
/* 在应用缓冲区与同步 VRAM 之间拷贝。 */
int gpgpuMemcpy(GpgpuContext *ctx, GpgpuDevicePtr device_address, void *host, size_t bytes,
		GpgpuMemcpyKind kind);
/* 装载 RV32 代码并登记 Kernel 对象。 */
int gpgpuLoadKernel(GpgpuContext *ctx, GpgpuKernel **out, const void *code, size_t bytes);
/* 卸载所属上下文的 Kernel。 */
int gpgpuUnloadKernel(GpgpuContext *ctx, GpgpuKernel *kernel);
/* Args 由小端 32 位字段紧密排列而成，a0 指向该参数块。 */
/* 同步上传参数并执行指定 Kernel。 */
int gpgpuLaunchKernel(GpgpuContext *ctx, GpgpuKernel *kernel, GpgpuDim3 grid, GpgpuDim3 block,
		      const void *args, size_t args_bytes, uint32_t shared_bytes);
/* 等待同一上下文的同步操作退出。 */
int gpgpuDeviceSynchronize(GpgpuContext *ctx);
/* 查询同步设备能力。 */
int gpgpuGetInfo(GpgpuContext *ctx, struct gpgpu_info *info);
/* 读取同步设备的中断统计。 */
int gpgpuGetStats(GpgpuContext *ctx, struct gpgpu_stats *stats);

#endif /* LIBGPGPU_H */
