/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/mman.h>
#include "internal.h"

/*
 * gpgpuKmtAllocMemory() - 申请 Host 或 VRAM 内存对象
 *
 * 通过 ioctl 取得句柄和地址；Host 对象再建立 CPU 映射，失败时回收申请。
 */
GpgpuKmtStatus gpgpuKmtAllocMemory(GpgpuKmtContext *context, uint32_t kind, uint64_t size,
				   uint64_t alignment, GpgpuKmtMemory *memory)
{
	struct gpgpu_memory_request request = {
		.version = GPGPU_QABI_VERSION,
		.kind = kind,
		.size = size,
		.alignment = alignment,
	};
	GpgpuKmtStatus status;
	void *mapping = NULL;

	if (!memory || !size || !alignment || (alignment & (alignment - 1)))
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(memory, 0, sizeof(*memory));
	status = gpgpu_kmt_begin(context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	if (ioctl(context->fd, GPGPU_IOC_MEMORY_ALLOC, &request) < 0) {
		status = gpgpu_kmt_error(errno);
		goto done;
	}
	if (kind == GPGPU_MEMORY_HOST) {
		mapping = mmap(NULL, request.size, PROT_READ | PROT_WRITE, MAP_SHARED, context->fd,
			       request.mmap_offset);
		if (mapping == MAP_FAILED) {
			struct gpgpu_object_request release = {
				.version = GPGPU_QABI_VERSION,
				.handle = request.handle,
			};

			status = gpgpu_kmt_error(errno);
			ioctl(context->fd, GPGPU_IOC_MEMORY_FREE, &release);
			goto done;
		}
	}
	*memory = (GpgpuKmtMemory) {
		.context = context,
		.handle = request.handle,
		.kind = request.kind,
		.size = request.size,
		.vram_offset = request.vram_offset,
		.host_address = request.host_address,
		.mmap_offset = request.mmap_offset,
		.cpu = mapping,
	};
	gpgpu_kmt_objects(context, 1);
done:
	gpgpu_kmt_end(context);
	return status;
}

/*
 * gpgpuKmtFreeMemory() - 释放不再使用的内存对象
 *
 * 先检查队列引用并解除映射；驱动拒绝释放时尝试恢复 CPU 映射。
 */
GpgpuKmtStatus gpgpuKmtFreeMemory(GpgpuKmtMemory *memory)
{
	GpgpuKmtContext *context;
	struct gpgpu_object_request request;
	GpgpuKmtStatus status;
	bool mapped;

	if (!memory || !memory->context)
		return GPGPU_KMT_INVALID_PARAMETER;
	context = memory->context;
	status = gpgpu_kmt_begin(context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (memory->queue_users) {
		pthread_mutex_unlock(&gpgpu_kmt_lock);
		gpgpu_kmt_end(context);
		return GPGPU_KMT_UNAVAILABLE;
	}
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	mapped = memory->cpu != NULL;
	if (mapped && munmap(memory->cpu, memory->size)) {
		status = gpgpu_kmt_error(errno);
		goto done;
	}
	memory->cpu = NULL;
	request = (struct gpgpu_object_request) {
		.version = GPGPU_QABI_VERSION,
		.handle = memory->handle,
	};
	if (ioctl(context->fd, GPGPU_IOC_MEMORY_FREE, &request) < 0) {
		status = gpgpu_kmt_error(errno);
		/* 外部映射等引用阻止回收时，恢复本库可用的映射。 */
		if (mapped) {
			void *mapping = mmap(NULL, memory->size, PROT_READ | PROT_WRITE, MAP_SHARED,
					     context->fd, memory->mmap_offset);

			if (mapping != MAP_FAILED)
				memory->cpu = mapping;
		}
		goto done;
	}
	gpgpu_kmt_objects(context, -1);
	memset(memory, 0, sizeof(*memory));
done:
	gpgpu_kmt_end(context);
	return status;
}

/*
 * gpgpuKmtCopyMemory() - 在 Host 与 VRAM 对象间同步拷贝
 *
 * 校验种类、归属和对象内范围，使用驱动 DMA 接口等待完成。
 */
GpgpuKmtStatus gpgpuKmtCopyMemory(GpgpuKmtMemory *host, uint64_t host_offset, GpgpuKmtMemory *vram,
				  uint64_t vram_offset, uint64_t size, uint32_t direction)
{
	struct gpgpu_memory_copy request;

	if (!gpgpu_kmt_range(host, host_offset, size) ||
	    !gpgpu_kmt_range(vram, vram_offset, size) || host->context != vram->context ||
	    host->kind != GPGPU_MEMORY_HOST || vram->kind != GPGPU_MEMORY_VRAM ||
	    direction > GPGPU_COPY_D2H) {
		return GPGPU_KMT_INVALID_PARAMETER;
	}
	request = (struct gpgpu_memory_copy) {
		.version = GPGPU_QABI_VERSION,
		.direction = direction,
		.host_handle = host->handle,
		.vram_handle = vram->handle,
		.host_offset = host_offset,
		.vram_offset = vram_offset,
		.size = size,
	};
	return gpgpu_kmt_ioctl(host->context, GPGPU_IOC_MEMORY_COPY, &request);
}
