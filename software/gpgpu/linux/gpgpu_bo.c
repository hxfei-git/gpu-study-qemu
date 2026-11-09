// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：GEM 对象、COPY 与 dcache 交接
 *
 * 每个对象的存储在创建时固定（Host 一致性内存或连续 VRAM），寿命由三类
 * 引用共同决定：
 *
 *   会话列表引用 ── 会话结束或句柄关闭且未绑定时放下
 *   GEM 句柄引用 ── 由 GEM 核心管理
 *   GPU 绑定引用 ── gpgpu_vm_map() 取得，gpgpu_vm_unmap() 放下
 *   CPU mmap 引用 ── GEM 核心随 VMA 管理
 *
 * 最后一个引用放下时 bo_free() 释放存储并放下设备引用。存储由 Host DMA
 * 内存或 VRAM 块的释放函数先回写并失效 dcache 中的旧行，不能确认时由
 * 它们保留存储，这段物理地址分给新对象时不会残留旧数据。
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

/* 释放存储；不能确认 dcache 已处置时由存储层保留。调用者持 dev->lock。 */
static void bo_storage_free(struct gpgpu_bo *bo)
{
	if (bo->kind == GPGPU_MEMORY_HOST)
		gpgpu_dma_buf_free(bo->dev, &bo->host);
	else
		gpgpu_vram_free(bo->dev, &bo->vram_blocks);
}

/* GEM 最后一个引用：交还存储，再放下 DRM 与设备引用。 */
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

/*
 * 只允许整个对象、共享、不可执行地映射。Host 对象映射一致性内存，
 * 可以读写。VRAM 对象只接受 PROT_NONE：用户库借它占住与 GPUVA 相同的
 * CPU 地址，这里不映射任何页，并清除 VM_MAYREAD/VM_MAYWRITE，之后也
 * 不能 mprotect 成可访问；VRAM 内容仍经 COPY 读写。
 */
static int bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct gpgpu_bo *bo = to_bo(obj);
	int ret;

	if (vma->vm_end - vma->vm_start != bo->mapped_size || !(vma->vm_flags & VM_SHARED) ||
	    (vma->vm_flags & VM_EXEC))
		return -EINVAL;

	vma->vm_pgoff = 0;
	vm_flags_clear(vma, VM_PFNMAP | VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	if (bo->kind == GPGPU_MEMORY_VRAM) {
		if (vma->vm_flags & (VM_READ | VM_WRITE))
			return -EACCES;
		vm_flags_clear(vma, VM_MAYREAD | VM_MAYWRITE);
		vma->vm_ops = &bo_vm_ops;
		return 0;
	}
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

/*
 * bo_args_valid() - 检查对象参数与对应内存域的容量
 *
 * 1. 检查标志、通知页和内存域；Host 容量由 Outbound 窗口限制，VRAM 不含
 *    固件保留区；
 * 2. 页对齐前检查 size_t 上界，再检查映射长度与对齐要求的较大值。
 *    取大后的长度不能超过对应内存域的容量。
 *
 * 实际能否分配仍由 DMA 分配器或连续 buddy 分配决定。
 *
 * Return: 参数、取整和分配长度均有效时为 true。
 */
static bool bo_args_valid(const struct gpgpu_bo_create *args)
{
	u64 capacity, mapped_size;

	/* 1. 通知页只能是一页 Host 对象，未知内存域不参与容量计算。 */
	if (args->flags & ~(GPGPU_BO_EVENT_PAGE | GPGPU_BO_UNCACHED))
		return false;
	if ((args->flags & GPGPU_BO_EVENT_PAGE) &&
	    (args->kind != GPGPU_MEMORY_HOST || args->size != GPGPU_EVENT_PAGE_SIZE))
		return false;
	switch (args->kind) {
	case GPGPU_MEMORY_HOST:
		capacity = GPGPU_OUTBOUND_SIZE;
		break;
	case GPGPU_MEMORY_VRAM:
		capacity = GPGPU_VRAM_USABLE_SIZE;
		break;
	default:
		return false;
	}

	/* 2. 先挡住 PAGE_ALIGN 溢出和 size_t 截断，再校验实际分配长度。 */
	if (!args->size || args->size > SIZE_MAX - (PAGE_SIZE - 1) || !args->alignment ||
	    !is_power_of_2(args->alignment) || args->alignment > GPGPU_QUEUE_RING_MAX)
		return false;

	mapped_size = PAGE_ALIGN(args->size);
	return max_t(u64, mapped_size, args->alignment) <= capacity;
}

/* 请求长度至少覆盖对象与对齐要求，底层分配器负责内部取整。 */
static int bo_storage_alloc(struct gpgpu_bo *bo, u64 alignment)
{
	u64 size = max_t(u64, bo->mapped_size, alignment);
	int ret;

	if (bo->kind == GPGPU_MEMORY_HOST) {
		ret = gpgpu_dma_buf_alloc(bo->dev, &bo->host, size, alignment);
		bo->phys_addr = bo->host.addr;
		return ret;
	}
	return gpgpu_vram_alloc(bo->dev, size, &bo->vram_blocks, &bo->phys_addr, true);
}

/**
 * gpgpu_bo_create() - BO_CREATE
 * @fpriv: 会话；调用者持 dev->lock，函数内会暂时释放。
 * @args: ioctl 参数。
 *
 * 为会话创建一个固定存储的对象：Host 对象分配一致性 DMA 内存，VRAM 对象
 * 从 gpu_buddy 分配连续块。存储在对象寿命内不迁移，绑定后的 GPU 地址
 * 因此不变。
 *
 * 1. VM 已失败时返回 -EBUSY，之后才检查参数格式。新对象尚未绑定，
 *    不改变任何队列能访问的地址，队列存活时也可以创建。带
 *    GPGPU_BO_EVENT_PAGE 而会话已有通知页时返回 -EEXIST；
 * 2. 分配并清零存储。带 GPGPU_BO_UNCACHED 或 GPGPU_BO_EVENT_PAGE 的对象
 *    记为不可缓存，绑定时写进叶子项。然后初始化 GEM。对象另取 DRM 与
 *    设备引用，句柄和会话都关闭后对象仍能安全释放存储；挂入会话链表
 *    即持有列表引用。可选 VRAM DMA 清零只在这个未公开阶段释放主锁，
 *    重锁后先复核设备与会话状态，之后才初始化 GEM。
 *    通知页在第 4 步解锁之前登记，并发的第二个通知页请求会在第 1 步
 *    失败；
 * 3. 建立 mmap 偏移，对象之后可经 render node 映射到用户态：Host 对象
 *    可读写，VRAM 对象只能以 PROT_NONE 映射；
 * 4. 创建 GEM 句柄。句柄一出现，其他线程就可能用它关闭对象，GEM 核心
 *    随即回调要取 dev->lock 的 bo_close()。因此先取临时引用保住对象，
 *    解锁后再创建；重新加锁后若句柄已被关闭，按失败处理；
 * 5. 记录句柄，返回映射长度和 mmap 偏移。
 *
 * 失败时撤销通知页登记并放下列表引用；GEM 的最后一个引用会经 bo_free()
 * 释放存储。
 */
int gpgpu_bo_create(struct gpgpu_fpriv *fpriv, struct gpgpu_bo_create *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *bo;
	u32 handle;
	int ret;

	/* 1. 先检查 VM 状态，再检查参数；每个会话只有一个通知页。 */
	if (fpriv->vm.failed)
		return -EBUSY;
	if (!bo_args_valid(args))
		return -EINVAL;
	if ((args->flags & GPGPU_BO_EVENT_PAGE) && fpriv->vm.notify)
		return -EEXIST;

	/* 2. 分配存储，初始化 GEM，再取 DRM、设备引用并挂入会话。 */
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->dev = dev;
	bo->kind = args->kind;
	bo->size = args->size;
	bo->mapped_size = PAGE_ALIGN(args->size);
	bo->uncached = args->flags & (GPGPU_BO_UNCACHED | GPGPU_BO_EVENT_PAGE);
	INIT_LIST_HEAD(&bo->vram_blocks);

	ret = bo_storage_alloc(bo, args->alignment);
	if (ret)
		goto free_bo;

	/* DMA 清零曾放下主锁；对象尚未公开，复核后才能登记 GEM。 */
	if (bo->kind == GPGPU_MEMORY_VRAM && dev->vram_zero.cpu) {
		if (!dev->present)
			ret = -ENODEV;
		else if (READ_ONCE(dev->fatal))
			ret = -EIO;
		else if (fpriv->vm.failed)
			ret = -EBUSY;
		if (ret) {
			bo_storage_free(bo);
			goto free_bo;
		}
	}

	bo->gem.funcs = &bo_funcs;
	drm_gem_private_object_init(dev->drm, &bo->gem, bo->mapped_size);
	drm_dev_get(dev->drm);
	gpgpu_device_get(dev);
	bo->fpriv = fpriv;
	list_add_tail(&bo->node, &fpriv->bos);
	if (args->flags & GPGPU_BO_EVENT_PAGE)
		gpgpu_vm_set_event_page(fpriv, bo);

	/* 3. 为 GEM 分配 mmap 偏移，失败时放下刚取得的列表引用。 */
	ret = drm_gem_create_mmap_offset(&bo->gem);
	if (ret)
		goto drop;

	/*
	 * 4. 临时引用保证解锁期间对象不被释放。其间其他线程可能
	 *    已关闭刚创建的句柄，bo_close() 会把 bo->fpriv 清为 NULL。
	 */
	drm_gem_object_get(&bo->gem);
	mutex_unlock(&dev->lock);
	ret = drm_gem_handle_create(fpriv->file, &bo->gem, &handle);
	mutex_lock(&dev->lock);
	if (!ret && !bo->fpriv)
		ret = -ENOENT;

	if (ret) {
		if (fpriv->vm.notify == bo)
			gpgpu_vm_set_event_page(fpriv, NULL);
		if (bo->fpriv)
			gpgpu_bo_drop(bo);
		mutex_unlock(&dev->lock);
		drm_gem_object_put(&bo->gem);
		mutex_lock(&dev->lock);
		return ret;
	}

	drm_gem_object_put(&bo->gem);

	/* 5. 句柄有效，填写输出。 */
	bo->handle = handle;
	args->handle = handle;
	args->mapped_size = bo->mapped_size;
	args->mmap_offset = drm_vma_node_offset_addr(&bo->gem.vma_node);
	return 0;

drop:
	if (fpriv->vm.notify == bo)
		gpgpu_vm_set_event_page(fpriv, NULL);
	gpgpu_bo_drop(bo);
	return ret;

free_bo:
	kfree(bo);
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

/*
 * 第 2 步：写 DMA 描述符并启动，调用者持 dma_lock。
 *
 * 先在 irq_lock 下清除上一次的 DMA 完成与错误状态，并重置 completion，
 * 保证随后等到的是本次的完成中断。dma_wmb() 让 Host 侧源数据先于启动
 * 对设备可见，再写源、目的、长度，最后写 CTRL 启动。
 */
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

int gpgpu_dma_copy(struct gpgpu_device *dev, bool to_host, u64 host, u64 vram, u32 size)
{
	int ret;

	/* 1. 调用者已持 dma_lock，拒绝不能安全启动的新事务。 */
	if (!READ_ONCE(dev->present) || READ_ONCE(dev->dma_uncertain) || READ_ONCE(dev->fatal))
		return -EIO;

	/* 2. COPY 与 BO 清零共用同一份发布和 completion 初始化逻辑。 */
	copy_start(dev, to_host, host, vram, size);

	/* 3. 只有本次等待超时才表示本次访问仍可能在途。 */
	if (!wait_for_completion_timeout(&dev->dma_done, msecs_to_jiffies(GPGPU_TIMEOUT_MS))) {
		WRITE_ONCE(dev->dma_uncertain, true);
		ret = -ETIMEDOUT;
	} else {
		ret = READ_ONCE(dev->dma_failed) ? -EIO : 0;
	}
	dma_rmb();
	return ret;
}

/**
 * gpgpu_bo_copy() - COPY：在 Host 对象与 VRAM 对象之间同步搬运
 * @fpriv: 会话；调用者持 dev->lock，函数内会暂时释放。
 * @args: ioctl 参数。
 *
 * 用设备 DMA 在 Host 对象和 VRAM 对象之间搬运一段数据，并等待完成。
 * 等待期间不持 dev->lock，同一时刻只允许一个 COPY。
 *
 * 1. 检查方向、对象种类与两端范围。两端对象的 users 加一，等待期间
 *    它们不能被解绑或释放；随后释放 dev->lock；
 * 2. 取 dma_lock 串行化整个请求。设备已移除、已故障或之前有 DMA 结果
 *    未知时返回 -EIO，否则写描述符并启动；
 * 3. 等完成中断。超时则无法证明 DMA 已结束，置 dma_uncertain：此后
 *    禁止 COPY、解绑与会话回收。完成后 dma_rmb()，保证调用者随后读到
 *    的是 DMA 写入的数据；
 * 4. 重新取 dev->lock，按本次结果撤销 users：没有启动（-EIO）、正常完成
 *    或设备报告 DMA 错误（-EIO，搬运已停止）时撤销；只有本次超时
 *    （-ETIMEDOUT）的两端对象继续保留，并记到 dev->copy_host 与
 *    dev->copy_vram，设备恢复确认停止后才撤销。dma_uncertain 可能来自之前
 *    另一次 COPY，不能用它判断本次是否还在访问。
 */
int gpgpu_bo_copy(struct gpgpu_fpriv *fpriv, struct gpgpu_memory_copy *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *host = gpgpu_bo_lookup(fpriv, args->host_handle);
	struct gpgpu_bo *vram = gpgpu_bo_lookup(fpriv, args->vram_handle);
	bool to_host = args->direction == GPGPU_COPY_D2H;
	int ret;

	/* 1. 检查参数，再计入两端对象的使用计数。 */
	if (args->direction > GPGPU_COPY_D2H || !host || !vram ||
	    host->kind != GPGPU_MEMORY_HOST || vram->kind != GPGPU_MEMORY_VRAM ||
	    !gpgpu_bo_range_ok(host, args->host_offset, args->size) ||
	    !gpgpu_bo_range_ok(vram, args->vram_offset, args->size))
		return -EINVAL;

	host->users++;
	vram->users++;
	mutex_unlock(&dev->lock);

	/* 2. 3. 换成 dma_lock，共用物理 DMA 的启动、等待和超时处理。 */
	mutex_lock(&dev->dma_lock);
	ret = gpgpu_dma_copy(dev, to_host, host->phys_addr + args->host_offset,
			     vram->phys_addr + args->vram_offset, args->size);
	mutex_unlock(&dev->dma_lock);

	/* 4. 回到 dev->lock；只有本次结果未知时保留计数并记下两端对象。 */
	mutex_lock(&dev->lock);
	if (ret != -ETIMEDOUT) {
		host->users--;
		vram->users--;
	} else {
		dev->copy_host = host;
		dev->copy_vram = vram;
	}
	return ret;
}

/**
 * gpgpu_bo_cache_sync() - CACHE_SYNC：按对象范围回写或失效 dcache
 * @fpriv: 会话；调用者持 dev->lock。
 * @args: ioctl 参数。
 *
 * CPU 改写可缓存对象后、发布任务前失效旧副本；GPU 写出的数据在任务
 * 完成时已由 CP 回写，需要提前取得时用回写。对象存储按页对齐，不与
 * 其他对象共用缓存行，设备把范围扩展到整行也不会触及别的对象。缓存
 * 属性由页表决定，驱动不据对象标志推断它没有缓存行，不可缓存对象
 * 同样发出命令；范围内没有缓存行时设备只做查找。
 *
 * Return: 0、-EINVAL，或 gpgpu_dcache_command() 的错误；只失效时遇到脏行
 * 为 -EBUSY，说明设备仍持有未交还的数据。
 */
int gpgpu_bo_cache_sync(struct gpgpu_fpriv *fpriv, struct gpgpu_cache_sync *args)
{
	struct gpgpu_bo *bo = gpgpu_bo_lookup(fpriv, args->handle);
	u32 cmd = 0;

	if (!gpgpu_bo_range_ok(bo, args->offset, args->size) || args->reserved || !args->op ||
	    (args->op & ~(GPGPU_CACHE_CLEAN | GPGPU_CACHE_INVALIDATE)))
		return -EINVAL;
	if (args->op & GPGPU_CACHE_CLEAN)
		cmd |= GPGPU_DCACHE_CLEAN;
	if (args->op & GPGPU_CACHE_INVALIDATE)
		cmd |= GPGPU_DCACHE_INVALIDATE;
	return gpgpu_dcache_command(fpriv->dev, cmd, bo->phys_addr + args->offset, args->size);
}
