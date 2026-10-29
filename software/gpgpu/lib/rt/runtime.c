// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU RT：Kernel 装载、数据上传与一次派发
 */
#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include "gpgpu_rt.h"

/* 运行时能登记的内存对象上限（Kernel、数据区与内部对象合计）。 */
#define RT_MAX_OBJECTS 32
/* 内部对象的对齐与 Ring 尺寸。 */
#define RT_PAGE 4096u
#define RT_RING_BYTES 4096u
/* 进度页中 R 与 W 的偏移。 */
#define RT_RPTR_OFFSET 0
#define RT_WPTR_OFFSET 8
/* 排空队列的等待上限，单位毫秒。 */
#define RT_DRAIN_TIMEOUT_MS 5000

/**
 * struct GpgpuRt - 运行时
 * @context: KMT 连接。
 * @objects: 登记的内存对象，destroy 时逆序释放。
 * @object_count: 登记数。
 * @staging: Host 暂存区，用于上传与下载。
 * @args: Host 参数块。
 * @ring: Host AQL Ring。
 * @progress: Host R/W 进度页。
 * @signal: Host Signal。
 * @queue: 第一次派发时创建。
 * @event: 与 queue 一起创建。
 */
struct GpgpuRt {
	GpgpuKmtContext *context;
	GpgpuKmtMemory *objects[RT_MAX_OBJECTS];
	unsigned int object_count;
	GpgpuKmtMemory *staging;
	GpgpuKmtMemory *args;
	GpgpuKmtMemory *ring;
	GpgpuKmtMemory *progress;
	GpgpuKmtMemory *signal;
	GpgpuKmtQueue *queue;
	GpgpuKmtEvent *event;
};

/* 分配并登记一个对象。 */
static GpgpuKmtStatus rt_alloc(GpgpuRt *rt, uint32_t kind, size_t size, uint32_t permissions,
			       GpgpuKmtMemory **memory)
{
	GpgpuKmtStatus status;

	if (rt->object_count == RT_MAX_OBJECTS)
		return GPGPU_KMT_OUT_OF_RESOURCES;
	status = gpgpuKmtAllocMemory(rt->context, kind, size, RT_PAGE, permissions, 0, memory);
	if (status == GPGPU_KMT_SUCCESS)
		rt->objects[rt->object_count++] = *memory;
	return status;
}

/* 1. 暂存区与参数块；2. Ring、进度与 Signal。队列推迟到第一次派发时创建。 */
GpgpuKmtStatus gpgpu_rt_create(GpgpuKmtContext *context, size_t staging_size, GpgpuRt **out)
{
	const uint32_t rw = GPGPU_VM_READ | GPGPU_VM_WRITE;
	GpgpuKmtStatus status;
	GpgpuRt *rt;

	if (!context || !staging_size || !out)
		return GPGPU_KMT_INVALID_PARAMETER;
	*out = NULL;
	rt = calloc(1, sizeof(*rt));
	if (!rt)
		return GPGPU_KMT_NO_MEMORY;
	rt->context = context;

	/* 1. 2. */
	status = rt_alloc(rt, GPGPU_MEMORY_HOST, staging_size, rw, &rt->staging);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, GPGPU_RT_MAX_ARGS * sizeof(uint32_t),
				  GPGPU_VM_READ, &rt->args);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, RT_RING_BYTES, rw, &rt->ring);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, RT_PAGE, rw, &rt->progress);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, sizeof(GpgpuSignal), rw, &rt->signal);
	if (status != GPGPU_KMT_SUCCESS) {
		gpgpu_rt_destroy(rt);
		return status;
	}
	*out = rt;
	return GPGPU_KMT_SUCCESS;
}

/*
 * 1. 排空并卸载队列（出错的队列直接卸载）；2. 销毁事件；3. 逆序释放对象。
 * 任一步失败立即返回，已释放的部分不再恢复，剩余资源由 KMT 的 Close 处理。
 */
GpgpuKmtStatus gpgpu_rt_destroy(GpgpuRt *rt)
{
	GpgpuKmtStatus status;

	if (!rt)
		return GPGPU_KMT_INVALID_PARAMETER;

	/* 1. 队列。 */
	if (rt->queue) {
		status = gpgpuKmtDrainQueue(rt->queue, RT_DRAIN_TIMEOUT_MS);
		if (status != GPGPU_KMT_SUCCESS && status != GPGPU_KMT_WAIT_FAILURE)
			return status;
		status = gpgpuKmtDestroyQueue(rt->queue);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->queue = NULL;
	}

	/* 2. 事件。 */
	if (rt->event) {
		status = gpgpuKmtDestroyEvent(rt->event);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->event = NULL;
	}

	/* 3. 对象。 */
	while (rt->object_count) {
		status = gpgpuKmtFreeMemory(rt->objects[rt->object_count - 1]);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->object_count--;
	}
	free(rt);
	return GPGPU_KMT_SUCCESS;
}

GpgpuKmtStatus gpgpu_rt_arg_address(GpgpuKmtMemory *buffer, uint32_t *arg)
{
	if (!buffer || !arg || buffer->gpu_address > UINT32_MAX)
		return GPGPU_KMT_INVALID_PARAMETER;
	*arg = (uint32_t)buffer->gpu_address;
	return GPGPU_KMT_SUCCESS;
}

GpgpuKmtStatus gpgpu_rt_upload(GpgpuRt *rt, GpgpuKmtMemory *buffer, const void *data,
			       size_t size)
{
	if (!rt || !data || size > rt->staging->size)
		return GPGPU_KMT_INVALID_PARAMETER;
	memcpy(rt->staging->cpu, data, size);
	return gpgpuKmtCopyMemory(rt->staging, 0, buffer, 0, size, GPGPU_COPY_H2D);
}

GpgpuKmtStatus gpgpu_rt_download(GpgpuRt *rt, GpgpuKmtMemory *buffer, void *data, size_t size)
{
	GpgpuKmtStatus status;

	if (!rt || !data || size > rt->staging->size)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = gpgpuKmtCopyMemory(rt->staging, 0, buffer, 0, size, GPGPU_COPY_D2H);
	if (status == GPGPU_KMT_SUCCESS)
		memcpy(data, rt->staging->cpu, size);
	return status;
}

GpgpuKmtStatus gpgpu_rt_buffer_alloc(GpgpuRt *rt, size_t size, GpgpuKmtMemory **buffer)
{
	if (!rt || !size || !buffer)
		return GPGPU_KMT_INVALID_PARAMETER;
	return rt_alloc(rt, GPGPU_MEMORY_VRAM, size, GPGPU_VM_READ | GPGPU_VM_WRITE, buffer);
}

/*
 * gpgpu_rt_kernel_load() - 装载 Kernel
 *
 * 1. 分配代码区（读 + 执行）与描述对象区（只读），都在 VRAM；
 * 2. 上传代码；
 * 3. 生成描述对象（入口为代码区的设备地址）并上传。
 */
GpgpuKmtStatus gpgpu_rt_kernel_load(GpgpuRt *rt, const void *code, size_t code_size,
				    uint32_t args_count, GpgpuRtKernel *kernel)
{
	GpgpuKernelDescriptor desc;
	GpgpuKmtStatus status;
	uint32_t entry;

	if (!rt || !code || !code_size || (code_size & 3) || !args_count ||
	    args_count > GPGPU_RT_MAX_ARGS || !kernel)
		return GPGPU_KMT_INVALID_PARAMETER;

	/* 1. 存储。 */
	status = rt_alloc(rt, GPGPU_MEMORY_VRAM, code_size, GPGPU_VM_READ | GPGPU_VM_EXEC,
			  &kernel->code);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_VRAM, sizeof(desc), GPGPU_VM_READ,
				  &kernel->descriptor);
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpu_rt_arg_address(kernel->code, &entry);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 2. 代码。 */
	status = gpgpu_rt_upload(rt, kernel->code, code, code_size);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 3. 描述对象。 */
	desc = (GpgpuKernelDescriptor) {
		.code_entry = htole32(entry),
		.code_size = htole32(code_size),
		.kernarg_size = htole32(args_count * sizeof(uint32_t)),
		.kernel_abi_version = htole32(GPGPU_KERNEL_ABI_VERSION),
	};
	return gpgpu_rt_upload(rt, kernel->descriptor, &desc, sizeof(desc));
}

/* 第一次派发时创建队列与事件；此后地址空间冻结。 */
static GpgpuKmtStatus rt_ensure_queue(GpgpuRt *rt)
{
	GpgpuKmtStatus status;

	if (rt->queue)
		return GPGPU_KMT_SUCCESS;
	status = gpgpuKmtCreateQueue(rt->context, rt->ring, 0, RT_RING_BYTES, rt->progress,
				     RT_RPTR_OFFSET, RT_WPTR_OFFSET, &rt->queue);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	return gpgpuKmtCreateEvent(rt->queue, &rt->event);
}

/* 维度数取最高的非 1 维。 */
static uint32_t rt_dimensions(GpgpuRtDim3 grid, GpgpuRtDim3 block)
{
	if (grid.z > 1 || block.z > 1)
		return 3;
	if (grid.y > 1 || block.y > 1)
		return 2;
	return 1;
}

static GpgpuAqlPacket rt_packet(const GpgpuRt *rt, const GpgpuRtKernel *kernel,
				GpgpuRtDim3 grid, GpgpuRtDim3 block)
{
	uint32_t header = GPGPU_AQL_HEADER | rt_dimensions(grid, block) << GPGPU_AQL_SETUP_SHIFT;

	return (GpgpuAqlPacket) {
		.full_header = htole32(header),
		.workgroup_size_x = htole16(block.x),
		.workgroup_size_y = htole16(block.y),
		.workgroup_size_z = htole16(block.z),
		.grid_size_x = htole32(grid.x * block.x),
		.grid_size_y = htole32(grid.y * block.y),
		.grid_size_z = htole32(grid.z * block.z),
		.kernel_object = htole64(kernel->descriptor->gpu_address),
		.kernarg_address = htole64(rt->args->gpu_address),
		.completion_signal = htole64(rt->signal->gpu_address),
	};
}

/*
 * gpgpu_rt_dispatch() - 一次派发
 *
 * 1. 确保队列与事件存在；
 * 2. 写参数块；
 * 3. 把 Signal 置为 1，并填入事件编号与通知槽地址；
 * 4. 组包、预留、发布；
 * 5. 等 Signal 归零。
 */
GpgpuKmtStatus gpgpu_rt_dispatch(GpgpuRt *rt, const GpgpuRtKernel *kernel, const uint32_t *args,
				 uint32_t args_count, GpgpuRtDim3 grid, GpgpuRtDim3 block,
				 uint32_t timeout_ms)
{
	uint32_t *block_args;
	GpgpuSignal *signal;
	GpgpuAqlPacket packet;
	GpgpuKmtStatus status;
	uint32_t number;

	if (!rt || !kernel || !args || !args_count || args_count > GPGPU_RT_MAX_ARGS)
		return GPGPU_KMT_INVALID_PARAMETER;

	/* 1. 队列。 */
	status = rt_ensure_queue(rt);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 2. 参数。 */
	block_args = rt->args->cpu;
	for (uint32_t i = 0; i < args_count; i++)
		block_args[i] = htole32(args[i]);

	/* 3. Signal。 */
	signal = rt->signal->cpu;
	*signal = (GpgpuSignal) {
		.value = htole64(1),
		.event_id = htole32(rt->event->id),
		.event_mailbox_ptr = htole64(rt->event->mailbox_address),
	};

	/* 4. 提交。 */
	packet = rt_packet(rt, kernel, grid, block);
	status = gpgpuKmtReserve(rt->queue, &number);
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtPublish(rt->queue, number, &packet);

	/* 5. 等待。 */
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtWaitSignal(signal, rt->event, timeout_ms);
	return status;
}
