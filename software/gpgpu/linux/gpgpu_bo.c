// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：GEM 对象与 COPY
 *
 * 每个对象的存储在创建时固定（Host 一致性内存或连续 VRAM），寿命由三类
 * 引用共同决定：
 *
 *   会话列表引用 ── 会话结束或句柄关闭且未绑定时放下
 *   GEM 句柄引用 ── 由 GEM 核心管理
 *   GPU 绑定引用 ── gpgpu_vm_map() 取得，gpgpu_vm_unmap() 放下
 *   CPU mmap 引用 ── GEM 核心随 VMA 管理
 *
 * 最后一个引用放下时 bo_free() 释放存储并放下设备引用。
 */
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include "gpgpu_drv.h"

static struct gpgpu_bo *to_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct gpgpu_bo, gem);
}

/* 释放存储；调用者持 dev->lock。 */
static void bo_storage_free(struct gpgpu_bo *bo)
{
	if (bo->kind == GPGPU_MEMORY_HOST)
		gpgpu_dma_buf_free(bo->dev, &bo->host);
	else
		gpgpu_vram_free(bo->dev, &bo->vram_blocks);
}

/* GEM 最后一个引用：释放存储，再放下 DRM 与设备引用。 */
static void bo_free(struct drm_gem_object *obj)
{
	struct gpgpu_bo *bo = to_bo(obj);
	struct gpgpu_device *dev = bo->dev;
	struct drm_device *drm = obj->dev;

	mutex_lock(&dev->lock);
	drm_gem_object_release(obj);
	bo_storage_free(bo);
	mutex_unlock(&dev->lock);
	kfree(bo);
	drm_dev_put(drm);
	gpgpu_device_put(dev);
}

/**
 * gpgpu_bo_drop() - 放下会话列表引用
 * @bo: 对象；调用者持 dev->lock，函数内会暂时释放。
 *
 * 放下引用可能触发 bo_free()，它需要 dev->lock，因此先解锁。其余引用
 * （句柄、绑定、mmap）继续保护存储。
 */
void gpgpu_bo_drop(struct gpgpu_bo *bo)
{
	struct gpgpu_device *dev = bo->dev;

	list_del_init(&bo->node);
	bo->fpriv = NULL;
	mutex_unlock(&dev->lock);
	drm_gem_object_put(&bo->gem);
	mutex_lock(&dev->lock);
}

/* 句柄关闭：未绑定且未被使用的对象同时放下列表引用；已绑定的由绑定继续保活。 */
static void bo_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct gpgpu_bo *bo = to_bo(obj);
	struct gpgpu_device *dev = bo->dev;

	mutex_lock(&dev->lock);
	bo->handle = 0;
	if (bo->fpriv && !bo->bound && !bo->users)
		gpgpu_bo_drop(bo);
	mutex_unlock(&dev->lock);
}

/* 不支持共享：拒绝 DMA-BUF 导出，避免 GEM 核心的默认回退。 */
static struct dma_buf *bo_export(struct drm_gem_object *obj, int flags)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct vm_operations_struct bo_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* 只允许整页、共享、不可执行地映射 Host 对象；VRAM 对象经 COPY 访问。 */
static int bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct gpgpu_bo *bo = to_bo(obj);
	int ret;

	if (bo->kind != GPGPU_MEMORY_HOST || vma->vm_end - vma->vm_start != bo->mapped_size ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	vma->vm_pgoff = 0;
	vm_flags_clear(vma, VM_PFNMAP | VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	ret = dma_mmap_coherent(&bo->dev->pdev->dev, vma, bo->host.cpu, bo->host.dma,
				bo->host.size);
	if (!ret)
		vma->vm_ops = &bo_vm_ops;
	return ret;
}

static const struct drm_gem_object_funcs bo_funcs = {
	.free = bo_free,
	.close = bo_close,
	.export = bo_export,
	.mmap = bo_mmap,
};

static bool bo_args_valid(const struct gpgpu_bo_create *args)
{
	return (args->kind == GPGPU_MEMORY_HOST || args->kind == GPGPU_MEMORY_VRAM) &&
		args->size && args->size <= GPGPU_VRAM_USABLE_SIZE && args->alignment &&
		is_power_of_2(args->alignment) && args->alignment <= GPGPU_QUEUE_RING_MAX &&
		!args->reserved;
}

/* 分配存储：长度取页对齐与对齐要求中较大者，并向上取 2 的幂以满足对齐。 */
static int bo_storage_alloc(struct gpgpu_bo *bo, u64 alignment)
{
	u64 size = roundup_pow_of_two(max_t(u64, bo->mapped_size, alignment));
	int ret;

	if (bo->kind == GPGPU_MEMORY_HOST) {
		ret = gpgpu_dma_buf_alloc(bo->dev, &bo->host, size, alignment);
		bo->phys_addr = bo->host.addr;
		return ret;
	}
	return gpgpu_vram_alloc(bo->dev, size, &bo->vram_blocks, &bo->phys_addr);
}

/**
 * gpgpu_bo_create() - BO_CREATE
 * @fpriv: 会话；调用者持 dev->lock，函数内会暂时释放。
 * @args: ioctl 参数。
 *
 * 1. 检查参数；队列存活期间冻结对象集合；
 * 2. 分配存储并初始化 GEM，对象持有 DRM 与设备引用；
 * 3. 建立 mmap 偏移；
 * 4. 创建句柄：GEM 核心可能回调 bo_close()，因此先取临时引用并解锁；
 * 5. 填写输出。
 */
int gpgpu_bo_create(struct gpgpu_fpriv *fpriv, struct gpgpu_bo_create *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *bo;
	u32 handle;
	int ret;

	/* 1. 参数。 */
	if (!bo_args_valid(args))
		return -EINVAL;
	if (fpriv->vm.failed || gpgpu_queue_any_live(fpriv))
		return -EBUSY;

	/* 2. 存储与 GEM。 */
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->dev = dev;
	bo->kind = args->kind;
	bo->size = args->size;
	bo->mapped_size = PAGE_ALIGN(args->size);
	INIT_LIST_HEAD(&bo->vram_blocks);
	ret = bo_storage_alloc(bo, args->alignment);
	if (ret) {
		kfree(bo);
		return ret;
	}
	bo->gem.funcs = &bo_funcs;
	drm_gem_private_object_init(dev->drm, &bo->gem, bo->mapped_size);
	drm_dev_get(dev->drm);
	gpgpu_device_get(dev);
	bo->fpriv = fpriv;
	list_add_tail(&bo->node, &fpriv->bos);

	/* 3. mmap 偏移。 */
	ret = drm_gem_create_mmap_offset(&bo->gem);
	if (ret)
		goto drop;

	/* 4. 句柄；其他线程可能在解锁期间关闭刚创建的句柄。 */
	drm_gem_object_get(&bo->gem);
	mutex_unlock(&dev->lock);
	ret = drm_gem_handle_create(fpriv->file, &bo->gem, &handle);
	mutex_lock(&dev->lock);
	if (!ret && !bo->fpriv)
		ret = -ENOENT;
	if (ret) {
		if (bo->fpriv)
			gpgpu_bo_drop(bo);
		mutex_unlock(&dev->lock);
		drm_gem_object_put(&bo->gem);
		mutex_lock(&dev->lock);
		return ret;
	}
	drm_gem_object_put(&bo->gem);

	/* 5. 输出。 */
	bo->handle = handle;
	args->handle = handle;
	args->mapped_size = bo->mapped_size;
	args->mmap_offset = bo->kind == GPGPU_MEMORY_HOST ?
		drm_vma_node_offset_addr(&bo->gem.vma_node) : 0;
	return 0;
drop:
	gpgpu_bo_drop(bo);
	return ret;
}

/* 按句柄查找会话拥有的对象；调用者持 dev->lock。 */
struct gpgpu_bo *gpgpu_bo_lookup(struct gpgpu_fpriv *fpriv, u32 handle)
{
	struct gpgpu_bo *bo;

	if (!handle)
		return NULL;
	list_for_each_entry(bo, &fpriv->bos, node)
		if (bo->handle == handle)
			return bo;
	return NULL;
}

/* [offset, offset + size) 非空且位于应用请求的长度内。 */
bool gpgpu_bo_range_ok(struct gpgpu_bo *bo, u64 offset, u64 size)
{
	return bo && size && size <= bo->size && offset <= bo->size - size;
}

/* 会话结束：放下会话拥有的全部对象的列表引用。调用者持 dev->lock。 */
void gpgpu_bo_release_all(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_bo *bo;

	/* gpgpu_bo_drop() 会解锁，因此每次重新取链表头。 */
	while (!list_empty(&fpriv->bos)) {
		bo = list_first_entry(&fpriv->bos, struct gpgpu_bo, node);
		gpgpu_bo_drop(bo);
	}
}

/* 第 2 步：写 DMA 描述符并启动。调用者持 dma_lock。 */
static void copy_start(struct gpgpu_device *dev, bool to_host, u64 host, u64 vram, u32 size)
{
	unsigned long flags;

	/* 只清 DMA 自己的完成与错误位，不触及 IH 或其他状态。 */
	spin_lock_irqsave(&dev->irq_lock, flags);
	gpgpu_wreg(dev, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_DMA_DONE);
	gpgpu_wreg(dev, GPGPU_REG_ERROR_STATUS, GPGPU_ERR_DMA_FAULT);
	dev->dma_failed = false;
	reinit_completion(&dev->dma_done);
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	dma_wmb();
	gpgpu_wreg64(dev, GPGPU_REG_DMA_SRC_LO, to_host ? vram : host);
	gpgpu_wreg64(dev, GPGPU_REG_DMA_DST_LO, to_host ? host : vram);
	gpgpu_wreg(dev, GPGPU_REG_DMA_SIZE, size);
	gpgpu_wreg(dev, GPGPU_REG_DMA_CTRL,
		   GPGPU_DMA_START | GPGPU_DMA_IRQ_ENABLE | (to_host ? GPGPU_DMA_DIR_FROM_VRAM : 0));
}

/**
 * gpgpu_bo_copy() - COPY：在 Host 对象与 VRAM 对象之间同步搬运
 * @fpriv: 会话；调用者持 dev->lock，函数内会暂时释放。
 * @args: ioctl 参数。
 *
 * 1. 检查两端对象与范围，计入使用计数以防等待期间被解绑；
 * 2. 换 dma_lock，写描述符并启动；
 * 3. 等完成中断；超时则无法证明 DMA 已结束，此后禁止 COPY 并保留两端对象；
 * 4. 回到 dev->lock，确认结束后撤销使用计数。
 */
int gpgpu_bo_copy(struct gpgpu_fpriv *fpriv, struct gpgpu_memory_copy *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *host = gpgpu_bo_lookup(fpriv, args->host_handle);
	struct gpgpu_bo *vram = gpgpu_bo_lookup(fpriv, args->vram_handle);
	bool to_host = args->direction == GPGPU_COPY_D2H;
	int ret;

	/* 1. 检查与计数。 */
	if (args->direction > GPGPU_COPY_D2H || !host || !vram ||
	    host->kind != GPGPU_MEMORY_HOST || vram->kind != GPGPU_MEMORY_VRAM ||
	    !gpgpu_bo_range_ok(host, args->host_offset, args->size) ||
	    !gpgpu_bo_range_ok(vram, args->vram_offset, args->size))
		return -EINVAL;
	host->users++;
	vram->users++;
	mutex_unlock(&dev->lock);

	/* 2. 启动。 */
	mutex_lock(&dev->dma_lock);
	if (!READ_ONCE(dev->present) || READ_ONCE(dev->dma_uncertain) || READ_ONCE(dev->fatal)) {
		ret = -EIO;
		goto unlock;
	}
	copy_start(dev, to_host, host->phys_addr + args->host_offset,
		   vram->phys_addr + args->vram_offset, args->size);

	/* 3. 等待。 */
	if (!wait_for_completion_timeout(&dev->dma_done, msecs_to_jiffies(GPGPU_TIMEOUT_MS))) {
		WRITE_ONCE(dev->dma_uncertain, true);
		ret = -ETIMEDOUT;
	} else {
		ret = READ_ONCE(dev->dma_failed) ? -EIO : 0;
	}
	dma_rmb();
unlock:
	mutex_unlock(&dev->dma_lock);

	/* 4. 撤销计数。 */
	mutex_lock(&dev->lock);
	if (!dev->dma_uncertain) {
		host->users--;
		vram->users--;
	}
	return ret;
}
