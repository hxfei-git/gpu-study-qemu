/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU RT：建立在 KMT 之上的小运行时
 *
 * KMT 只封装驱动接口；把一个 RV64 Kernel 跑起来还需要上传代码、生成描述
 * 对象、建队列与事件、组 AQL 包、准备 Signal 并等待。本层把这些固定步骤
 * 收起来，应用只描述“哪段代码、什么参数、多大网格”：
 *
 *   gpgpu_rt_create()            分配暂存区、参数块、Ring、进度页与 Signal
 *   gpgpu_rt_kernel_load()       上传代码并生成 Kernel 描述对象（均在 VRAM）
 *   gpgpu_rt_buffer_alloc()      VRAM 数据区，可 upload/download
 *   gpgpu_rt_buffer_free()       释放不再使用的数据区
 *   gpgpu_rt_dispatch()          写参数、组包、提交并等待完成
 *   gpgpu_rt_destroy()           按顺序回收
 *
 * gpgpu_rt_dispatch() 在第一次调用时才创建队列，每次派发使用新事件。
 * 队列存活期间仍可分配、释放数据区；释放前须确认已经没有任务使用它。
 * 同一运行时的各个调用由调用者串行执行。
 */
#ifndef GPGPU_RT_H
#define GPGPU_RT_H

#include "gpgpu/gpgpu_version.h"
#include "gpgpu_kmt.h"

/* 一次派发中参数块的最多参数个数；每个参数占一个 8 字节槽。 */
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
 * @staging_size: 非零 Host 暂存区字节数；上传、下载按此容量分块。
 * @rt: 输出。
 */
GpgpuKmtStatus gpgpu_rt_create(GpgpuKmtContext *context, size_t staging_size, GpgpuRt **rt);

/* 按“队列 → 事件 → 内存”的顺序回收；队列未能确认卸载时返回错误并保留资源。 */
GpgpuKmtStatus gpgpu_rt_destroy(GpgpuRt *rt);

/*
 * 上传 RV64 代码并生成描述对象；code_size 须为 4 的倍数，参数块长度为
 * args_count 个 8 字节槽。
 */
GpgpuKmtStatus gpgpu_rt_kernel_load(GpgpuRt *rt, const void *code, size_t code_size,
				    uint32_t args_count, GpgpuRtKernel *kernel);

/* 分配一段可读写的 VRAM 数据区。 */
GpgpuKmtStatus gpgpu_rt_buffer_alloc(GpgpuRt *rt, size_t size, GpgpuKmtMemory **buffer);
/*
 * 释放由本运行时 buffer_alloc 分配的数据区；内部对象、Kernel 代码和描述对象
 * 返回 INVALID_PARAMETER。不排空队列，调用者须确认已没有任务使用 buffer。
 */
GpgpuKmtStatus gpgpu_rt_buffer_free(GpgpuRt *rt, GpgpuKmtMemory *buffer);
/**
 * gpgpu_rt_upload_range() - 经固定大小的暂存区分块上传到 VRAM
 * @rt: 运行时；调用者串行使用，暂存区大小由 create 指定。
 * @buffer: 与运行时属于同一 KMT 连接的 VRAM 对象。
 * @offset: 对象内的起始字节偏移。
 * @data: 至少 size 字节的 Host 源数据。
 * @size: 非零字节数，完整范围不得超过 buffer 的请求长度。
 *
 * 1. 写暂存区前校验完整范围；派发或 COPY 结果未确认时拒绝继续上传；
 * 2. 每块先写暂存区，再同步 COPY 到 offset 加上已完成长度的位置，成功后
 *    才复用暂存区。调用者须保证没有其他任务访问目标范围。
 *
 * 任一块失败立即返回，已完成的块不回退，失败块可能只完成一部分。
 * COPY 超时或设备错误后禁止复用本运行时，只能尝试 destroy 回收。
 *
 * Return: SUCCESS、参数非法时的 INVALID_PARAMETER、运行时不可用时的
 * UNAVAILABLE，或 COPY 返回的错误。
 */
GpgpuKmtStatus gpgpu_rt_upload_range(GpgpuRt *rt, GpgpuKmtMemory *buffer, uint64_t offset,
				     const void *data, size_t size);
/**
 * gpgpu_rt_download_range() - 经固定大小的暂存区分块下载到 Host
 * @rt: 运行时；调用者串行使用，暂存区大小由 create 指定。
 * @buffer: 与运行时属于同一 KMT 连接的 VRAM 对象。
 * @offset: 对象内的起始字节偏移。
 * @data: 至少 size 字节的 Host 目标缓冲区。
 * @size: 非零字节数，完整范围不得超过 buffer 的请求长度。
 *
 * 1. 校验完整范围，避免部分下载；派发或 COPY 结果未确认时拒绝操作；
 * 2. 从 offset 加上已完成长度的位置同步 COPY，每块确认成功后才写入 Host
 *    目标。调用者须保证没有其他任务改写源范围。
 *
 * 任一块失败立即返回。Host 保留已完成的块，失败块及后续内容不变。
 * COPY 超时或设备错误后禁止复用本运行时，只能尝试 destroy 回收。
 *
 * Return: SUCCESS、参数非法时的 INVALID_PARAMETER、运行时不可用时的
 * UNAVAILABLE，或 COPY 返回的错误。
 */
GpgpuKmtStatus gpgpu_rt_download_range(GpgpuRt *rt, GpgpuKmtMemory *buffer, uint64_t offset,
				       void *data, size_t size);
/* 从对象偏移 0 上传，等同于 upload_range。 */
GpgpuKmtStatus gpgpu_rt_upload(GpgpuRt *rt, GpgpuKmtMemory *buffer, const void *data,
			       size_t size);
/* 从对象偏移 0 下载，等同于 download_range。 */
GpgpuKmtStatus gpgpu_rt_download(GpgpuRt *rt, GpgpuKmtMemory *buffer, void *data, size_t size);

/**
 * gpgpu_rt_dispatch() - 提交一次派发并等待完成
 * @rt: 运行时。
 * @kernel: 已装载的 Kernel。
 * @args: 参数块内容，每个 8 字节，依次放在 a0 指向的位置；个数须与
 *        装载时一致。数组参数直接填对象的 gpu_address。
 * @args_count: 参数个数，至多 GPGPU_RT_MAX_ARGS。
 * @grid: 各维工作组数。
 * @block: 各维工作组尺寸。
 * @timeout_ms: 等待上限。
 *
 * 维度数取最高的非 1 维。总线程数为 grid × block。
 */
GpgpuKmtStatus gpgpu_rt_dispatch(GpgpuRt *rt, const GpgpuRtKernel *kernel, const uint64_t *args,
				 uint32_t args_count, GpgpuRtDim3 grid, GpgpuRtDim3 block,
				 uint32_t timeout_ms);

#endif /* GPGPU_RT_H */
