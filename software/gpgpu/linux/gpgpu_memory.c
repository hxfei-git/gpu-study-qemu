// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "gpgpu_queue_drv.h"

/*
 * gpgpu_memory_init() - 建立队列模式的 VRAM 分配器
 *
 * 只将前 60 MiB 交给 gpu_buddy，另将首页登记为保留分配。
 */
int gpgpu_memory_init(struct gpgpu_qdev *dev)
{
#if IS_REACHABLE(CONFIG_GPU_BUDDY)
	int ret = gpu_buddy_init(&dev->buddy, GPGPU_VRAM_USABLE_SIZE, PAGE_SIZE);

	if (ret)
		return ret;
	dev->buddy_ready = true;
	/* 首页作为真实占用登记；固件区从未进入分配器。 */
	return gpu_buddy_alloc_blocks(&dev->buddy, 0, PAGE_SIZE, PAGE_SIZE, PAGE_SIZE,
				      &dev->reserved_blocks, GPU_BUDDY_RANGE_ALLOCATION);
#else
	return -EOPNOTSUPP;
#endif
}

/*
 * gpgpu_memory_fini() - 销毁 VRAM 分配器
 *
 * 所有用户块已归还后，释放保留首页并清理 buddy 状态。
 */
void gpgpu_memory_fini(struct gpgpu_qdev *dev)
{
#if IS_REACHABLE(CONFIG_GPU_BUDDY)
	if (dev->buddy_ready) {
		gpu_buddy_free_list(&dev->buddy, &dev->reserved_blocks, 0);
		gpu_buddy_fini(&dev->buddy);
		dev->buddy_ready = false;
	}
#endif
}

/*
 * gpgpu_memory_find() - 按句柄查找会话内存对象
 *
 * 调用方持 dev->lock，句柄不能跨会话使用。
 */
struct gpgpu_qmemory *gpgpu_memory_find(struct gpgpu_qclient *client, u32 id)
{
	struct gpgpu_qmemory *m;

	list_for_each_entry(m, &client->memory, node) {
		if (id && m->id == id)
			return m;
	}
	return NULL;
}

/*
 * gpgpu_memory_range() - 检查对象内字节范围
 *
 * 要求非空范围，使用剩余长度比较避免 offset + size 溢出。
 */
bool gpgpu_memory_range(struct gpgpu_qmemory *m, u64 offset, u64 size)
{
	u64 length = m && m->gem_ready ? m->logical_size : (m ? m->size : 0);

	return m && size && size <= length && offset <= length - size;
}

/*
 * gpgpu_memory_delete() - 回收内存对象及底层存储
 *
 * 持锁调用；Host 释放一致性 DMA 内存，VRAM 将块归还 buddy。
 */
void gpgpu_memory_storage_free(struct gpgpu_qmemory *m)
{
	struct gpgpu_qdev *dev = m->dev;

	if (m->kind == GPGPU_MEMORY_HOST) {
		dma_free_coherent(&dev->pdev->dev, m->size, m->cpu, m->dma);
	} else {
#if IS_REACHABLE(CONFIG_GPU_BUDDY)
		gpu_buddy_free_list(&dev->buddy, &m->blocks, 0);
#endif
	}
	kfree(m);
}

/* 持锁移除会话拥有的引用，GEM 其余引用继续保护存储。 */
static void gpgpu_memory_delete(struct gpgpu_qmemory *m)
{
	if (m->gem_ready) {
		gpgpu_gem_drop(m);
		return;
	}
	list_del(&m->node);
	gpgpu_memory_storage_free(m);
}

/*
 * gpgpu_memory_alloc() - 为会话分配 Host 或 VRAM 对象
 *
 * 校验大小和对齐，建立底层存储；GEM 包装随后分配用户句柄和映射令牌。
 */
int gpgpu_memory_alloc(struct gpgpu_qclient *client, struct gpgpu_bo_create *r)
{
	struct gpgpu_qdev *dev = client->dev;
	struct gpgpu_qmemory *m;
	u64 alignment = r->alignment;
	int ret = 0;

	if ((r->kind != GPGPU_MEMORY_HOST && r->kind != GPGPU_MEMORY_VRAM) || !r->size ||
	    r->size > GPGPU_VRAM_USABLE_SIZE || !alignment || !is_power_of_2(alignment) ||
	    alignment > GPGPU_QUEUE_RING_MAX || !client->next_memory) {
		return -EINVAL;
	}
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->client = client;
	m->dev = dev;
	m->logical_size = r->size;
	m->mapped_size = ALIGN(r->size, PAGE_SIZE);
	m->kind = r->kind;
	m->size = ALIGN(r->size, PAGE_SIZE);
	INIT_LIST_HEAD(&m->blocks);
	if (m->kind == GPGPU_MEMORY_HOST) {
		/*
		 * dma_alloc_coherent 按分配阶对齐；
		 * 进度页必须是可原子访问的 RAM。
		 */
		m->size = roundup_pow_of_two(max_t(u64, m->size, alignment));
		m->cpu = dma_alloc_coherent(&dev->pdev->dev, m->size, &m->dma, GFP_KERNEL);
		if (!m->cpu) {
			ret = -ENOMEM;
			goto fail;
		}
		if ((m->dma & (alignment - 1)) ||
		    !gpgpu_dma_address(dev, m->dma, m->size, &m->physical_address)) {
			dma_free_coherent(&dev->pdev->dev, m->size, m->cpu, m->dma);
			ret = -ERANGE;
			goto fail;
		}
		memset(m->cpu, 0, m->size);
	} else {
#if IS_REACHABLE(CONFIG_GPU_BUDDY)
		struct gpu_buddy_block *block;

		m->size = roundup_pow_of_two(max_t(u64, m->size, alignment));
		ret = gpu_buddy_alloc_blocks(&dev->buddy, PAGE_SIZE, GPGPU_VRAM_USABLE_SIZE,
					     m->size, m->size, &m->blocks,
					     GPU_BUDDY_CONTIGUOUS_ALLOCATION);
		if (ret)
			goto fail;
		block = list_first_entry(&m->blocks, struct gpu_buddy_block, link);
		m->offset = gpu_buddy_block_offset(block);
		m->physical_address = m->offset;
		memset_io(dev->vram + m->offset, 0, m->size);
#else
		ret = -EOPNOTSUPP;
		goto fail;
#endif
	}
	m->id = client->next_memory++;
	list_add_tail(&m->node, &client->memory);
	r->handle = m->id;
	r->mapped_size = m->mapped_size;
	return 0;
fail:
	kfree(m);
	return ret;
}

/*
 * gpgpu_memory_release_all() - 释放已结束会话的全部内存
 *
 * 调用方持锁并已确认相关设备访问和用户映射结束。
 */
void gpgpu_memory_release_all(struct gpgpu_qclient *client)
{
	struct gpgpu_qmemory *m, *next;

	list_for_each_entry_safe(m, next, &client->memory, node) {
		gpgpu_memory_delete(m);
	}
}

/*
 * gpgpu_memory_copy() - 通过 DMA 同步搬运两个对象内的数据
 *
 * 入口持设备锁；保留两端引用后换 DMA 锁等待，超时则继续保留引用。
 */
int gpgpu_memory_copy(struct gpgpu_qclient *client, struct gpgpu_memory_copy *r)
{
	struct gpgpu_qdev *dev = client->dev;
	struct gpgpu_qmemory *h = gpgpu_memory_find(client, r->host_handle);
	struct gpgpu_qmemory *v = gpgpu_memory_find(client, r->vram_handle);
	unsigned long flags;
	bool d2h = r->direction == GPGPU_COPY_D2H;
	int ret;

	if (r->direction > GPGPU_COPY_D2H || !h || !v || h->kind != GPGPU_MEMORY_HOST ||
	    v->kind != GPGPU_MEMORY_VRAM || !gpgpu_memory_range(h, r->host_offset, r->size) ||
	    !gpgpu_memory_range(v, r->vram_offset, r->size)) {
		return -EINVAL;
	}
	h->users++;
	v->users++;
	mutex_unlock(&dev->lock);
	mutex_lock(&dev->dma_lock);
	if (!READ_ONCE(dev->present) || READ_ONCE(dev->dma_uncertain) || READ_ONCE(dev->fatal)) {
		ret = -EIO;
		goto unlock;
	}
	spin_lock_irqsave(&dev->irq_lock, flags);
	/* 不清 IH、队列错误或其他请求状态。 */
	writel(GPGPU_IRQ_DMA_DONE, dev->regs + GPGPU_REG_IRQ_ACK);
	writel(GPGPU_ERR_DMA_FAULT, dev->regs + GPGPU_REG_ERROR_STATUS);
	dev->dma_failed = false;
	reinit_completion(&dev->dma_done);
	spin_unlock_irqrestore(&dev->irq_lock, flags);
	dma_wmb();
	gpgpu_qwrite64(dev, GPGPU_REG_DMA_SRC_LO,
		       d2h ? v->offset + r->vram_offset : h->physical_address + r->host_offset);
	gpgpu_qwrite64(dev, GPGPU_REG_DMA_DST_LO,
		       d2h ? h->physical_address + r->host_offset : v->offset + r->vram_offset);
	writel(r->size, dev->regs + GPGPU_REG_DMA_SIZE);
	writel(GPGPU_DMA_START | GPGPU_DMA_IRQ_ENABLE | (d2h ? GPGPU_DMA_DIR_FROM_VRAM : 0),
	       dev->regs + GPGPU_REG_DMA_CTRL);
	ret = wait_for_completion_timeout(&dev->dma_done, msecs_to_jiffies(GPGPU_Q_TIMEOUT_MS)) ?
		0 :
		-ETIMEDOUT;
	if (ret) {
		/*
		 * 不能证明 DMA 已结束时，
		 * 后续请求与资源回收均保持禁止。
		 */
		WRITE_ONCE(dev->dma_uncertain, true);
	} else if (READ_ONCE(dev->dma_failed)) {
		ret = -EIO;
	}
	dma_rmb();
unlock:
	mutex_unlock(&dev->dma_lock);
	mutex_lock(&dev->lock);
	if (!dev->dma_uncertain) {
		h->users--;
		v->users--;
	}
	return ret;
}
