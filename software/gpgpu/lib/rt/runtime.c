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

/*
 * gpgpu_rt_create() - 分配运行时内部的 Host 对象
 *
 * 1. 暂存区（上传、下载时与 VRAM 之间中转）和参数块（设备只读）；
 * 2. AQL Ring（4 KiB，64 个槽）、R/W 进度页和一个 Signal。
 *
 * 它们都是 Host 对象，CPU 可以直接写。队列推迟到第一次派发时创建：
 * 队列存活期间地址空间冻结，调用者要在那之前分配完 Kernel 和数据区。
 * 任一对象分配失败时，释放已分配的部分并返回错误。
 */
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

	/*
	 * 1. 暂存区和参数块；2. Ring、进度页和 Signal。
	 *    某次分配失败后跳过后面的分配，统一在下方回收。
	 */
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
 * gpgpu_rt_destroy() - 回收运行时
 *
 * 1. 先排空再卸载队列，卸载前 CP 必须处理完所有包。队列已出错时排空
 *    返回 WAIT_FAILURE，照常卸载；
 * 2. 再销毁事件。驱动要求来源队列已卸载或事件已通知；
 * 3. 最后按分配的逆序释放全部对象。队列卸载后地址空间才解冻。
 *
 * 任一步失败立即返回并保留 rt，已释放的部分不再恢复。调用者可以
 * 重试，剩余资源也会在 KMT 最后一次 Close 时回收。
 */
GpgpuKmtStatus gpgpu_rt_destroy(GpgpuRt *rt)
{
	GpgpuKmtStatus status;

	if (!rt)
		return GPGPU_KMT_INVALID_PARAMETER;

	/* 1. 排空超时等其他错误直接返回，队列和对象都保留。 */
	if (rt->queue) {
		status = gpgpuKmtDrainQueue(rt->queue, RT_DRAIN_TIMEOUT_MS);
		if (status != GPGPU_KMT_SUCCESS && status != GPGPU_KMT_WAIT_FAILURE)
			return status;
		status = gpgpuKmtDestroyQueue(rt->queue);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->queue = NULL;
	}

	/* 2. 队列已卸载，事件不会再收到通知，可以销毁。 */
	if (rt->event) {
		status = gpgpuKmtDestroyEvent(rt->event);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->event = NULL;
	}

	/* 3. 每释放一个就减少计数，失败后重试不会重复释放。 */
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
 * CP 经 AQL 包的 kernel_object 找到描述对象，再按其中的入口和长度让 RV32
 * 计算执行器取指，所以代码和描述对象都放在 VRAM：
 *
 * 1. 分配代码区（读 + 执行）和描述对象区（只读），取代码区的 32 位设备
 *    地址作为入口；
 * 2. 经暂存区把代码上传到代码区；
 * 3. 填写描述对象：入口、代码长度、参数块长度（args_count 个 32 位参数）
 *    和 ABI 版本，再上传。
 *
 * 失败时已分配的对象仍登记在 rt 中，随 gpgpu_rt_destroy() 释放。
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

	/* 1. 代码区可执行，描述对象只读；入口地址必须能放进 32 位。 */
	status = rt_alloc(rt, GPGPU_MEMORY_VRAM, code_size, GPGPU_VM_READ | GPGPU_VM_EXEC,
			  &kernel->code);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_VRAM, sizeof(desc), GPGPU_VM_READ,
				  &kernel->descriptor);
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpu_rt_arg_address(kernel->code, &entry);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 2. 上传 RV32 机器码。 */
	status = gpgpu_rt_upload(rt, kernel->code, code, code_size);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 3. 多字节字段按小端写入，与设备端读取方式一致。 */
	desc = (GpgpuKernelDescriptor) {
		.code_entry = htole32(entry),
		.code_size = htole32(code_size),
		.kernarg_size = htole32(args_count * sizeof(uint32_t)),
		.kernel_abi_version = htole32(GPGPU_KERNEL_ABI_VERSION),
	};
	return gpgpu_rt_upload(rt, kernel->descriptor, &desc, sizeof(desc));
}

/*
 * 第一次派发时创建队列，再为它创建事件（事件必须绑定来源队列）。
 * 此后地址空间冻结，不能再分配或释放内存。
 */
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

/*
 * 组一个 Kernel Dispatch 包。AQL 的 grid_size 是各维总线程数，
 * 所以用工作组数乘每组尺寸；Header 由 Publish 最后写入。
 */
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
 * gpgpu_rt_dispatch() - 同步执行一次派发
 *
 * 运行时只有一个参数块和一个 Signal，每次派发都复用，所以要等本次
 * 完成才返回。超时返回时 CP 可能仍在使用它们。
 *
 * 1. 确保队列与事件存在；
 * 2. 把参数按小端写进参数块。RV32 执行器把参数块地址放进 a0，
 *    Kernel 用普通 LW 读取；
 * 3. 把 Signal 置为 1，填入事件编号与通知槽地址。CP 完成后把值减为 0，
 *    再写通知槽，驱动随后唤醒事件等待者；
 * 4. 组包，经 Reserve 取号并等到空槽，再由 Publish 发布 Header、敲 Doorbell；
 * 5. 等 Signal 归零，期限为 timeout_ms。
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

	/* 1. 只有第一次派发会真正创建。 */
	status = rt_ensure_queue(rt);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/*
	 * 2. 参数块是 Host 对象，CPU 直接写；
	 *    可见性由 Publish 中的屏障保证。
	 */
	block_args = rt->args->cpu;
	for (uint32_t i = 0; i < args_count; i++)
		block_args[i] = htole32(args[i]);

	/*
	 * 3. 必须在发布包之前写好 Signal：
	 *    CP 取到包后随时可能完成并递减它。
	 */
	signal = rt->signal->cpu;
	*signal = (GpgpuSignal) {
		.value = htole64(1),
		.event_id = htole32(rt->event->id),
		.event_mailbox_ptr = htole64(rt->event->mailbox_address),
	};

	/* 4. Reserve 失败时编号已被占用，队列不能再用，直接返回错误。 */
	packet = rt_packet(rt, kernel, grid, block);
	status = gpgpuKmtReserve(rt->queue, &number);
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtPublish(rt->queue, number, &packet);

	/* 5. 先忙等约 200 μs，再阻塞等待事件。 */
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtWaitSignal(signal, rt->event, timeout_ms);
	return status;
}
