// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：内存对象
 *
 * 一个内存对象 = 一个 GEM 对象 + （Host 对象的）CPU 映射 + 一个 GPU 绑定。
 * 三者在 AllocMemory 中依次建立，任一步失败都撤销前面的步骤；
 * FreeMemory 以相反顺序撤销：先解除 GPU 绑定，再解除 CPU 映射，最后关闭句柄。
 */
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kmt_internal.h"

/* 把 value 向上取整到 alignment 的倍数；alignment 为 2 的幂。 */
static uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

/* 与已绑定对象重叠时返回该对象，否则返回 NULL。调用者持 lock。 */
static KmtMemory *vm_find_overlap(GpgpuKmtContext *c, uint64_t va, uint64_t size)
{
	for (KmtMemory *m = c->memory; m; m = m->next)
		if (va < m->pub.gpu_address + m->pub.mapped_size && m->pub.gpu_address < va + size)
			return m;
	return NULL;
}

/* 在可分配区间中按首次适配选一个对齐的空闲 GPUVA；没有返回 0。调用者持 lock。 */
static uint64_t vm_choose_address(GpgpuKmtContext *c, uint64_t size, uint64_t alignment)
{
	alignment = alignment < KMT_PAGE_SIZE ? KMT_PAGE_SIZE : alignment;
	for (unsigned int i = 0; i < c->vm_info.range_count; i++) {
		const struct gpgpu_va_range *r = &c->vm_info.ranges[i];
		uint64_t va = align_up(r->start, alignment);
		KmtMemory *busy;

		while (va < r->end && size <= r->end - va) {
			busy = vm_find_overlap(c, va, size);
			if (!busy)
				return va;
			va = align_up(busy->pub.gpu_address + busy->pub.mapped_size, alignment);
		}
	}
	return 0;
}

/* 关闭 GEM 句柄；调用者持 lock。 */
static GpgpuKmtStatus gem_close(GpgpuKmtContext *c, uint32_t handle)
{
	struct gpgpu_gem_close args = { .handle = handle };

	return kmt_ioctl(c, GPGPU_DRM_GEM_CLOSE, &args);
}

/* 绑定失败的错误中，这些表示驱动可能已写入部分页表，相关资源会被保留。 */
static bool map_error_uncertain(int error)
{
	return error == EIO || error == ETIMEDOUT || error == ENOMEM;
}

/*
 * gpgpuKmtAllocMemory() - 创建并绑定一个整对象
 *
 * 一个内存对象由三部分组成：驱动中的 GEM 对象、Host 对象在本进程的 CPU
 * 映射，以及设备可用的 GPU 地址。依次建立：
 *
 * 1. GPGPU_DRM_BO_CREATE 创建 GEM 对象，驱动返回句柄、页对齐后的
 *    mapped_size 和 mmap 偏移；
 * 2. Host 对象用 mmap 建立 CPU 映射。VRAM 对象没有 CPU 映射，
 *    只能经 CopyMemory 访问；
 * 3. GPUVM 开启时由库在可分配区间中挑选对齐的 GPUVA（或使用 fixed_va），
 *    再用 GPGPU_DRM_MAP 让驱动写页表；关闭时驱动在 map.address 中返回
 *    GPU 物理地址。绑定失败且驱动可能已写入部分页表时标记 uncertain；
 * 4. 填写公开信息，挂到连接的 memory 链表。
 *
 * 第 2、3 步失败时解除 CPU 映射并关闭 GEM 句柄。整个过程持连接锁，
 * 保证并发分配不会选到同一个 GPUVA。
 */
GpgpuKmtStatus gpgpuKmtAllocMemory(GpgpuKmtContext *c, uint32_t kind, uint64_t size,
				   uint64_t alignment, uint32_t permissions, uint64_t fixed_va,
				   GpgpuKmtMemory **memory)
{
	struct gpgpu_bo_create create = {
		.version = GPGPU_DRM_VERSION,
		.kind = kind,
		.size = size,
		.alignment = alignment,
	};
	struct gpgpu_vm_map map = { .version = GPGPU_DRM_VERSION, .permissions = permissions };
	GpgpuKmtStatus status;
	KmtMemory *m;
	void *cpu = NULL;

	if (!memory || !size || !alignment || (alignment & (alignment - 1)))
		return GPGPU_KMT_INVALID_PARAMETER;
	*memory = NULL;
	m = calloc(1, sizeof(*m));
	if (!m)
		return GPGPU_KMT_NO_MEMORY;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS)
		goto free_m;
	pthread_mutex_lock(&c->lock);

	/* GPUVM 关闭时设备地址由驱动分配，不能指定 fixed_va。 */
	if (!c->gpuvm && fixed_va) {
		status = GPGPU_KMT_NOT_SUPPORTED;
		goto out;
	}

	/* 1. 创建 GEM 对象，取得句柄、mapped_size 和 mmap 偏移。 */
	status = kmt_ioctl(c, GPGPU_DRM_BO_CREATE, &create);
	if (status != GPGPU_KMT_SUCCESS)
		goto out;

	/* 2. 只有 Host 对象建立 CPU 映射。 */
	if (kind == GPGPU_MEMORY_HOST) {
		cpu = mmap(NULL, create.mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
			   create.mmap_offset);
		if (cpu == MAP_FAILED) {
			cpu = NULL;
			status = kmt_status(errno);
			goto undo;
		}
	}

	/*
	 * 3. GPUVM 开启时先选 GPUVA，再请驱动写页表；
	 *    关闭时 address 为 0，由驱动填回 GPU 物理地址。
	 */
	map.handle = create.handle;
	if (c->gpuvm) {
		map.address = fixed_va ? fixed_va :
					 vm_choose_address(c, create.mapped_size, alignment);
		if (!map.address) {
			status = GPGPU_KMT_OUT_OF_RESOURCES;
			goto undo;
		}
	}
	if (ioctl(c->fd, GPGPU_DRM_MAP, &map) < 0) {
		int error = errno;

		if (map_error_uncertain(error))
			c->uncertain = true;
		status = kmt_status(error);
		goto undo;
	}

	/* 4. 挂入链表后把 m 置 NULL，避免末尾的 free 释放它。 */
	m->pub = (GpgpuKmtMemory) {
		.kind = kind,
		.size = size,
		.mapped_size = create.mapped_size,
		.cpu = cpu,
		.gpu_address = map.address,
	};
	m->context = c;
	m->handle = create.handle;
	m->bound = true;
	m->next = c->memory;
	c->memory = m;
	*memory = &m->pub;
	m = NULL;
	goto out;
undo:
	if (cpu)
		munmap(cpu, create.mapped_size);
	/* 句柄关不掉时驱动仍保留对象，本连接不能再报告正常关闭。 */
	if (gem_close(c, create.handle) != GPGPU_KMT_SUCCESS)
		c->uncertain = true;
out:
	pthread_mutex_unlock(&c->lock);
	kmt_leave(c);
free_m:
	free(m);
	return status;
}

/*
 * gpgpuKmtFreeMemory() - 释放内存对象
 *
 * 连接中还有存活队列时地址空间冻结，返回 UNAVAILABLE；本对象被某条队列
 * 用作 Ring 或进度时同样拒绝。按与分配相反的顺序撤销：
 *
 * 1. GPGPU_DRM_UNMAP 解除 GPU 绑定，设备不能再经这个地址访问对象；
 * 2. 解除 CPU 映射；
 * 3. 关闭 GEM 句柄；
 * 4. 从连接链表摘除，释放库内对象。
 *
 * 任一步失败都返回错误并保留对象。已完成的步骤记在 bound 和 cpu 上，
 * 重试时不会重复执行。
 */
GpgpuKmtStatus gpgpuKmtFreeMemory(GpgpuKmtMemory *memory)
{
	KmtMemory *m = kmt_memory(memory);
	GpgpuKmtContext *c = m ? m->context : NULL;
	struct gpgpu_vm_map unmap = { .version = GPGPU_DRM_VERSION };
	GpgpuKmtStatus status;

	if (!c)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	pthread_mutex_lock(&c->lock);

	/* 队列存活期间地址空间冻结，任何对象都不能释放。 */
	if (m->queue_users || c->queues) {
		status = GPGPU_KMT_UNAVAILABLE;
		goto out;
	}

	/* 1. 解绑成功后清除 bound。 */
	if (m->bound) {
		unmap.address = m->pub.gpu_address;
		status = kmt_ioctl(c, GPGPU_DRM_UNMAP, &unmap);
		if (status != GPGPU_KMT_SUCCESS)
			goto out;
		m->bound = false;
	}

	/* 2. 解除映射成功后清空 cpu。 */
	if (m->pub.cpu) {
		if (munmap(m->pub.cpu, m->pub.mapped_size)) {
			status = kmt_status(errno);
			goto out;
		}
		m->pub.cpu = NULL;
	}

	/* 3. 关闭句柄，驱动放下本连接对 GEM 对象的引用。 */
	status = gem_close(c, m->handle);
	if (status != GPGPU_KMT_SUCCESS)
		goto out;

	/* 4. 从链表摘除并释放库内对象。 */
	KMT_UNLINK(&c->memory, m);
	free(m);
out:
	pthread_mutex_unlock(&c->lock);
	kmt_leave(c);
	return status;
}

/*
 * 请驱动在 Host 对象与 VRAM 对象之间同步复制，返回时复制已结束。
 * 两个对象须属于同一连接，各自的范围不超过请求长度。
 */
GpgpuKmtStatus gpgpuKmtCopyMemory(GpgpuKmtMemory *host, uint64_t host_offset,
				  GpgpuKmtMemory *vram, uint64_t vram_offset, uint64_t size,
				  uint32_t direction)
{
	KmtMemory *h = kmt_memory(host), *v = kmt_memory(vram);
	struct gpgpu_memory_copy args;

	if (!kmt_range_ok(h, host_offset, size) || !kmt_range_ok(v, vram_offset, size) ||
	    h->context != v->context || h->pub.kind != GPGPU_MEMORY_HOST ||
	    v->pub.kind != GPGPU_MEMORY_VRAM || direction > GPGPU_COPY_D2H)
		return GPGPU_KMT_INVALID_PARAMETER;

	args = (struct gpgpu_memory_copy) {
		.version = GPGPU_DRM_VERSION,
		.direction = direction,
		.host_handle = h->handle,
		.vram_handle = v->handle,
		.host_offset = host_offset,
		.vram_offset = vram_offset,
		.size = size,
	};
	return kmt_call(h->context, GPGPU_DRM_COPY, &args);
}
