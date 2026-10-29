// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：用户 AQL 队列与 Doorbell
 *
 * 创建队列（gpgpu_queue_create）：
 *   1. 检查 Ring 与进度：两个 BO 都已绑定、属于 Host 内存、不被别的队列占用，
 *      偏移对齐且 Ring 与 R/W 不重叠；当前 R == W；
 *   2. 分配会话的 Doorbell 页和页内入口；
 *   3. 分配 MQD，Ring 全部置为 INVALID；
 *   4. 经 HIQ 发送 MAP_QUEUES。结果未知时队列保留为 FAULTED，CP 可能仍在
 *      访问它；确认失败时直接删除。
 *
 * 销毁队列（gpgpu_queue_destroy）：正常队列必须已排空（R == W），经 HIQ 发送
 * UNMAP_QUEUES，确认后消费旧 IH，再放下 Ring 与进度的使用计数。队列对象本身
 * 等事件和 Doorbell 映射都不再引用时才回收。
 */
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "gpgpu_drv.h"

struct gpgpu_queue *gpgpu_queue_lookup(struct gpgpu_fpriv *fpriv, u32 id)
{
	struct gpgpu_queue *q;

	list_for_each_entry(q, &fpriv->queues, node)
		if (q->id == id)
			return q;
	return NULL;
}

/* 是否有尚未确认卸载的队列（包括出错和结果未知的）；存活期间冻结地址空间。 */
bool gpgpu_queue_any_live(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_queue *q;

	list_for_each_entry(q, &fpriv->queues, node)
		if (!q->dead)
			return true;
	return false;
}

/* 删除队列对象；未卸载的队列同时放下 Ring 与进度的使用计数。调用者持 dev->lock。 */
void gpgpu_queue_free(struct gpgpu_queue *q)
{
	if (!q->dead) {
		q->ring->users--;
		q->progress->users--;
	}
	gpgpu_dma_buf_free(q->fpriv->dev, &q->mqd);
	list_del(&q->node);
	kfree(q);
}

static bool queue_has_events(struct gpgpu_fpriv *fpriv, struct gpgpu_queue *q)
{
	struct gpgpu_event *e;

	list_for_each_entry(e, &fpriv->events, node)
		if (e->queue == q)
			return true;
	return false;
}

/* 回收已卸载、且没有事件引用的队列；Doorbell 页仍有映射时一律保留，防止入口串用。 */
static void queue_collect(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_queue *q, *next;

	if (atomic_read(&fpriv->db_maps))
		return;
	list_for_each_entry_safe(q, next, &fpriv->queues, node)
		if (q->dead && !queue_has_events(fpriv, q))
			gpgpu_queue_free(q);
}

static bool ranges_overlap(u64 a, u64 a_size, u64 b, u64 b_size)
{
	return a < b + b_size && b < a + a_size;
}

/* 第 1 步：Ring 与进度对象及偏移。 */
static bool queue_args_valid(struct gpgpu_bo *ring, struct gpgpu_bo *progress,
			     const struct gpgpu_queue_create *args)
{
	if (args->reserved || !ring || !progress || !ring->bound || !progress->bound ||
	    ring->kind != GPGPU_MEMORY_HOST || progress->kind != GPGPU_MEMORY_HOST)
		return false;
	if (!is_power_of_2(args->ring_size) || args->ring_size < GPGPU_QUEUE_RING_MIN ||
	    args->ring_size > GPGPU_QUEUE_RING_MAX || (args->ring_offset & 63) ||
	    (args->rptr_offset & 7) || (args->wptr_offset & 7) ||
	    args->rptr_offset == args->wptr_offset)
		return false;
	if (!gpgpu_bo_range_ok(ring, args->ring_offset, args->ring_size) ||
	    !gpgpu_bo_range_ok(progress, args->rptr_offset, 4) ||
	    !gpgpu_bo_range_ok(progress, args->wptr_offset, 4))
		return false;
	return ring != progress ||
		(!ranges_overlap(args->ring_offset, args->ring_size, args->rptr_offset, 4) &&
		 !ranges_overlap(args->ring_offset, args->ring_size, args->wptr_offset, 4));
}

/* 第 1 步：同一个 BO 不能同时作为两条存活队列的 Ring 或进度。返回已用入口位图。 */
static int queue_claim_storage(struct gpgpu_fpriv *fpriv, struct gpgpu_bo *ring,
			       struct gpgpu_bo *progress, unsigned long *used)
{
	struct gpgpu_queue *other;

	*used = 0;
	list_for_each_entry(other, &fpriv->queues, node) {
		*used |= BIT(other->db_index);
		if (!other->dead && (other->ring == ring || other->progress == progress ||
				     other->ring == progress || other->progress == ring))
			return -EBUSY;
	}
	return 0;
}

/* 第 2 步：会话第一次建队列时分配一页 Doorbell，页内入口按会话分配。 */
static int queue_alloc_doorbell(struct gpgpu_fpriv *fpriv, unsigned long used, u32 *index)
{
	struct gpgpu_device *dev = fpriv->dev;
	unsigned int pages = GPGPU_DOORBELL_BAR_SIZE / PAGE_SIZE;

	if ((used & (BIT(GPGPU_QUEUES_PER_FILE) - 1)) == BIT(GPGPU_QUEUES_PER_FILE) - 1)
		return -ENOSPC;
	if (!fpriv->db_page) {
		unsigned int page = find_first_zero_bit(&dev->db_pages, pages);

		if (page >= pages)
			return -ENOSPC;
		set_bit(page, &dev->db_pages);
		fpriv->db_page = page;
	}
	*index = (used & 1) ? 1 : 0;
	return 0;
}

/* 第 3 步：填写 MQD 并把 Ring 的全部包置为 INVALID。 */
static void queue_init_mqd(struct gpgpu_queue *q, const struct gpgpu_queue_create *args,
			   u32 rptr)
{
	struct gpgpu_fpriv *fpriv = q->fpriv;
	GpgpuMqd *mqd = q->mqd.cpu;

	*mqd = (GpgpuMqd) {
		.ring_base = cpu_to_le64(q->ring->gpu_addr + args->ring_offset),
		.rptr_report_addr = cpu_to_le64(q->progress->gpu_addr + args->rptr_offset),
		.wptr_poll_addr = cpu_to_le64(q->progress->gpu_addr + args->wptr_offset),
		.pq_control = cpu_to_le32(args->ring_size),
		.doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE |
						(fpriv->db_page * PAGE_SIZE + q->db_index * 8)),
		.vmid = cpu_to_le32(fpriv->vm.vmid),
		.aql_control = cpu_to_le32(1),
		.saved_rptr = cpu_to_le64(rptr),
	};
	for (u32 i = 0; i < args->ring_size; i += sizeof(GpgpuAqlPacket))
		WRITE_ONCE(*(u32 *)(q->ring->host.cpu + args->ring_offset + i),
			   cpu_to_le32(GPGPU_AQL_INVALID));
}

/*
 * 第 4 步：MAP_QUEUES 失败后的处理。从未发布、或 CP 明确拒绝（未占用槽）
 * 时可以删除；MQD 访问失败、需要恢复或结果未知时，CP 可能仍持有该队列，保留。
 */
static int queue_map_failed(struct gpgpu_queue *q, int ret, u32 status, bool published)
{
	bool cp_may_hold = published && (ret || status == GPGPU_MANAGEMENT_MQD_FAULT ||
					 status == GPGPU_MANAGEMENT_RECOVERY_REQUIRED);

	if (cp_may_hold)
		q->state = GPGPU_QUEUE_FAULTED;
	else
		gpgpu_queue_free(q);
	if (ret)
		return ret;
	return status == GPGPU_MANAGEMENT_NO_SLOT ? -ENOSPC : -EIO;
}

int gpgpu_queue_create(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_create *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *ring = gpgpu_bo_lookup(fpriv, args->ring_handle);
	struct gpgpu_bo *progress = gpgpu_bo_lookup(fpriv, args->progress_handle);
	struct gpgpu_queue *q;
	unsigned long used;
	u32 rptr, db_index, status;
	bool published;
	int ret;

	/* 1. 对象与进度。 */
	queue_collect(fpriv);
	if (fpriv->vm.failed || !fpriv->vm.vmid || gpgpu_queue_any_live(fpriv))
		return -EBUSY;
	if (!queue_args_valid(ring, progress, args) || !fpriv->next_queue)
		return -EINVAL;
	ret = queue_claim_storage(fpriv, ring, progress, &used);
	if (ret)
		return ret;
	rptr = le32_to_cpu(READ_ONCE(*(u32 *)(progress->host.cpu + args->rptr_offset)));
	if (rptr != le32_to_cpu(READ_ONCE(*(u32 *)(progress->host.cpu + args->wptr_offset))))
		return -EBUSY;

	/* 2. Doorbell。 */
	ret = queue_alloc_doorbell(fpriv, used, &db_index);
	if (ret)
		return ret;

	/* 3. 队列对象与 MQD。 */
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	ret = gpgpu_dma_buf_alloc(dev, &q->mqd, sizeof(GpgpuMqd), 8);
	if (ret) {
		kfree(q);
		return ret;
	}
	q->fpriv = fpriv;
	q->ring = ring;
	q->progress = progress;
	q->rptr = progress->host.cpu + args->rptr_offset;
	q->wptr = progress->host.cpu + args->wptr_offset;
	q->db_index = db_index;
	q->id = fpriv->next_queue++;
	q->state = GPGPU_QUEUE_UNLOADING;
	q->uncertain = true;
	ring->users++;
	progress->users++;
	queue_init_mqd(q, args, rptr);
	list_add_tail(&q->node, &fpriv->queues);

	/* 4. 映射。 */
	ret = gpgpu_mgmt_submit(dev, GPGPU_HQD_SLOT_HIQ, GPGPU_MAP_QUEUES, GPGPU_QUEUE_USER_AQL,
				q->mqd.addr, &status, &published);
	q->management_status = status;
	if (ret || status != GPGPU_MANAGEMENT_OK)
		return queue_map_failed(q, ret, status, published);
	if (dev->fatal) {
		q->state = GPGPU_QUEUE_FAULTED;
		return -EIO;
	}
	q->uncertain = false;
	q->state = GPGPU_QUEUE_READY;
	args->queue_id = q->id;
	args->doorbell_offset = q->db_index * 8;
	args->doorbell_mmap_offset = (u64)GPGPU_DOORBELL_MMAP_PAGE << PAGE_SHIFT;
	args->doorbell_map_size = PAGE_SIZE;
	return 0;
}

static struct gpgpu_queue *queue_get(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args)
{
	struct gpgpu_queue *q = gpgpu_queue_lookup(fpriv, args->queue_id);

	if (!q || q->dead || args->reserved)
		return NULL;
	gpgpu_vm_save_fault(q);
	return q;
}

static void queue_report(struct gpgpu_queue *q, struct gpgpu_queue_state *args)
{
	args->state = q->state;
	args->error = q->error;
	args->management_status = q->management_status;
}

int gpgpu_queue_query(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args)
{
	struct gpgpu_queue *q = queue_get(fpriv, args);

	if (!q)
		return -EINVAL;
	queue_report(q, args);
	return 0;
}

/*
 * gpgpu_queue_destroy() - 正常卸载一条队列
 *
 * 1. 结果未知或正在卸载时拒绝；READY 队列必须已排空；
 * 2. 发送 UNMAP_QUEUES；
 * 3. 确认成功：消费旧 IH，放下使用计数，标记 dead；
 *    从未发布：恢复原状态；已发布但 CP 拒绝：队列视为出错；
 *    已发布但结果未知：保持 UNLOADING 与 uncertain，资源全部保留。
 */
int gpgpu_queue_destroy(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_queue *q = queue_get(fpriv, args);
	u32 status, previous;
	bool published;
	int ret;

	/* 1. 前提。 */
	if (!q)
		return -EINVAL;
	if (q->uncertain || q->state == GPGPU_QUEUE_UNLOADING)
		return -EBUSY;
	dma_rmb();
	if (q->state == GPGPU_QUEUE_READY && READ_ONCE(*q->rptr) != READ_ONCE(*q->wptr))
		return -EBUSY;

	/* 2. 卸载。 */
	previous = q->state;
	q->state = GPGPU_QUEUE_UNLOADING;
	q->uncertain = true;
	ret = gpgpu_mgmt_submit(dev, GPGPU_HQD_SLOT_HIQ, GPGPU_UNMAP_QUEUES, GPGPU_QUEUE_USER_AQL,
				q->mqd.addr, &status, &published);
	q->management_status = status;

	/* 3. 结果。 */
	if (!ret && status == GPGPU_MANAGEMENT_OK) {
		gpgpu_ih_drain(dev);
		q->ring->users--;
		q->progress->users--;
		q->dead = true;
		q->uncertain = false;
	} else if (!published) {
		q->uncertain = false;
		q->state = dev->fatal ? GPGPU_QUEUE_FAULTED : previous;
	} else if (!ret) {
		q->state = GPGPU_QUEUE_FAULTED;
		ret = -EIO;
	}
	queue_report(q, args);
	return ret;
}

/* Doorbell 页的 VMA 引用：存活期间不回收页内入口对应的队列。 */
static void doorbell_vma_open(struct vm_area_struct *vma)
{
	struct gpgpu_fpriv *fpriv = vma->vm_private_data;

	atomic_inc(&fpriv->db_maps);
}

static void doorbell_vma_close(struct vm_area_struct *vma)
{
	struct gpgpu_fpriv *fpriv = vma->vm_private_data;

	atomic_dec(&fpriv->db_maps);
}

static const struct vm_operations_struct doorbell_vm_ops = {
	.open = doorbell_vma_open,
	.close = doorbell_vma_close,
};

/**
 * gpgpu_doorbell_mmap() - 把会话的 Doorbell 页映射给用户
 * @fpriv: 会话。
 * @vma: 偏移令牌为 GPGPU_DOORBELL_MMAP_PAGE、长度一页、共享且不可执行。
 *
 * Return: 0 或负错误码。
 */
int gpgpu_doorbell_mmap(struct gpgpu_fpriv *fpriv, struct vm_area_struct *vma)
{
	struct gpgpu_device *dev = fpriv->dev;
	unsigned long pfn;
	int ret = -EINVAL;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	mutex_lock(&dev->lock);
	if (!dev->present) {
		ret = -ENODEV;
		goto out;
	}
	if (!fpriv->db_page || vma->vm_end - vma->vm_start != PAGE_SIZE)
		goto out;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vm_flags_clear(vma, VM_MAYEXEC);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	pfn = (pci_resource_start(dev->pdev, 4) >> PAGE_SHIFT) + fpriv->db_page;
	ret = io_remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE, vma->vm_page_prot);
	if (ret)
		goto out;
	vma->vm_private_data = fpriv;
	vma->vm_ops = &doorbell_vm_ops;
	doorbell_vma_open(vma);
out:
	mutex_unlock(&dev->lock);
	return ret;
}
