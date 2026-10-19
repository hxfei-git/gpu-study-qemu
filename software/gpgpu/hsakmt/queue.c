/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/mman.h>
#include "internal.h"

/*
 * gpgpuKmtCreateQueue() - 用已有 Ring 和进度对象建立 AQL 队列
 *
 * 驱动完成映射后保留内存引用并映射 Doorbell；
 * 回滚未确认时继续保留引用。
 */
GpgpuKmtStatus gpgpuKmtCreateQueue(GpgpuKmtContext *context, GpgpuKmtMemory *ring,
				   uint64_t ring_offset, uint32_t ring_size,
				   GpgpuKmtMemory *progress, uint64_t rptr_offset,
				   uint64_t wptr_offset, GpgpuKmtQueue *queue)
{
	struct gpgpu_queue_create request;
	GpgpuKmtStatus status;
	void *mapping;

	if (!queue || !gpgpu_kmt_range(ring, ring_offset, ring_size) ||
	    !gpgpu_kmt_range(progress, rptr_offset, 4) ||
	    !gpgpu_kmt_range(progress, wptr_offset, 4) || ring->context != context ||
	    progress->context != context || ring->kind != GPGPU_MEMORY_HOST ||
	    progress->kind != GPGPU_MEMORY_HOST || !ring->cpu || !progress->cpu ||
	    (ring_offset & 63) || (rptr_offset & 7) || (wptr_offset & 7) ||
	    rptr_offset == wptr_offset || ring_size < GPGPU_QUEUE_RING_MIN ||
	    ring_size > GPGPU_QUEUE_RING_MAX || (ring_size & (ring_size - 1))) {
		return GPGPU_KMT_INVALID_PARAMETER;
	}
	memset(queue, 0, sizeof(*queue));
	request = (struct gpgpu_queue_create) {
		.version = GPGPU_QABI_VERSION,
		.ring_handle = ring->handle,
		.progress_handle = progress->handle,
		.ring_offset = ring_offset,
		.ring_size = ring_size,
		.rptr_offset = rptr_offset,
		.wptr_offset = wptr_offset,
	};
	status = gpgpu_kmt_begin(context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	if (gpgpu_kmt_raw_ioctl(context, GPGPU_IOC_QUEUE_CREATE, &request) < 0) {
		if (context->gpuvm && (errno == EPROTO || errno == EIO || errno == ETIMEDOUT))
			context->uncertain = true;
		status = gpgpu_kmt_error(errno);
		goto done;
	}
	*queue = (GpgpuKmtQueue) {
		.context = context,
		.id = request.queue_id,
		.ring_size = ring_size,
		.ring_memory = ring,
		.progress_memory = progress,
		.ring = (void *)((char *)ring->cpu + ring_offset),
		.rptr = (void *)((char *)progress->cpu + rptr_offset),
		.wptr = (void *)((char *)progress->cpu + wptr_offset),
		.doorbell_map_size = request.doorbell_map_size,
	};
	pthread_mutex_lock(&gpgpu_kmt_lock);
	ring->queue_users++;
	progress->queue_users++;
	context->objects++;
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	if (request.doorbell_map_size < 8 ||
	    request.doorbell_offset > request.doorbell_map_size - 8 ||
	    (request.doorbell_offset & 7)) {
		status = GPGPU_KMT_ERROR;
		goto rollback;
	}
	mapping = mmap(NULL, request.doorbell_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       context->fd, request.doorbell_mmap_offset);
	if (mapping == MAP_FAILED) {
		status = gpgpu_kmt_error(errno);
		goto rollback;
	}
	queue->doorbell_mapping = mapping;
	queue->doorbell = (void *)((char *)mapping + request.doorbell_offset);
	if (context->gpuvm) {
		pthread_mutex_lock(&gpgpu_kmt_lock);
		queue->next = context->queues;
		context->queues = queue;
		pthread_mutex_unlock(&gpgpu_kmt_lock);
	}
	goto done;
rollback:
	/*
	 * 映射失败时尝试正常卸载，失败不交付可提交的队列标识。
	 * 无法确认卸载时保留连接对象计数和两个内存引用。
	 * 关闭、释放继续返回暂不可用；进程退出后由驱动保留这些资源。
	 */
	if (gpgpuKmtDestroyQueue(queue) != GPGPU_KMT_SUCCESS)
		memset(queue, 0, sizeof(*queue));
done:
	gpgpu_kmt_end(context);
	return status;
}

/*
 * gpgpuKmtQueryQueue() - 查询队列状态和最近管理结果
 *
 * 按会话内队列编号调用驱动，不改变共享 R/W。
 */
GpgpuKmtStatus gpgpuKmtQueryQueue(GpgpuKmtQueue *queue, struct gpgpu_queue_state *state)
{
	if (!queue || !queue->context || !state)
		return GPGPU_KMT_INVALID_PARAMETER;
	*state = (struct gpgpu_queue_state) {
		.version = GPGPU_QABI_VERSION,
		.queue_id = queue->id,
	};
	return gpgpu_kmt_ioctl(queue->context, GPGPU_IOC_QUEUE_QUERY, state);
}

/*
 * gpgpuKmtDestroyQueue() - 卸载队列并释放用户态映射
 *
 * 调用方先停止生产并排空；驱动确认成功后解除 Doorbell 映射和内存引用。
 */
GpgpuKmtStatus gpgpuKmtDestroyQueue(GpgpuKmtQueue *queue)
{
	struct gpgpu_queue_state request;
	GpgpuKmtContext *context;
	GpgpuKmtStatus status;

	if (!queue || !queue->context)
		return GPGPU_KMT_INVALID_PARAMETER;
	context = queue->context;
	status = gpgpu_kmt_begin(context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	request = (struct gpgpu_queue_state) {
		.version = GPGPU_QABI_VERSION,
		.queue_id = queue->id,
	};
	if (gpgpu_kmt_raw_ioctl(context, GPGPU_IOC_QUEUE_DESTROY, &request) < 0) {
		status = gpgpu_kmt_error(errno);
		goto done;
	}
	if (queue->doorbell_mapping)
		munmap(queue->doorbell_mapping, queue->doorbell_map_size);
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (context->gpuvm) {
		GpgpuKmtQueue **link;

		for (link = &context->queues; *link && *link != queue; link = &(*link)->next) {
		}
		if (*link)
			*link = queue->next;
	}
	queue->ring_memory->queue_users--;
	queue->progress_memory->queue_users--;
	context->objects--;
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	memset(queue, 0, sizeof(*queue));
done:
	gpgpu_kmt_end(context);
	return status;
}
