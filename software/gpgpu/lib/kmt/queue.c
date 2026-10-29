// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：用户队列
 *
 * 创建：1. 检查 Ring 与进度对象；2. QUEUE_CREATE；3. 映射 Doorbell 页；
 * 4. 登记。第 3 步失败时立即卸载队列；卸载结果未知时保留全部引用。
 */
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kmt_internal.h"

static bool queue_args_valid(GpgpuKmtContext *c, KmtMemory *ring, uint64_t ring_offset,
			     uint32_t ring_size, KmtMemory *progress, uint64_t rptr_offset,
			     uint64_t wptr_offset)
{
	return ring && progress && ring->context == c && progress->context == c &&
		ring->pub.cpu && progress->pub.cpu && kmt_range_ok(ring, ring_offset, ring_size) &&
		kmt_range_ok(progress, rptr_offset, 4) && kmt_range_ok(progress, wptr_offset, 4) &&
		!(ring_offset & 63) && !(rptr_offset & 7) && !(wptr_offset & 7) &&
		rptr_offset != wptr_offset && ring_size >= GPGPU_QUEUE_RING_MIN &&
		ring_size <= GPGPU_QUEUE_RING_MAX && !(ring_size & (ring_size - 1));
}

/* 驱动返回的 Doorbell 入口必须是映射页内 8 字节对齐的位置。 */
static bool doorbell_valid(const struct gpgpu_queue_create *r)
{
	return r->doorbell_map_size >= 8 && r->doorbell_offset <= r->doorbell_map_size - 8 &&
		!(r->doorbell_offset & 7);
}

static GpgpuKmtStatus queue_unload(GpgpuKmtQueue *q)
{
	struct gpgpu_queue_state args = { .version = GPGPU_DRM_VERSION, .queue_id = q->id };

	return kmt_ioctl(q->context, GPGPU_DRM_QUEUE_DESTROY, &args);
}

static void queue_register(GpgpuKmtContext *c, GpgpuKmtQueue *q)
{
	pthread_mutex_lock(&c->lock);
	q->ring_memory->queue_users++;
	q->progress_memory->queue_users++;
	q->next = c->queues;
	c->queues = q;
	pthread_mutex_unlock(&c->lock);
}

GpgpuKmtStatus gpgpuKmtCreateQueue(GpgpuKmtContext *c, GpgpuKmtMemory *ring_memory,
				   uint64_t ring_offset, uint32_t ring_size,
				   GpgpuKmtMemory *progress_memory, uint64_t rptr_offset,
				   uint64_t wptr_offset, GpgpuKmtQueue **queue)
{
	KmtMemory *ring = kmt_memory(ring_memory), *progress = kmt_memory(progress_memory);
	struct gpgpu_queue_create args;
	GpgpuKmtStatus status;
	GpgpuKmtQueue *q;
	void *page;

	/* 1. 参数。 */
	if (!queue || !queue_args_valid(c, ring, ring_offset, ring_size, progress, rptr_offset,
					wptr_offset))
		return GPGPU_KMT_INVALID_PARAMETER;
	*queue = NULL;
	q = calloc(1, sizeof(*q));
	if (!q)
		return GPGPU_KMT_NO_MEMORY;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS)
		goto free_q;

	/* 2. 创建。 */
	args = (struct gpgpu_queue_create) {
		.version = GPGPU_DRM_VERSION,
		.ring_handle = ring->handle,
		.progress_handle = progress->handle,
		.ring_size = ring_size,
		.ring_offset = ring_offset,
		.rptr_offset = rptr_offset,
		.wptr_offset = wptr_offset,
	};
	if (ioctl(c->fd, GPGPU_DRM_QUEUE_CREATE, &args) < 0) {
		int error = errno;

		/* 映射请求已发出但结果未知：驱动保留队列，本连接不能再正常关闭。 */
		if (error == EPROTO || error == EIO || error == ETIMEDOUT)
			kmt_mark_uncertain(c);
		status = kmt_status(error);
		goto leave;
	}
	*q = (GpgpuKmtQueue) {
		.context = c,
		.id = args.queue_id,
		.ring_memory = ring,
		.progress_memory = progress,
		.ring = (GpgpuAqlPacket *)((char *)ring->pub.cpu + ring_offset),
		.slots = ring_size / sizeof(GpgpuAqlPacket),
		.rptr = (uint32_t *)((char *)progress->pub.cpu + rptr_offset),
		.wptr = (uint32_t *)((char *)progress->pub.cpu + wptr_offset),
		.doorbell_page_size = args.doorbell_map_size,
	};

	/* 3. Doorbell。 */
	page = doorbell_valid(&args) ? mmap(NULL, args.doorbell_map_size, PROT_READ | PROT_WRITE,
					    MAP_SHARED, c->fd, args.doorbell_mmap_offset) :
				       MAP_FAILED;
	if (page == MAP_FAILED) {
		status = doorbell_valid(&args) ? kmt_status(errno) : GPGPU_KMT_ERROR;
		if (queue_unload(q) != GPGPU_KMT_SUCCESS) {
			/* 驱动仍持有队列：保留引用，Close 将返回 UNAVAILABLE。 */
			queue_register(c, q);
			kmt_mark_uncertain(c);
			q = NULL;
		}
		goto leave;
	}
	q->doorbell_page = page;
	q->doorbell = (volatile uint64_t *)((char *)page + args.doorbell_offset);

	/* 4. 登记。 */
	queue_register(c, q);
	*queue = q;
	q = NULL;
leave:
	kmt_leave(c);
free_q:
	free(q);
	return status;
}

GpgpuKmtStatus gpgpuKmtQueryQueue(GpgpuKmtQueue *q, struct gpgpu_queue_state *state)
{
	if (!q || !state)
		return GPGPU_KMT_INVALID_PARAMETER;
	*state = (struct gpgpu_queue_state) { .version = GPGPU_DRM_VERSION, .queue_id = q->id };
	return kmt_call(q->context, GPGPU_DRM_QUEUE_QUERY, state);
}

GpgpuKmtStatus gpgpuKmtGetVmFault(GpgpuKmtQueue *q, struct gpgpu_vm_fault *fault)
{
	if (!q || !fault)
		return GPGPU_KMT_INVALID_PARAMETER;
	*fault = (struct gpgpu_vm_fault) { .version = GPGPU_DRM_VERSION, .queue_id = q->id };
	return kmt_call(q->context, GPGPU_DRM_FAULT, fault);
}

/* 驱动确认卸载后，才解除 Doorbell 映射、放下内存引用并释放对象。 */
GpgpuKmtStatus gpgpuKmtDestroyQueue(GpgpuKmtQueue *q)
{
	GpgpuKmtContext *c = q ? q->context : NULL;
	GpgpuKmtStatus status;

	if (!c)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = queue_unload(q);
	if (status == GPGPU_KMT_SUCCESS) {
		if (q->doorbell_page)
			munmap(q->doorbell_page, q->doorbell_page_size);
		pthread_mutex_lock(&c->lock);
		q->ring_memory->queue_users--;
		q->progress_memory->queue_users--;
		KMT_UNLINK(&c->queues, q);
		pthread_mutex_unlock(&c->lock);
		free(q);
	}
	kmt_leave(c);
	return status;
}
