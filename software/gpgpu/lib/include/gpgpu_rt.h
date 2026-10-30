/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU RT：建立在 KMT 之上的小运行时
 *
 * KMT 只封装驱动接口；把一个 RV32 Kernel 跑起来还需要上传代码、生成描述对象、
 * 建队列与事件、组 AQL 包、准备 Signal 并等待。本层把这些固定步骤收起来，
 * 应用只描述“哪段代码、什么参数、多大网格”：
 *
 *   gpgpu_rt_create()            分配暂存区、参数块、Ring、进度页与 Signal
 *   gpgpu_rt_kernel_load()       上传代码并生成 Kernel 描述对象（均在 VRAM）
 *   gpgpu_rt_buffer_alloc()      VRAM 数据区，可 upload/download
 *   gpgpu_rt_dispatch()          写参数、组包、提交并等待完成
 *   gpgpu_rt_destroy()           按顺序回收
 *
 * 地址空间在队列存活期间冻结，所以全部对象都要在第一次 dispatch 之前分配；
 * gpgpu_rt_dispatch() 在第一次调用时才创建队列。
 */
#ifndef GPGPU_RT_H
#define GPGPU_RT_H

#include "gpgpu/gpgpu_version.h"
#include "gpgpu_kmt.h"

/* 一次派发中参数块的最多 32 位参数个数。 */
#define GPGPU_RT_MAX_ARGS 16

typedef struct GpgpuRt GpgpuRt;

/**
 * struct GpgpuRtKernel - 一个已装载的 Kernel
 * @code: 代码所在 VRAM 对象（读 + 执行）。
 * @descriptor: 描述对象所在 VRAM 对象（只读）。
 */
typedef struct GpgpuRtKernel {
	GpgpuKmtMemory *code;
	GpgpuKmtMemory *descriptor;
} GpgpuRtKernel;

/**
 * struct GpgpuRtDim3 - 三维尺寸
 * @x: X 方向。
 * @y: Y 方向。
 * @z: Z 方向。
 */
typedef struct GpgpuRtDim3 {
	uint32_t x;
	uint32_t y;
	uint32_t z;
} GpgpuRtDim3;

/**
 * gpgpu_rt_create() - 建立运行时
 * @context: 已打开的 KMT 连接。
 * @staging_size: Host 暂存区字节数，须不小于任何一次上传或下载的长度。
 * @rt: 输出。
 */
GpgpuKmtStatus gpgpu_rt_create(GpgpuKmtContext *context, size_t staging_size, GpgpuRt **rt);

/* 按“队列 → 事件 → 内存”的顺序回收；队列未能确认卸载时返回错误并保留资源。 */
GpgpuKmtStatus gpgpu_rt_destroy(GpgpuRt *rt);

/* 上传 RV32 代码并生成描述对象；code_size 须为 4 的倍数。 */
GpgpuKmtStatus gpgpu_rt_kernel_load(GpgpuRt *rt, const void *code, size_t code_size,
				    uint32_t args_count, GpgpuRtKernel *kernel);

/* 分配一段可读写的 VRAM 数据区。 */
GpgpuKmtStatus gpgpu_rt_buffer_alloc(GpgpuRt *rt, size_t size, GpgpuKmtMemory **buffer);
/* 经暂存区把 Host 数据上传到 VRAM。 */
GpgpuKmtStatus gpgpu_rt_upload(GpgpuRt *rt, GpgpuKmtMemory *buffer, const void *data,
			       size_t size);
/* 经暂存区把 VRAM 数据下载到 Host。 */
GpgpuKmtStatus gpgpu_rt_download(GpgpuRt *rt, GpgpuKmtMemory *buffer, void *data, size_t size);

/**
 * gpgpu_rt_arg_address() - 把对象的设备地址转成 32 位参数
 * @buffer: 对象。
 * @arg: 输出；RV32 Kernel 以 32 位读取地址。
 *
 * Return: 地址超出 32 位时返回 GPGPU_KMT_INVALID_PARAMETER。
 */
GpgpuKmtStatus gpgpu_rt_arg_address(GpgpuKmtMemory *buffer, uint32_t *arg);

/**
 * gpgpu_rt_dispatch() - 提交一次派发并等待完成
 * @rt: 运行时。
 * @kernel: 已装载的 Kernel。
 * @args: 参数块内容，依次放在 a0 指向的位置；个数须与装载时一致。
 * @args_count: 参数个数，至多 GPGPU_RT_MAX_ARGS。
 * @grid: 各维工作组数。
 * @block: 各维工作组尺寸。
 * @timeout_ms: 等待上限。
 *
 * 维度数取最高的非 1 维。总线程数为 grid × block。
 */
GpgpuKmtStatus gpgpu_rt_dispatch(GpgpuRt *rt, const GpgpuRtKernel *kernel, const uint32_t *args,
				 uint32_t args_count, GpgpuRtDim3 grid, GpgpuRtDim3 block,
				 uint32_t timeout_ms);

#endif /* GPGPU_RT_H */
