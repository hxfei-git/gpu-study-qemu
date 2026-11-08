// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU RT：Kernel 装载、数据上传与一次派发
 */
#include <endian.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "gpgpu_rt.h"

/* 运行时能登记的内存对象上限（Kernel、数据区与内部对象合计）。 */
#define RT_MAX_OBJECTS 32
/* 待回收旧事件的上限；正常情况下下一次派发前就能回收。 */
#define RT_MAX_PENDING_EVENTS 4
/* 内部对象的对齐与 Ring 尺寸。 */
#define RT_PAGE 4096u
#define RT_RING_BYTES 4096u
/* 进度页中 R 与 W 的偏移。 */
#define RT_RPTR_OFFSET 0
#define RT_WPTR_OFFSET 8
/* 排空队列的等待上限，单位毫秒。 */
#define RT_DRAIN_TIMEOUT_MS 5000

/**
 * struct rt_object - 运行时登记的对象记录
 * @memory: 内存对象指针。
 * @can_free: 是否可由 buffer_free 单独释放（buffer_alloc 分配的数据区）。
 */
struct rt_object {
	GpgpuKmtMemory *memory;
	bool can_free;
};

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
 * @event: 本次派发的事件；派发失败时保留到 destroy。
 * @pending_events: 计算已完成、驱动尚未允许销毁的旧事件。
 * @pending_count: 待回收事件数。
 * @failed: 派发已发布但结果未确认，不能再改写参数块与 Signal。
 */
struct GpgpuRt {
	GpgpuKmtContext *context;
	struct rt_object objects[RT_MAX_OBJECTS];
	unsigned int object_count;
	GpgpuKmtMemory *staging;
	GpgpuKmtMemory *args;
	GpgpuKmtMemory *ring;
	GpgpuKmtMemory *progress;
	GpgpuKmtMemory *signal;
	GpgpuKmtQueue *queue;
	GpgpuKmtEvent *event;
	GpgpuKmtEvent *pending_events[RT_MAX_PENDING_EVENTS];
	unsigned int pending_count;
	bool failed;
};

/*
 * 分配并登记一个对象，默认不可由 buffer_free 单独释放；
 * flags 见 gpgpuKmtAllocMemory()。
 */
static GpgpuKmtStatus rt_alloc(GpgpuRt *rt, uint32_t kind, uint32_t flags, size_t size,
			       uint32_t permissions, GpgpuKmtMemory **memory)
{
	GpgpuKmtStatus status;

	if (rt->object_count == RT_MAX_OBJECTS)
		return GPGPU_KMT_OUT_OF_RESOURCES;
	status = gpgpuKmtAllocMemory(rt->context, kind, flags, size, RT_PAGE, permissions, 0,
				     memory);
	if (status == GPGPU_KMT_SUCCESS)
		rt->objects[rt->object_count++] = (struct rt_object) { .memory = *memory };
	return status;
}

/*
 * gpgpu_rt_create() - 分配运行时内部的 Host 对象
 *
 * 1. 暂存区（上传、下载时与 VRAM 之间中转）和参数块（设备只读）。两者
 *    可缓存：暂存区只经 COPY 访问，由设备维护；参数块每次派发前失效；
 * 2. AQL Ring（4 KiB，64 个槽）、R/W 进度页和一个 Signal。CP 持续轮询
 *    它们，三者都不可缓存。
 *
 * 它们都是 Host 对象，CPU 可以直接写。队列推迟到第一次派发时创建。
 * 任一对象分配失败时，释放已分配的部分并返回错误。
 */
GpgpuKmtStatus gpgpu_rt_create(GpgpuKmtContext *context, size_t staging_size, GpgpuRt **out)
{
	const uint32_t rw = GPGPU_VM_READ | GPGPU_VM_WRITE;
	const uint32_t uc = GPGPU_BO_UNCACHED;
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
	status = rt_alloc(rt, GPGPU_MEMORY_HOST, 0, staging_size, rw, &rt->staging);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, 0, GPGPU_RT_MAX_ARGS * sizeof(uint64_t),
				  GPGPU_VM_READ, &rt->args);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, uc, RT_RING_BYTES, rw, &rt->ring);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, uc, RT_PAGE, rw, &rt->progress);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_HOST, uc, sizeof(GpgpuSignal), rw, &rt->signal);
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
 * 2. 再销毁本次和待回收的事件。驱动要求来源队列已卸载或事件已通知；
 * 3. 最后按分配的逆序释放全部对象。Ring 与进度对象要等队列卸载后才能释放。
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

	while (rt->pending_count) {
		status = gpgpuKmtDestroyEvent(rt->pending_events[rt->pending_count - 1]);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->pending_count--;
	}

	/* 3. 每释放一个就减少计数，失败后重试不会重复释放。 */
	while (rt->object_count) {
		status = gpgpuKmtFreeMemory(rt->objects[rt->object_count - 1].memory);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		rt->object_count--;
	}
	free(rt);
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
	GpgpuKmtStatus status;

	if (!rt || !size || !buffer)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = rt_alloc(rt, GPGPU_MEMORY_VRAM, 0, size, GPGPU_VM_READ | GPGPU_VM_WRITE, buffer);
	if (status == GPGPU_KMT_SUCCESS)
		rt->objects[rt->object_count - 1].can_free = true;
	return status;
}

/*
 * gpgpu_rt_kernel_load() - 装载 Kernel
 *
 * CP 经 AQL 包的 kernel_object 找到描述对象，再按其中的入口和长度让 RV64
 * 计算执行器取指，所以代码和描述对象都放在 VRAM：
 *
 * 1. 分配代码区（读 + 执行）和描述对象区（只读），代码区的 64 位设备
 *    地址就是入口；
 * 2. 经暂存区把代码上传到代码区；
 * 3. 填写描述对象：入口、代码长度、参数块长度（args_count 个 8 字节槽）
 *    和 ABI 版本，再上传。
 *
 * 失败时已分配的对象仍登记在 rt 中，随 gpgpu_rt_destroy() 释放。
 */
GpgpuKmtStatus gpgpu_rt_kernel_load(GpgpuRt *rt, const void *code, size_t code_size,
				    uint32_t args_count, GpgpuRtKernel *kernel)
{
	GpgpuKernelDescriptor desc;
	GpgpuKmtStatus status;

	if (!rt || !code || !code_size || (code_size & 3) || !args_count ||
	    args_count > GPGPU_RT_MAX_ARGS || !kernel)
		return GPGPU_KMT_INVALID_PARAMETER;

	/* 1. 代码区可执行，描述对象只读。 */
	status = rt_alloc(rt, GPGPU_MEMORY_VRAM, 0, code_size, GPGPU_VM_READ | GPGPU_VM_EXEC,
			  &kernel->code);
	if (status == GPGPU_KMT_SUCCESS)
		status = rt_alloc(rt, GPGPU_MEMORY_VRAM, 0, sizeof(desc), GPGPU_VM_READ,
				  &kernel->descriptor);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 2. 上传 RV64 机器码。 */
	status = gpgpu_rt_upload(rt, kernel->code, code, code_size);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 3. 多字节字段按小端写入，与设备端读取方式一致。 */
	desc = (GpgpuKernelDescriptor) {
		.code_entry = htole64(kernel->code->gpu_address),
		.code_size = htole32(code_size),
		.kernarg_size = htole32(args_count * sizeof(uint64_t)),
		.kernel_abi_version = htole32(GPGPU_KERNEL_ABI_VERSION),
	};
	return gpgpu_rt_upload(rt, kernel->descriptor, &desc, sizeof(desc));
}

/*
 * 第一次派发时创建队列。事件由每次派发单独创建，不在这里处理。
 */
static GpgpuKmtStatus rt_ensure_queue(GpgpuRt *rt)
{
	if (rt->queue)
		return GPGPU_KMT_SUCCESS;
	return gpgpuKmtCreateQueue(rt->context, rt->ring, 0, RT_RING_BYTES, rt->progress,
				   RT_RPTR_OFFSET, RT_WPTR_OFFSET, &rt->queue);
}

/*
 * 销毁已登记的旧事件。Signal 归零早于驱动消费 IH，旧事件可能仍是 PENDING，
 * 销毁返回忙时保留，下次派发前再试，不忙等。
 */
static void rt_reclaim_events(GpgpuRt *rt)
{
	unsigned int kept = 0;

	for (unsigned int i = 0; i < rt->pending_count; i++)
		if (gpgpuKmtDestroyEvent(rt->pending_events[i]) != GPGPU_KMT_SUCCESS)
			rt->pending_events[kept++] = rt->pending_events[i];
	rt->pending_count = kept;
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
 * 驱动的事件是一次性的：通知后保持 NOTIFIED，不会回到 PENDING。所以每次
 * 派发都用新事件，旧事件只等待回收。运行时只有一个参数块和一个 Signal，
 * 要等本次完成才返回。
 *
 * 1. 确保队列存在，回收旧事件；待回收事件已满时返回 OUT_OF_RESOURCES；
 * 2. 把参数按小端写进参数块，每个参数一个 8 字节槽。RV64 执行器把
 *    参数块地址放进 a0，Kernel 用普通 LD 读取。参数块可缓存，上一次派发
 *    读过的行可能还留在 dcache 中，所以写完后失效这段范围；
 * 3. 创建本次事件。第 2、3 步失败时还没有发布任何包，可以重试；
 * 4. 把 Signal 置为 1，填入本次事件的编号与通知槽地址。CP 完成后把值减为 0，
 *    再写通知槽，驱动随后唤醒事件等待者；
 * 5. 组包，经 Reserve 取号并等到空槽，再由 Publish 发布 Header、敲 Doorbell；
 * 6. 等 Signal 归零，期限为 timeout_ms。CP 发布 Signal 前已回写 dcache，
 *    Kernel 写出的数据此时对 CPU 可见；
 * 7. 成功后销毁本次事件，返回忙时登记待回收。
 *
 * 第 5、6 步失败时 CP 可能仍在读写参数块和 Signal，运行时标记为失败并保留
 * 本次事件，此后只能 gpgpu_rt_destroy()。
 */
GpgpuKmtStatus gpgpu_rt_dispatch(GpgpuRt *rt, const GpgpuRtKernel *kernel, const uint64_t *args,
				 uint32_t args_count, GpgpuRtDim3 grid, GpgpuRtDim3 block,
				 uint32_t timeout_ms)
{
	uint64_t *block_args;
	GpgpuSignal *signal;
	GpgpuAqlPacket packet;
	GpgpuKmtStatus status;
	uint32_t number;

	if (!rt || !kernel || !args || !args_count || args_count > GPGPU_RT_MAX_ARGS)
		return GPGPU_KMT_INVALID_PARAMETER;
	if (rt->failed)
		return GPGPU_KMT_UNAVAILABLE;

	/* 1. 只有第一次派发会真正创建队列。 */
	status = rt_ensure_queue(rt);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	rt_reclaim_events(rt);
	if (rt->pending_count == RT_MAX_PENDING_EVENTS)
		return GPGPU_KMT_OUT_OF_RESOURCES;

	/*
	 * 2. 参数块是可缓存的 Host 对象，CPU 直接写；失效 dcache 中的
	 *    旧副本后，执行器才会从内存读到新参数。写入顺序由 Publish
	 *    中的屏障保证。
	 */
	block_args = rt->args->cpu;
	for (uint32_t i = 0; i < args_count; i++)
		block_args[i] = htole64(args[i]);
	status = gpgpuKmtSyncMemory(rt->args, 0, args_count * sizeof(uint64_t),
				    GPGPU_CACHE_INVALIDATE);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 3. 事件绑定来源队列，所以在队列之后创建。 */
	status = gpgpuKmtCreateEvent(rt->queue, &rt->event);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/*
	 * 4. 必须在发布包之前写好 Signal：
	 *    CP 取到包后随时可能完成并递减它。
	 */
	signal = rt->signal->cpu;
	*signal = (GpgpuSignal) {
		.value = htole64(1),
		.event_id = htole32(rt->event->id),
		.event_mailbox_ptr = htole64(rt->event->mailbox_address),
	};

	/* 5. Reserve 失败时编号已被占用，队列不能再用。 */
	packet = rt_packet(rt, kernel, grid, block);
	status = gpgpuKmtReserve(rt->queue, &number);
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtPublish(rt->queue, number, &packet);

	/* 6. 先忙等约 200 μs，再阻塞等待本次事件。 */
	if (status == GPGPU_KMT_SUCCESS)
		status = gpgpuKmtWaitSignal(signal, rt->event, timeout_ms);
	if (status != GPGPU_KMT_SUCCESS) {
		rt->failed = true;
		return status;
	}

	/* 7. 计算已经成功，销毁失败只推迟回收，不改变返回值。 */
	if (gpgpuKmtDestroyEvent(rt->event) != GPGPU_KMT_SUCCESS)
		rt->pending_events[rt->pending_count++] = rt->event;
	rt->event = NULL;
	return GPGPU_KMT_SUCCESS;
}

/*
 * gpgpu_rt_buffer_free() - 释放 gpgpu_rt_buffer_alloc() 分配的数据区
 *
 * 队列可以仍然驻留，但调用者要保证已经没有任务使用 buffer。只接受本
 * 运行时登记、且由 buffer_alloc 分配的对象；内部对象、Kernel 代码和描述
 * 对象都拒绝。释放失败时保留登记，可以重试；成功后后续登记前移一位，
 * gpgpu_rt_destroy() 仍按原顺序逆序释放其余对象。
 */
GpgpuKmtStatus gpgpu_rt_buffer_free(GpgpuRt *rt, GpgpuKmtMemory *buffer)
{
	GpgpuKmtStatus status;
	unsigned int i;

	if (!rt || !buffer)
		return GPGPU_KMT_INVALID_PARAMETER;
	for (i = 0; i < rt->object_count && rt->objects[i].memory != buffer; i++)
		;
	if (i == rt->object_count || !rt->objects[i].can_free)
		return GPGPU_KMT_INVALID_PARAMETER;

	status = gpgpuKmtFreeMemory(buffer);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	memmove(&rt->objects[i], &rt->objects[i + 1],
		(rt->object_count - i - 1) * sizeof(rt->objects[0]));
	rt->object_count--;
	return GPGPU_KMT_SUCCESS;
}
