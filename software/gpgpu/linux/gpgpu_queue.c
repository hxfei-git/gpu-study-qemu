// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：用户 AQL 队列与 Doorbell
 *
 * 创建队列（gpgpu_queue_create）：
 *   1. 检查 Ring 与进度：两个 BO 都已绑定、属于不可缓存的 Host 内存、不被
 *      别的队列占用，偏移对齐且 Ring 与 R/W 不重叠；当前 R == W；
 *   2. 分配会话的 Doorbell 页和页内入口；
 *   3. 分配 MQD 并登记为不可缓存；
 *   4. 取得 VMID（gpgpu_vm_attach()），填写 MQD，Ring 全部置为 INVALID；
 *   5. 经 HIQ 发送 MAP_QUEUES。CP 可能仍持有队列时（结果未知、重复映射、
 *      需要恢复）队列保留为 FAULTED，VMID 也一并保留；CP 明确拒绝且没有
 *      保留槽位时直接删除并归还 VMID。
 *
 * 销毁队列（gpgpu_queue_destroy）：正常队列必须已排空（R == W），经 HIQ 发送
 * UNMAP_QUEUES，确认后消费旧 IH，再放下 Ring 与进度的使用计数并归还 VMID。
 * CP 明确拒绝时队列仍驻留，资源保留，调用者处理后可以再次卸载；只有
 * 结果未知时才禁止再次发送。队列对象本身等事件和 Doorbell 映射都不再
 * 引用时才回收。
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

/* 是否有尚未确认卸载的队列（包括出错和结果未知的）。 */
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
	gpgpu_ctrl_buf_free(q->fpriv->dev, &q->mqd);
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

/*
 * 第 1 步：Ring 与进度对象及偏移。两个对象都须是不可缓存的 Host 对象：
 * CP 持续轮询 W 与 Header，R、W 还可能落在同一条缓存行内，经 dcache
 * 访问会读到旧值，整行回写也会覆盖 CPU 新写的字段。
 */
static bool queue_args_valid(struct gpgpu_bo *ring, struct gpgpu_bo *progress,
			     const struct gpgpu_queue_create *args)
{
	if (args->reserved || ring->kind != GPGPU_MEMORY_HOST ||
	    progress->kind != GPGPU_MEMORY_HOST || !ring->uncached || !progress->uncached)
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

/*
 * 第 2 步：会话第一次建队列时从 Doorbell BAR 分配一页，此后复用这一页。
 * used 是会话已占用的页内入口位图；入口 0、1 都被占用时返回 -ENOSPC，
 * 否则取空闲的那一个。
 */
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
 * queue_map_failed() - 第 5 步：MAP_QUEUES 失败后的处理
 *
 * 按 CP 是否可能仍持有这份 MQD 决定删除还是保留：
 *
 * 1. 从未发布，或 CP 明确拒绝且没有为它保留槽位时，删除队列并归还
 *    VMID。名额已满、参数非法、读 MQD 或读 W 失败（MQD_FAULT）都属于后者：
 *    固件在写入 HQD 之前失败，已撤销占用登记；
 * 2. 重复映射说明 CP 已绑定同一 MQD 地址，需要恢复或结果未知时 CP 可能
 *    仍持有该队列，这三种情况保留队列与 VMID。
 *
 * 用户库把 -EIO、-ETIMEDOUT、-EPROTO 当作结果未知并保留资源，所以 CP
 * 明确拒绝时返回 -ENOSPC 或 -EINVAL。
 *
 * Return: 从未发布时为管理提交的错误；CP 明确拒绝时为 -ENOSPC（名额已满）
 * 或 -EINVAL；保留队列时为管理提交的错误或 -EIO。
 */
static int queue_map_failed(struct gpgpu_queue *q, int ret, u32 status, bool published)
{
	struct gpgpu_fpriv *fpriv = q->fpriv;
	bool cp_may_hold = published && (ret || status == GPGPU_MANAGEMENT_DUPLICATE ||
					 status == GPGPU_MANAGEMENT_RECOVERY_REQUIRED);

	/* 2. 保留队列；uncertain 不清除，资源留到设备确认停止。 */
	if (cp_may_hold) {
		q->state = GPGPU_QUEUE_FAULTED;
		return ret ? ret : -EIO;
	}

	/* 1. CP 没有收到命令或没有持有这份 MQD。 */
	gpgpu_queue_free(q);
	gpgpu_vm_detach(fpriv);
	if (ret)
		return ret;
	return status == GPGPU_MANAGEMENT_NO_SLOT ? -ENOSPC : -EINVAL;
}

/*
 * gpgpu_queue_create() - QUEUE_CREATE：建立一条用户 AQL 队列
 *
 * 调用者持 vm_serial 与 dev->lock；第 5 步等待管理结果时会暂时释放
 * dev->lock。步骤见文件头。
 *
 * 队列对象在发送 MAP_QUEUES 前就登记为 UNLOADING + uncertain，并取得
 * Ring 与进度的使用计数。等待期间即使文件关闭，会话也会因这条
 * 未确认的队列保留为 orphan，Ring 与进度不会被解绑或释放。确认
 * 映射成功后才转为 READY。
 */
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

	/*
	 * 1. 先回收可回收的旧队列。对象不存在、未绑定，或会话仍有
	 *    存活队列时返回 -EBUSY（每个 VM 只有一条队列）；格式错误
	 *    返回 -EINVAL。
	 */
	queue_collect(fpriv);
	if (fpriv->vm.failed || !fpriv->vm.ready || !ring || !progress || !ring->bound ||
	    !progress->bound || gpgpu_queue_any_live(fpriv))
		return -EBUSY;
	if (!queue_args_valid(ring, progress, args) || !fpriv->next_queue)
		return -EINVAL;
	ret = queue_claim_storage(fpriv, ring, progress, &used);
	if (ret)
		return ret;

	/* MQD 从当前 R 恢复，因此要求 R == W：Ring 中没有未消费的包。 */
	rptr = le32_to_cpu(READ_ONCE(*(u32 *)(progress->host.cpu + args->rptr_offset)));
	if (rptr != le32_to_cpu(READ_ONCE(*(u32 *)(progress->host.cpu + args->wptr_offset))))
		return -EBUSY;

	/* 2. 首次建队列时为会话分配 Doorbell 页，再选页内空闲入口。 */
	ret = queue_alloc_doorbell(fpriv, used, &db_index);
	if (ret)
		return ret;

	/* 3. 分配队列对象与 MQD。 */
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	ret = gpgpu_ctrl_buf_alloc(dev, &q->mqd, sizeof(GpgpuMqd), 8);
	if (ret) {
		kfree(q);
		return ret;
	}

	/*
	 * 4. 队列驻留期间占用 VMID，MQD 写入这个编号。之后把队列登记为
	 *    “结果未知”，取得 Ring 与进度的使用计数，再填 MQD、把 Ring
	 *    置为 INVALID。
	 */
	ret = gpgpu_vm_attach(fpriv);
	if (ret) {
		gpgpu_ctrl_buf_free(dev, &q->mqd);
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

	/*
	 * 5. 经 HIQ 发送 MAP_QUEUES，由 CP 选择空闲用户槽装载 MQD。失败交给
	 *    queue_map_failed() 决定删除还是保留；等待期间设备若进入故障，
	 *    队列保留为 FAULTED。
	 */
	ret = gpgpu_mgmt_submit(dev, GPGPU_HQD_SLOT_HIQ, GPGPU_MAP_QUEUES, GPGPU_QUEUE_USER_AQL,
				q->mqd.addr, &status, &published);
	q->management_status = status;
	if (ret || status != GPGPU_MANAGEMENT_OK)
		return queue_map_failed(q, ret, status, published);
	if (dev->fatal) {
		q->state = GPGPU_QUEUE_FAULTED;
		return -EIO;
	}

	/* 映射已确认：返回队列编号和 Doorbell 的映射令牌与页内偏移。 */
	q->uncertain = false;
	q->state = GPGPU_QUEUE_READY;
	args->queue_id = q->id;
	args->doorbell_offset = q->db_index * 8;
	args->doorbell_mmap_offset = (u64)GPGPU_DOORBELL_MMAP_PAGE << PAGE_SHIFT;
	args->doorbell_map_size = PAGE_SIZE;
	return 0;
}

/* 按编号取未卸载的队列，并先读取它所在槽的 GPUVM 故障，供后续报告。 */
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

/* QUEUE_QUERY：返回状态、首次错误和最近一次管理结果。 */
int gpgpu_queue_query(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args)
{
	struct gpgpu_queue *q = queue_get(fpriv, args);

	if (!q)
		return -EINVAL;
	queue_report(q, args);
	return 0;
}

/*
 * queue_unmap_rejected() - CP 返回了有效结果，但没有卸载队列
 *
 * gpgpu_queue_destroy() 第 5 步调用，队列仍占着 HQD 槽，资源一律保留。
 * 等待管理结果时 dev->lock 曾被释放，IH 可能已经或即将把本队列置为
 * FAULTED，所以先消费旧 IH，再决定状态：
 *
 * 1. MQD_FAULT：CP 读 W 或保存 MQD 失败，已停止取包，队列为 FAULTED。
 *    再次卸载走出错队列的路径，不要求排空；
 * 2. 设备故障时同样为 FAULTED；
 * 3. 其余结果（RECOVERY_REQUIRED、INVALID 等）CP 没有改动队列，期间
 *    没有出错就恢复卸载前的状态。
 *
 * 例：驱动看到 R == W 后另一线程又发布了包，CP 看到 R != W，返回
 * RECOVERY_REQUIRED；队列恢复为 READY，调用者停止提交、排空后重试。
 *
 * Return: 恢复为 READY 的 RECOVERY_REQUIRED 返回 -EBUSY，其余为 -EIO。
 */
static int queue_unmap_rejected(struct gpgpu_queue *q, u32 previous, u32 status)
{
	struct gpgpu_device *dev = q->fpriv->dev;

	gpgpu_ih_drain(dev);

	/* 1. 2. CP 已停止取包或设备故障：队列保持出错。 */
	if (status == GPGPU_MANAGEMENT_MQD_FAULT || dev->fatal)
		q->state = GPGPU_QUEUE_FAULTED;
	/* 3. 等待期间 IH 已置 FAULTED 时不再覆盖。 */
	else if (q->state == GPGPU_QUEUE_UNLOADING)
		q->state = previous;

	if (status == GPGPU_MANAGEMENT_RECOVERY_REQUIRED && q->state == GPGPU_QUEUE_READY)
		return -EBUSY;
	return -EIO;
}

/*
 * gpgpu_queue_destroy() - 正常卸载一条队列
 *
 * 调用者持 vm_serial 与 dev->lock；第 2 步等待时会暂时释放 dev->lock。
 *
 * uncertain 只表示上一条管理命令的结果未知；拿到 CP 的有效结果后
 * 就清除，队列可以再次卸载。是否回收 Ring、进度和 VMID 另看 CP 是否
 * 已经放开队列，只有第 5 步的 OK 与 NOT_MAPPED 回收。
 *
 * 1. 上一次管理结果未知或正在卸载时返回 -EBUSY。READY 队列必须已排空
 *    （R == W），CP 卸载时不处理未消费的包；FAULTED 队列不要求排空；
 * 2. 先把队列置为 UNLOADING + uncertain，再经 HIQ 发送 UNMAP_QUEUES。
 *    等待期间文件关闭时，会话因这条队列保留为 orphan；
 * 3. 已发布却没有取得有效结果，如超时、通道故障、结果码非法或设备
 *    移除：CP 可能已经卸载，也可能仍在运行，保持 uncertain 与全部资源；
 * 4. 从未发布：CP 没有收到命令，恢复原状态；
 * 5. OK 或 NOT_MAPPED：CP 当前不再持有这条队列。NOT_MAPPED 只说明此刻
 *    没有绑定，不说明队列从未运行，因此和 OK 一样先消费旧 IH，让已
 *    发布的通知和错误都处理完，再放下 Ring 与进度的使用计数并标记
 *    dead，最后归还 VMID，队列对象留待 queue_collect() 回收。VMID 归还
 *    失败只让 VM 进入 failed，队列已经卸载，仍按成功返回。其余有效结果
 *    交给 queue_unmap_rejected()。
 *
 * 第 4 步和 queue_unmap_rejected() 恢复原状态时，等待期间 IH 已置为
 * FAULTED 的队列保持 FAULTED。
 */
int gpgpu_queue_destroy(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_queue *q = queue_get(fpriv, args);
	u32 status, previous;
	bool published;
	int ret;

	/* 1. 只有结果已知、非卸载中且已排空的队列才能卸载。 */
	if (!q)
		return -EINVAL;
	if (q->uncertain || q->state == GPGPU_QUEUE_UNLOADING)
		return -EBUSY;

	/* R 由 CP 写回 Host 内存；dma_rmb() 使读到的 R、W 不早于之前的读取。 */
	dma_rmb();
	if (q->state == GPGPU_QUEUE_READY && READ_ONCE(*q->rptr) != READ_ONCE(*q->wptr))
		return -EBUSY;

	/* 2. 记下原状态，从未发布或 CP 拒绝时可以恢复。 */
	previous = q->state;
	q->state = GPGPU_QUEUE_UNLOADING;
	q->uncertain = true;
	ret = gpgpu_mgmt_submit(dev, GPGPU_HQD_SLOT_HIQ, GPGPU_UNMAP_QUEUES, GPGPU_QUEUE_USER_AQL,
				q->mqd.addr, &status, &published);
	q->management_status = status;

	/* 3. 结果未知，不能判断 CP 是否还持有队列。 */
	if (published && ret)
		goto out;
	q->uncertain = false;

	/* 4. 从未发布；设备移除或故障时队列已被置为 FAULTED，不再覆盖。 */
	if (!published) {
		if (q->state == GPGPU_QUEUE_UNLOADING)
			q->state = dev->fatal ? GPGPU_QUEUE_FAULTED : previous;
		goto out;
	}

	/* 5. ret == 0 时设备仍在，可以访问寄存器。 */
	if (status != GPGPU_MANAGEMENT_OK && status != GPGPU_MANAGEMENT_NOT_MAPPED) {
		ret = queue_unmap_rejected(q, previous, status);
		goto out;
	}
	gpgpu_ih_drain(dev);
	q->ring->users--;
	q->progress->users--;
	q->dead = true;
	gpgpu_vm_detach(fpriv);

out:
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

	/* 映射为不可缓存的设备页：用户写入要立即到达设备。 */
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
