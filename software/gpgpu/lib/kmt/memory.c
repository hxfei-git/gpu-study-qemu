// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：内存对象
 *
 * 一个内存对象 = 一个 GEM 对象 + 同一地址上的 CPU 映射与 GPU 绑定。
 * GPUVM 开启时 GPUVA 等于对象在本进程中的 CPU 地址：库先申请 CPU 地址，
 * 把对象映射到这里，再用同一数值绑定 GPU。Host 对象的这段地址可读写，
 * VRAM 对象只占住地址，内容经 CopyMemory 访问。
 *
 * AllocMemory 依次建立，任一步失败都撤销前面的步骤；FreeMemory 以相反
 * 顺序撤销：先解除 GPU 绑定，再解除 CPU 映射，最后关闭句柄。GPU 绑定
 * 解除之前不释放 CPU 地址，否则这段地址可能被 CPU 分给别的映射，而 GPU
 * 页表里仍指向旧对象。
 */
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "kmt_internal.h"

/* 占位映射的标志：私有匿名、不占交换空间，与 PROT_NONE 一起使用。 */
#define KMT_RESERVE_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)

/* alignment 必须是 2 的幂；加法不溢出时输出向上取整的结果。 */
static bool align_up_ok(uint64_t value, uint64_t alignment, uint64_t *aligned)
{
	if (!alignment || (alignment & (alignment - 1)) ||
	    value > UINT64_MAX - (alignment - 1))
		return false;

	*aligned = (value + alignment - 1) & ~(alignment - 1);
	return true;
}

/*
 * kmt_reserve_va() - 申请一段 CPU 虚拟地址，供对象映射并用作 GPUVA
 * @size: 页对齐的长度。
 * @alignment: 起点对齐，2 的幂，不小于 KMT_PAGE_SIZE。
 * @fixed_va: 非零时要求恰好使用这一地址。
 *
 * 照 libhsakmt 的做法先占住 CPU 地址，再把对象映射到这里：占位映射为
 * PROT_NONE，不能访问。fixed_va 为 0 时多申请 alignment - 一页的余量，
 * 裁掉头尾得到对齐的 [va, va + size)；非零时用 MAP_FIXED_NOREPLACE，
 * 这段地址已有映射则失败，不覆盖已有内容。
 *
 * 例：size = 0x3000、alignment = 0x1_0000，预留 0x1_2000 字节：
 *
 *   原区间    [0x4000_3000, 0x4001_5000)
 *   对齐区间  [0x4001_0000, 0x4001_3000)
 *   裁去头部  0xd000 字节；裁去尾部 0x2000 字节。
 *
 * 1. 检查长度、对齐与地址宽度，防止 mmap 参数被截断；
 * 2. 固定地址时检查完整范围，再用 MAP_FIXED_NOREPLACE 占位；
 * 3. 自动选址时检查预留余量与起点取整，裁掉头尾多余的页。
 *
 * Return: 起始地址；失败返回 NULL，errno 为原因。
 */
static uint8_t *kmt_reserve_va(uint64_t size, uint64_t alignment, uint64_t fixed_va)
{
	uint64_t padded, aligned;
	size_t head, tail;
	uint8_t *base, *va;

	/* 1. size 与 alignment 最终都要作为 size_t 或 uintptr_t 使用。 */
	if (!size || (size & (KMT_PAGE_SIZE - 1)) || size > SIZE_MAX ||
	    alignment < KMT_PAGE_SIZE || alignment > SIZE_MAX || (alignment & (alignment - 1)))
		goto invalid;

	/* 2. 固定地址不需要对齐余量，且不能覆盖已有映射。 */
	if (fixed_va) {
		if (fixed_va > UINTPTR_MAX || (fixed_va & (alignment - 1)) ||
		    size > UINTPTR_MAX - fixed_va)
			goto invalid;
		base = mmap((void *)(uintptr_t)fixed_va, size, PROT_NONE,
			    KMT_RESERVE_FLAGS | MAP_FIXED_NOREPLACE, -1, 0);
		if (base == MAP_FAILED)
			return NULL;
		/* 旧内核不认识 MAP_FIXED_NOREPLACE，只把地址当提示。 */
		if (base != (uint8_t *)(uintptr_t)fixed_va) {
			munmap(base, size);
			errno = EEXIST;
			return NULL;
		}
		return base;
	}

	/* 3. 预留长度与对齐后的地址都先用减法检查，再进行加法。 */
	if (size > SIZE_MAX - (alignment - KMT_PAGE_SIZE))
		goto invalid;
	padded = size + (alignment - KMT_PAGE_SIZE);
	base = mmap(NULL, padded, PROT_NONE, KMT_RESERVE_FLAGS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	if (!align_up_ok((uintptr_t)base, alignment, &aligned) || aligned > UINTPTR_MAX ||
	    size > UINTPTR_MAX - aligned) {
		munmap(base, padded);
		goto invalid;
	}
	va = (uint8_t *)(uintptr_t)aligned;
	head = aligned - (uintptr_t)base;
	tail = padded - head - size;
	if (head)
		munmap(base, head);
	if (tail)
		munmap(va + size, tail);
	return va;

invalid:
	errno = EINVAL;
	return NULL;
}

/* 关闭 GEM 句柄；调用者持 lock。 */
static GpgpuKmtStatus gem_close(GpgpuKmtContext *c, uint32_t handle)
{
	struct gpgpu_gem_close args = { .handle = handle };

	return kmt_ioctl(c, GPGPU_DRM_GEM_CLOSE, &args);
}

/*
 * 绑定失败的错误中，这些表示驱动已写页表但失效未确认，相关资源
 * 会被保留。ENOSPC、ENOMEM 来自绑定前的表页分配，对象未绑定，
 * 可以直接撤销。
 */
static bool map_error_uncertain(int error)
{
	return error == EIO || error == ETIMEDOUT;
}

/*
 * kmt_memory_create() - 创建并绑定一个整对象，填写 m，不挂入连接链表
 *
 * 调用者持 lock，依次建立：
 *
 * 1. kmt_reserve_va() 申请一段对齐的 CPU 地址 va，先以 PROT_NONE 占住；
 * 2. GPGPU_DRM_BO_CREATE 创建 GEM 对象，驱动返回句柄、页对齐后的
 *    mapped_size 和 mmap 偏移；flags 带 GPGPU_BO_EVENT_PAGE 时驱动同时把它
 *    登记为会话的通知页，带它或 GPGPU_BO_UNCACHED 时对象按不可缓存绑定；
 * 3. 用 MAP_FIXED 把对象映射到 va，替换占位：Host 对象可读写；VRAM 对象
 *    只能是 PROT_NONE，CPU 访问这段地址会触发段错误；
 * 4. GPGPU_DRM_MAP 绑定 GPU：GPUVM 开启时 address 就是 va，GPUVA 与 CPU
 *    地址相同；关闭时 address 为 0，驱动填回 GPU 物理地址。驱动已写页表
 *    但失效未确认时标记 uncertain；
 * 5. 填写 m 的公开信息与句柄。
 *
 * 第 2～4 步失败时解除 va 处的映射并关闭 GEM 句柄。绑定结果未知时页表
 * 可能已指向 va，这段地址换回 PROT_NONE 占位，不归还给进程。
 */
GpgpuKmtStatus kmt_memory_create(GpgpuKmtContext *c, uint32_t kind, uint32_t flags, uint64_t size,
				 uint64_t alignment, uint32_t permissions, uint64_t fixed_va,
				 KmtMemory *m)
{
	struct gpgpu_bo_create create = {
		.version = GPGPU_DRM_VERSION,
		.kind = kind,
		.size = size,
		.alignment = alignment,
		.flags = flags,
	};
	struct gpgpu_vm_map map = { .version = GPGPU_DRM_VERSION, .permissions = permissions };
	int prot = kind == GPGPU_MEMORY_HOST ? PROT_READ | PROT_WRITE : PROT_NONE;
	uint64_t length;
	GpgpuKmtStatus status;
	bool keep_va = false;
	uint8_t *va;

	/* 1. 页取整不能回绕或截断；先占住 CPU 地址，GPUVA 取同一数值。 */
	if ((kind != GPGPU_MEMORY_HOST && kind != GPGPU_MEMORY_VRAM) || !size || !alignment ||
	    (alignment & (alignment - 1)) || !align_up_ok(size, KMT_PAGE_SIZE, &length) ||
	    length > SIZE_MAX)
		return GPGPU_KMT_INVALID_PARAMETER;
	va = kmt_reserve_va(length, alignment > KMT_PAGE_SIZE ? alignment : KMT_PAGE_SIZE,
			    fixed_va);
	if (!va)
		return kmt_status(errno);

	/* 2. 创建 GEM 对象；页对齐长度须等于 length，才能整段替换占位。 */
	status = kmt_ioctl(c, GPGPU_DRM_BO_CREATE, &create);
	if (status != GPGPU_KMT_SUCCESS) {
		munmap(va, length);
		return status;
	}
	if (create.mapped_size != length) {
		status = GPGPU_KMT_ERROR;
		goto undo;
	}

	/* 3. 对象映射到 va，MAP_FIXED 替换占位映射。 */
	if (mmap(va, length, prot, MAP_SHARED | MAP_FIXED, c->fd, create.mmap_offset) ==
	    MAP_FAILED) {
		status = kmt_status(errno);
		goto undo;
	}

	/* 4. GPUVM 开启时 GPUVA 就是 va；关闭时由驱动填回 GPU 物理地址。 */
	map.handle = create.handle;
	if (c->gpuvm)
		map.address = (uintptr_t)va;
	if (ioctl(c->fd, GPGPU_DRM_MAP, &map) < 0) {
		int error = errno;

		if (map_error_uncertain(error)) {
			c->uncertain = true;
			keep_va = true;
		}
		status = kmt_status(error);
		goto undo;
	}

	/* 5. 填写公开信息；VRAM 对象的 CPU 地址不可访问，不公开。 */
	m->pub = (GpgpuKmtMemory) {
		.kind = kind,
		.size = size,
		.mapped_size = length,
		.cpu = kind == GPGPU_MEMORY_HOST ? va : NULL,
		.gpu_address = map.address,
	};
	m->context = c;
	m->handle = create.handle;
	m->map = va;
	m->bound = true;
	return GPGPU_KMT_SUCCESS;

undo:
	if (keep_va)
		mmap(va, length, PROT_NONE, KMT_RESERVE_FLAGS | MAP_FIXED, -1, 0);
	else
		munmap(va, length);
	/* 句柄关不掉时驱动仍保留对象，本连接不能再报告正常关闭。 */
	if (gem_close(c, create.handle) != GPGPU_KMT_SUCCESS)
		c->uncertain = true;
	return status;
}

/*
 * 创建并绑定一个整对象，成功后挂到连接的 memory 链表。通知页由 Open
 * 单独创建，这里只接受 GPGPU_BO_UNCACHED。
 */
GpgpuKmtStatus gpgpuKmtAllocMemory(GpgpuKmtContext *c, uint32_t kind, uint32_t flags,
				   uint64_t size, uint64_t alignment, uint32_t permissions,
				   uint64_t fixed_va, GpgpuKmtMemory **memory)
{
	GpgpuKmtStatus status;
	KmtMemory *m;

	if (!memory || !size || !alignment || (alignment & (alignment - 1)) ||
	    (flags & ~GPGPU_BO_UNCACHED))
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

	status = kmt_memory_create(c, kind, flags, size, alignment, permissions, fixed_va, m);
	if (status == GPGPU_KMT_SUCCESS) {
		/* 挂入链表后把 m 置 NULL，避免末尾的 free 释放它。 */
		m->next = c->memory;
		c->memory = m;
		*memory = &m->pub;
		m = NULL;
	}
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
 * 本对象被某条队列用作 Ring 或进度时返回 UNAVAILABLE；其他对象在队列存活
 * 期间也可以释放，调用者要先确认没有任务还在使用它。按与分配相反的顺序撤销：
 *
 * 1. GPGPU_DRM_UNMAP 解除 GPU 绑定，设备不能再经这个地址访问对象；
 * 2. 解除 CPU 映射，归还这段 CPU 地址；第 1 步失败时不执行，地址继续
 *    被对象占住；
 * 3. 关闭 GEM 句柄；
 * 4. 从连接链表摘除，释放库内对象。
 *
 * 任一步失败都返回错误并保留对象。已完成的步骤记在 bound 和 map 上，
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

	/* Ring 与进度对象要等所属队列卸载后才能释放。 */
	if (m->queue_users) {
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

	/* 2. 解除映射成功后清空 map 与 cpu。 */
	if (m->map) {
		if (munmap(m->map, m->pub.mapped_size)) {
			status = kmt_status(errno);
			goto out;
		}
		m->map = NULL;
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

/* 请驱动按对象范围回写或失效 dcache，返回时设备已完成维护。 */
GpgpuKmtStatus gpgpuKmtSyncMemory(GpgpuKmtMemory *memory, uint64_t offset, uint64_t size,
				  uint32_t op)
{
	KmtMemory *m = kmt_memory(memory);
	struct gpgpu_cache_sync args;

	if (!kmt_range_ok(m, offset, size))
		return GPGPU_KMT_INVALID_PARAMETER;

	args = (struct gpgpu_cache_sync) {
		.version = GPGPU_DRM_VERSION,
		.handle = m->handle,
		.offset = offset,
		.size = size,
		.op = op,
	};
	return kmt_call(m->context, GPGPU_DRM_CACHE_SYNC, &args);
}
