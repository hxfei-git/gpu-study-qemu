/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/mman.h>
#include "internal.h"

/* 从能力区间选取首个空闲整对象范围，重用已经解除的 GPUVA。 */
static uint64_t vm_choose_address(GpgpuKmtContext *c, uint64_t size, uint64_t alignment)
{
	alignment = alignment < 4096 ? 4096 : alignment;
	for (unsigned int i = 0; i < c->vm_info.range_count; i++) {
		uint64_t address = (c->vm_info.ranges[i].start + alignment - 1) & ~(alignment - 1);

		while (address < c->vm_info.ranges[i].end &&
		       size <= c->vm_info.ranges[i].end - address) {
			GpgpuKmtMemory *m;

			for (m = c->memory; m; m = m->next)
				if (m->gpu_va && address < m->gpu_va + m->mapped_size &&
				    m->gpu_va < address + size)
					break;
			if (!m)
				return address;
			address = (m->gpu_va + m->mapped_size + alignment - 1) & ~(alignment - 1);
		}
	}
	return 0;
}

/* 分配存储后建立 CPU 映射和整对象 GPU 映射，全部成功才交付对象。 */
GpgpuKmtStatus gpgpuKmtAllocMemoryEx(GpgpuKmtContext *c, uint32_t kind, uint64_t size,
				     uint64_t alignment, uint32_t permissions, uint64_t fixed_va,
				     GpgpuKmtMemory *m)
{
	struct gpgpu_bo_create create = {
		.version = GPGPU_DRM_VERSION,
		.kind = kind,
		.size = size,
		.alignment = alignment,
	};
	struct gpgpu_vm_map map = { .version = GPGPU_DRM_VERSION, .permissions = permissions };
	struct gpgpu_gem_close release;
	GpgpuKmtStatus status;
	void *cpu = NULL;

	if (!m || !size || !alignment || (alignment & (alignment - 1)))
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(m, 0, sizeof(*m));
	status = gpgpu_kmt_begin(c);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (!c->gpuvm) {
		status = GPGPU_KMT_NOT_SUPPORTED;
		goto out;
	}
	if (ioctl(c->fd, GPGPU_DRM_BO_CREATE, &create) < 0) {
		status = gpgpu_kmt_error(errno);
		goto out;
	}
	release = (struct gpgpu_gem_close) { .handle = create.handle };
	if (kind == GPGPU_MEMORY_HOST) {
		cpu = mmap(NULL, create.mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
			   create.mmap_offset);
		if (cpu == MAP_FAILED) {
			cpu = NULL;
			status = gpgpu_kmt_error(errno);
			goto rollback;
		}
	}
	map.handle = create.handle;
	map.address = fixed_va ? fixed_va : vm_choose_address(c, create.mapped_size, alignment);
	if (!map.address) {
		status = GPGPU_KMT_OUT_OF_RESOURCES;
		goto rollback;
	}
	if (ioctl(c->fd, GPGPU_DRM_MAP, &map) < 0) {
		int error = errno;

		/* 写表或失效失败可能留下保留资源，最后关闭不能误报正常回收。 */
		if (error == EIO || error == ETIMEDOUT || error == ENOMEM)
			c->uncertain = true;
		status = gpgpu_kmt_error(error);
		goto rollback;
	}
	*m = (GpgpuKmtMemory) {
		.context = c,
		.handle = create.handle,
		.kind = kind,
		.size = size,
		.mapped_size = create.mapped_size,
		.gpu_va = map.address,
		.mmap_offset = create.mmap_offset,
		.cpu = cpu,
		.next = c->memory,
	};
	c->memory = m;
	c->objects++;
	goto out;
rollback:
	if (cpu)
		munmap(cpu, create.mapped_size);
	if (ioctl(c->fd, GPGPU_DRM_GEM_CLOSE, &release) < 0)
		c->uncertain = true;
out:
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	gpgpu_kmt_end(c);
	return status;
}

/* 先同步撤销 GPU 映射，成功后才解除 CPU 映射并关闭 GEM 句柄。 */
static GpgpuKmtStatus vm_free_memory(GpgpuKmtMemory *m)
{
	GpgpuKmtContext *c = m->context;
	struct gpgpu_vm_map map = { .version = GPGPU_DRM_VERSION, .address = m->gpu_va };
	struct gpgpu_gem_close release = { .handle = m->handle };
	GpgpuKmtStatus status = gpgpu_kmt_begin(c);
	GpgpuKmtMemory **link;

	if (status != GPGPU_KMT_SUCCESS)
		return status;
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (m->gpu_va && ioctl(c->fd, GPGPU_DRM_UNMAP, &map) < 0) {
		status = gpgpu_kmt_error(errno);
		goto out;
	}
	m->gpu_va = 0;
	if (m->cpu && munmap(m->cpu, m->mapped_size)) {
		status = gpgpu_kmt_error(errno);
		goto out;
	}
	m->cpu = NULL;
	if (ioctl(c->fd, GPGPU_DRM_GEM_CLOSE, &release) < 0) {
		status = gpgpu_kmt_error(errno);
		goto out;
	}
	for (link = &c->memory; *link && *link != m; link = &(*link)->next) {
	}
	if (*link)
		*link = m->next;
	c->objects--;
	memset(m, 0, sizeof(*m));
out:
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	gpgpu_kmt_end(c);
	return status;
}

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
	if (context->gpuvm) {
		status = gpgpuKmtAllocMemoryEx(context, kind, size, alignment,
					       GPGPU_VM_READ | GPGPU_VM_WRITE, 0, memory);
		gpgpu_kmt_end(context);
		return status;
	}
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
	if (context->gpuvm)
		return vm_free_memory(memory);
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
