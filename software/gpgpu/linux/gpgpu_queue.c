// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "gpgpu_queue_drv.h"

/* BAR0 原生 CP 控制寄存器偏移。 */
#define CP_CTRL 0x0500
/* BAR0 原生 CP 复位入口寄存器偏移。 */
#define CP_RESET_VECTOR 0x0504
/* 管理 Ring 加一页 R/W 存储的总字节数。 */
#define MANAGEMENT_STORAGE (GPGPU_MGMT_BYTES + PAGE_SIZE)

/*
 * delete_request() - 释放一次管理请求的结果区
 *
 * 调用方持设备锁，并已按提交结果判断该结果区可以回收。
 */
static void delete_request(struct gpgpu_qdev *dev, struct gpgpu_management_request *request)
{
	dma_free_coherent(&dev->pdev->dev, sizeof(*request->result), request->result, request->dma);
	list_del(&request->node);
	kfree(request);
}

/*
 * management_submit() - 发布管理命令对并等待完成序号
 *
 * 入口及返回均持 dev->lock，发布时换 management_lock，等待时不持二者。
 * 发布后超时或通道故障保留结果区；成功读取的 status 交给调用方解释。
 */
static int management_submit(struct gpgpu_qdev *dev, unsigned int ring, u32 opcode, u32 kind,
			     dma_addr_t mqd, u32 *status, bool *published)
{
	struct gpgpu_management_ring *mq = &dev->management[ring];
	struct gpgpu_management_request *request;
	GpgpuManagementPacket *packets = mq->storage;
	unsigned long deadline = jiffies + msecs_to_jiffies(GPGPU_Q_TIMEOUT_MS);
	u32 w, r;
	u64 sequence;
	int ret = 0;

	*published = false;
	*status = GPGPU_MANAGEMENT_PENDING;
	request = kzalloc(sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;
	request->result = dma_alloc_coherent(&dev->pdev->dev, sizeof(*request->result),
					     &request->dma, GFP_KERNEL);
	if (!request->result) {
		kfree(request);
		return -ENOMEM;
	}
	memset(request->result, 0, sizeof(*request->result));
	request->result->status = cpu_to_le32(GPGPU_MANAGEMENT_PENDING);
	list_add_tail(&request->node, &dev->requests);
	mutex_unlock(&dev->lock);
	mutex_lock(&dev->management_lock);
	if (!READ_ONCE(dev->present) || READ_ONCE(dev->fatal) || READ_ONCE(mq->failed) ||
	    !dev->next_sequence) {
		ret = -EIO;
		goto unlock_publish;
	}
	w = le32_to_cpu(READ_ONCE(*mq->wptr));
	r = le32_to_cpu(READ_ONCE(*mq->rptr));
	dma_rmb();
	if ((u32)(w - r) > GPGPU_MGMT_BYTES / sizeof(*packets) - 2) {
		ret = -EBUSY;
		goto unlock_publish;
	}
	sequence = dev->next_sequence++;
	packets[w % (GPGPU_MGMT_BYTES / sizeof(*packets))] = (GpgpuManagementPacket) {
		.opcode = cpu_to_le32(opcode),
		.queue_kind = cpu_to_le32(kind),
		.address = cpu_to_le64(mqd),
	};
	packets[(w + 1) % (GPGPU_MGMT_BYTES / sizeof(*packets))] = (GpgpuManagementPacket) {
		.opcode = cpu_to_le32(GPGPU_WRITE_COMPLETION),
		.address = cpu_to_le64(request->dma),
		.value = cpu_to_le64(sequence),
	};
	dma_wmb();
	WRITE_ONCE(*mq->wptr, cpu_to_le32(w + 2));
	/* 共享 W 必须先于 Doorbell MMIO 写入对设备可见。 */
	wmb();
	writeq((u64)(u32)(w + 1), dev->doorbell + mq->doorbell);
	*published = true;
unlock_publish:
	mutex_unlock(&dev->management_lock);
	if (!ret) {
		while (le64_to_cpu(READ_ONCE(request->result->completion)) != sequence) {
			if (!READ_ONCE(dev->present) || READ_ONCE(dev->fatal) ||
			    READ_ONCE(mq->failed)) {
				ret = -EIO;
				break;
			}
			if (time_after_eq(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				break;
			}
			usleep_range(50, 100);
		}
		if (!ret) {
			dma_rmb();
			*status = le32_to_cpu(READ_ONCE(request->result->status));
		}
	}
	mutex_lock(&dev->lock);
	/*
	 * 发布后超时/通道故障，保留结果区，
	 * 禁止被新请求复用。
	 */
	if (!ret || !*published)
		delete_request(dev, request);
	/*
	 * 完成标记先到仍可能遇到移除；
	 * 重获锁后禁止调用者继续访问 BAR。
	 */
	if (!dev->present)
		ret = -ENODEV;
	return ret;
}

/*
 * management_allocate() - 为 KIQ 或 HIQ 分配共享对象
 *
 * 创建 Ring、独立 MQD 和附加进度页，填好 Host 地址及 Doorbell 配置。
 */
static int management_allocate(struct gpgpu_qdev *dev, unsigned int index)
{
	struct gpgpu_management_ring *mq = &dev->management[index];

	mq->storage = dma_alloc_coherent(&dev->pdev->dev, MANAGEMENT_STORAGE, &mq->dma, GFP_KERNEL);
	mq->mqd = dma_alloc_coherent(&dev->pdev->dev, sizeof(*mq->mqd), &mq->mqd_dma, GFP_KERNEL);
	if (!mq->storage || !mq->mqd)
		return -ENOMEM;
	memset(mq->storage, 0, MANAGEMENT_STORAGE);
	memset(mq->mqd, 0, sizeof(*mq->mqd));
	mq->rptr = mq->storage + GPGPU_MGMT_BYTES;
	mq->wptr = mq->storage + GPGPU_MGMT_BYTES + 8;
	mq->doorbell = index * 8;
	mq->mqd->ring_base = cpu_to_le64(mq->dma);
	mq->mqd->rptr_report_addr = cpu_to_le64(mq->dma + GPGPU_MGMT_BYTES);
	mq->mqd->wptr_poll_addr = cpu_to_le64(mq->dma + GPGPU_MGMT_BYTES + 8);
	mq->mqd->pq_control = cpu_to_le32(GPGPU_MGMT_BYTES);
	mq->mqd->doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | mq->doorbell);
	return 0;
}

/*
 * management_free() - 回收管理 Ring、进度页与 MQD
 *
 * 设备已停止访问这些对象后由销毁路径调用。
 */
static void management_free(struct gpgpu_qdev *dev, unsigned int index)
{
	struct gpgpu_management_ring *mq = &dev->management[index];

	if (mq->mqd)
		dma_free_coherent(&dev->pdev->dev, sizeof(*mq->mqd), mq->mqd, mq->mqd_dma);
	if (mq->storage)
		dma_free_coherent(&dev->pdev->dev, MANAGEMENT_STORAGE, mq->storage, mq->dma);
}

/*
 * delete_queue() - 删除 Host 队列对象及独占 MQD
 *
 * 持设备锁调用；未标记 dead 的对象同时放下 Ring 与进度引用。
 */
static void delete_queue(struct gpgpu_user_queue *q)
{
	struct gpgpu_qdev *dev = q->client->dev;

	if (!q->dead) {
		q->ring->users--;
		q->progress->users--;
	}
	dma_free_coherent(&dev->pdev->dev, sizeof(*q->mqd), q->mqd, q->mqd_dma);
	list_del(&q->node);
	kfree(q);
}

/*
 * client_delete() - 回收一个可安全销毁的会话
 *
 * 持设备锁按事件、队列、内存顺序释放，再归还 Doorbell 页。
 */
static void client_delete(struct gpgpu_qclient *client)
{
	struct gpgpu_user_queue *q, *next;

	gpgpu_event_release_all(client);
	list_for_each_entry_safe(q, next, &client->queues, node) {
		delete_queue(q);
	}
	gpgpu_memory_release_all(client);
	if (client->db_page)
		clear_bit(client->db_page, &client->dev->db_pages);
	list_del(&client->node);
	kfree(client);
}

/*
 * qdev_destroy() - 释放最后一个队列设备引用
 *
 * 收回管理结果区、管理 Ring、IH 和 buddy，最后放下 PCI 引用。
 */
static void qdev_destroy(struct kref *refs)
{
	struct gpgpu_qdev *dev = container_of(refs, struct gpgpu_qdev, refs);
	struct gpgpu_management_request *r, *next;

	list_for_each_entry_safe(r, next, &dev->requests, node) {
		delete_request(dev, r);
	}
	management_free(dev, 0);
	management_free(dev, 1);
	if (dev->ih)
		dma_free_coherent(&dev->pdev->dev, GPGPU_IH_BYTES, dev->ih, dev->ih_dma);
	gpgpu_memory_fini(dev);
	pci_dev_put(dev->pdev);
	kfree(dev);
}

/*
 * gpgpu_qdev_create() - 建立队列模式设备上下文
 *
 * 要求无系统 IOMMU、缓存一致性 RAM 与冷启动 CP，再分配管理及 IH 资源。
 */
struct gpgpu_qdev *gpgpu_qdev_create(struct pci_dev *pdev, void __iomem *regs,
				     void __iomem *doorbell)
{
	struct gpgpu_qdev *dev;
	int ret;
	/*
	 * 第二阶段使用直通物理地址
	 * 及可原子读改写的缓存一致性 RAM。
	 */
	if (iommu_get_domain_for_dev(&pdev->dev) || !dev_is_dma_coherent(&pdev->dev) ||
	    readl(regs + CP_CTRL) != 3 ||
	    readl(regs + GPGPU_REG_VRAM_SIZE_LO) != GPGPU_FIRMWARE_STACK_TOP ||
	    PAGE_SIZE > GPGPU_QUEUE_DOORBELL_SIZE / 3) {
		return ERR_PTR(-EOPNOTSUPP);
	}
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	dev->pdev = pci_dev_get(pdev);
	dev->regs = regs;
	dev->doorbell = doorbell;
	kref_init(&dev->refs);
	mutex_init(&dev->lock);
	mutex_init(&dev->management_lock);
	mutex_init(&dev->dma_lock);
	spin_lock_init(&dev->irq_lock);
	init_completion(&dev->dma_done);
	INIT_WORK(&dev->ih_work, gpgpu_ih_work);
	INIT_LIST_HEAD(&dev->clients);
	INIT_LIST_HEAD(&dev->requests);
	INIT_LIST_HEAD(&dev->reserved_blocks);
	dev->next_sequence = 1;
	dev->db_pages = 1; /* 第0页只供内核管理队列使用。 */
	dev->present = true;
	ret = gpgpu_memory_init(dev);
	if (ret)
		goto fail;
	dev->vram = pci_iomap(pdev, 2, 0);
	if (!dev->vram) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = management_allocate(dev, 0);
	if (!ret)
		ret = management_allocate(dev, 1);
	if (ret)
		goto fail;
	dev->ih = dma_alloc_coherent(&pdev->dev, GPGPU_IH_BYTES, &dev->ih_dma, GFP_KERNEL);
	if (!dev->ih) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(dev->ih, 0, GPGPU_IH_BYTES);
	return dev;
fail:
	if (dev->vram)
		pci_iounmap(pdev, dev->vram);
	kref_put(&dev->refs, qdev_destroy);
	return ERR_PTR(ret);
}

/*
 * gpgpu_qdev_start() - 加载固件并建立 KIQ 到 HIQ 的管理路径
 *
 * 保持 CP 复位时配置固件、IH 和 KIQ，启动后通过 KIQ 请求映射 HIQ。
 */
int gpgpu_qdev_start(struct gpgpu_qdev *dev)
{
#if IS_REACHABLE(CONFIG_GPU_BUDDY)
	struct gpgpu_management_ring *kiq = &dev->management[0];
	const struct firmware *fw;
	u32 status;
	bool published;
	int ret = request_firmware(&fw, "gpgpu-cp.bin", &dev->pdev->dev);

	if (ret)
		return ret;
	if (!fw->size || fw->size > GPGPU_FIRMWARE_STACK_BASE - GPGPU_FIRMWARE_BASE) {
		release_firmware(fw);
		return -EINVAL;
	}
	writel(1, dev->regs + CP_CTRL);
	readl(dev->regs + CP_CTRL);
	memcpy_toio(dev->vram + GPGPU_FIRMWARE_BASE, fw->data, fw->size);
	release_firmware(fw);
	writel(GPGPU_FIRMWARE_BASE, dev->regs + CP_RESET_VECTOR);
	gpgpu_qwrite64(dev, GPGPU_REG_IH_BASE_LO, dev->ih_dma);
	writel(GPGPU_IH_BYTES, dev->regs + GPGPU_REG_IH_SIZE);
	writel(GPGPU_IH_ENABLE, dev->regs + GPGPU_REG_IH_CTRL);
	writel(0, dev->regs + GPGPU_REG_HQD_SELECT);
	if (readl(dev->regs + GPGPU_REG_HQD_SELECT))
		return -EIO;
/* 冷启动时向 Host 当前选中的 HQD 槽写入一个字段。 */
#define HQD_WRITE(field, value) writel((value), dev->regs + GPGPU_REG_HQD_WINDOW + (field))
	gpgpu_qwrite64(dev, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_LO, kiq->mqd_dma);
	gpgpu_qwrite64(dev, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_RING_LO, kiq->dma);
	gpgpu_qwrite64(dev, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_RPTR_ADDR_LO,
		       kiq->dma + GPGPU_MGMT_BYTES);
	gpgpu_qwrite64(dev, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_WPTR_ADDR_LO,
		       kiq->dma + GPGPU_MGMT_BYTES + 8);
	HQD_WRITE(GPGPU_HQD_CONTROL, GPGPU_MGMT_BYTES);
	HQD_WRITE(GPGPU_HQD_DOORBELL, GPGPU_DOORBELL_ENABLE);
	HQD_WRITE(GPGPU_HQD_VMID, 0);
	HQD_WRITE(GPGPU_HQD_AQL, 0);
	HQD_WRITE(GPGPU_HQD_RPTR, 0);
	dma_wmb();
	HQD_WRITE(GPGPU_HQD_ACTIVE, 1);
	readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ACTIVE);
#undef HQD_WRITE
	writel(GPGPU_Q_IRQ_ALL, dev->regs + GPGPU_REG_IRQ_ENABLE);
	/* 固件、IH 和 KIQ 配置必须先于解除 CP 复位完成。 */
	wmb();
	dev->started = true;
	writel(0, dev->regs + CP_CTRL);
	mutex_lock(&dev->lock);
	ret = management_submit(dev, 0, GPGPU_MAP_QUEUES, GPGPU_QUEUE_HIQ,
				dev->management[1].mqd_dma, &status, &published);
	mutex_unlock(&dev->lock);
	return ret ? ret : (status ? -EIO : 0);
#else
	/*
	 * 原发行版内核只运行同步模式，
	 * 不引入固件加载模块依赖。
	 */
	return -EOPNOTSUPP;
#endif
}

/*
 * gpgpu_qdev_stop() - 在移除或启动失败时停止设备访问
 *
 * 先禁止新请求，再串行结束管理与 DMA，复位并等待 IRQ/工作项结束。
 * 只有此后才回收 orphan；会话关闭和普通等待超时不调用本入口。
 */
void gpgpu_qdev_stop(struct gpgpu_qdev *dev)
{
	struct gpgpu_qclient *c, *next;

	mutex_lock(&dev->lock);
	WRITE_ONCE(dev->present, false);
	gpgpu_events_fault(dev, NULL, GPGPU_QUEUE_ERR_FIRMWARE);
	mutex_unlock(&dev->lock);
	mutex_lock(&dev->management_lock);
	mutex_lock(&dev->dma_lock);
	writel(0, dev->regs + GPGPU_REG_IRQ_ENABLE);
	writel(GPGPU_CTRL_RESET, dev->regs + GPGPU_REG_GLOBAL_CTRL);
	readl(dev->regs + GPGPU_REG_GLOBAL_CTRL);
	pci_clear_master(dev->pdev);
	mutex_unlock(&dev->dma_lock);
	mutex_unlock(&dev->management_lock);
	for (unsigned int i = 0; i < 4; i++)
		synchronize_irq(pci_irq_vector(dev->pdev, i));
	cancel_work_sync(&dev->ih_work);
	mutex_lock(&dev->lock);
	/*
	 * 复位和异步回调均已结束，
	 * close 才可释放仍有设备引用的对象。
	 */
	dev->stopped = true;
	list_for_each_entry_safe(c, next, &dev->clients, node) {
		if (c->orphan) {
			client_delete(c);
			kref_put(&dev->refs, qdev_destroy);
		}
	}
	mutex_unlock(&dev->lock);
	pci_iounmap(dev->pdev, dev->vram);
	dev->vram = NULL;
}

/*
 * gpgpu_qdev_put() - 放下 PCI 入口持有的队列设备引用
 *
 * 调用方先 free_irq，避免中断仍访问已经销毁的状态。
 */
void gpgpu_qdev_put(struct gpgpu_qdev *dev)
{
	kref_put(&dev->refs, qdev_destroy);
}

/*
 * gpgpu_qopen() - 创建一次打开对应的队列会话
 *
 * 登记进程归属、对象链表与句柄起点，并持有设备引用。
 */
int gpgpu_qopen(struct gpgpu_qdev *dev, struct file *file)
{
	struct gpgpu_qclient *c;

	mutex_lock(&dev->lock);
	if (!dev->present || dev->fatal) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		mutex_unlock(&dev->lock);
		return -ENOMEM;
	}
	c->dev = dev;
	c->owner = task_tgid_nr(current);
	c->next_memory = c->next_queue = c->next_event = 1;
	INIT_LIST_HEAD(&c->memory);
	INIT_LIST_HEAD(&c->queues);
	INIT_LIST_HEAD(&c->events);
	atomic_set(&c->db_maps, 0);
	list_add_tail(&c->node, &dev->clients);
	kref_get(&dev->refs);
	file->private_data = c;
	mutex_unlock(&dev->lock);
	return nonseekable_open(NULL, file);
}

/*
 * gpgpu_qrelease() - 结束文件对应的队列会话
 *
 * 仍可能存在设备访问时保留整个 orphan 上下文，停机后才允许回收。
 */
int gpgpu_qrelease(struct inode *inode, struct file *file)
{
	struct gpgpu_qclient *c = file->private_data;
	struct gpgpu_qdev *dev = c->dev;
	struct gpgpu_user_queue *q;
	bool retain;

	mutex_lock(&dev->lock);
	retain = dev->dma_uncertain;
	list_for_each_entry(q, &c->queues, node) {
		retain |= !q->dead;
	}
	if (retain && !dev->stopped) {
		/*
		 * file/VMA 引用结束仍可能有设备访问，
		 * 保留整个上下文直到设备移除。
		 */
		c->orphan = true;
		mutex_unlock(&dev->lock);
		return 0;
	}
	client_delete(c);
	mutex_unlock(&dev->lock);
	kref_put(&dev->refs, qdev_destroy);
	return 0;
}

/*
 * gpgpu_queue_find() - 按队列编号查找会话对象
 *
 * 调用方持 dev->lock，返回对象可能已经卸载，需继续检查 dead。
 */
struct gpgpu_user_queue *gpgpu_queue_find(struct gpgpu_qclient *client, u32 id)
{
	struct gpgpu_user_queue *q;

	list_for_each_entry(q, &client->queues, node) {
		if (q->id == id)
			return q;
	}
	return NULL;
}

/*
 * queue_collect() - 回收已卸载且无旧通知引用的队列
 *
 * 持设备锁；仍有 Doorbell 映射或事件时保留 MQD 与入口，
 * 防止旧通知串队列。
 */
static void queue_collect(struct gpgpu_qclient *c)
{
	struct gpgpu_user_queue *q, *next;

	if (atomic_read(&c->db_maps))
		return;
	list_for_each_entry_safe(q, next, &c->queues, node) {
		struct gpgpu_qevent *e;
		bool used = false;

		list_for_each_entry(e, &c->events, node) {
			used |= e->queue == q;
		}
		if (q->dead && !used)
			delete_queue(q);
	}
}

/*
 * overlap() - 判断两个非溢出的字节区间是否相交
 *
 * 调用者先验证两个范围，随后比较半开区间边界。
 */
static bool overlap(u64 a, u64 asize, u64 b, u64 bsize)
{
	return a < b + bsize && b < a + asize;
}

/*
 * queue_create() - 为已有 Host 内存建立用户队列
 *
 * 校验 Ring/进度独占与对齐，创建 MQD 并请求 HIQ 映射，成功后交付 Doorbell。
 */
static int queue_create(struct gpgpu_qclient *c, struct gpgpu_queue_create *r)
{
	struct gpgpu_qdev *dev = c->dev;
	struct gpgpu_qmemory *ring = gpgpu_memory_find(c, r->ring_handle);
	struct gpgpu_qmemory *p = gpgpu_memory_find(c, r->progress_handle);
	struct gpgpu_user_queue *q, *other;
	unsigned long used = 0;
	bool published;
	u32 status, initial;
	int ret;

	queue_collect(c);
	if (r->reserved || !ring || !p || ring->kind != GPGPU_MEMORY_HOST ||
	    p->kind != GPGPU_MEMORY_HOST || !is_power_of_2(r->ring_size) ||
	    r->ring_size < GPGPU_QUEUE_RING_MIN || r->ring_size > GPGPU_QUEUE_RING_MAX ||
	    (r->ring_offset & 63) || (r->rptr_offset & 7) || (r->wptr_offset & 7) ||
	    !gpgpu_memory_range(ring, r->ring_offset, r->ring_size) ||
	    !gpgpu_memory_range(p, r->rptr_offset, 4) ||
	    !gpgpu_memory_range(p, r->wptr_offset, 4) || r->rptr_offset == r->wptr_offset ||
	    !c->next_queue ||
	    (ring == p &&
	     (overlap(r->ring_offset, r->ring_size, r->rptr_offset, 4) ||
	      overlap(r->ring_offset, r->ring_size, r->wptr_offset, 4)))) {
		return -EINVAL;
	}
	/*
	 * 同一存储不能同时归属两个队列，
	 * 防止一个生产者覆盖另一个消费状态。
	 */
	list_for_each_entry(other, &c->queues, node) {
		used |= BIT(other->db_index);
		if (!other->dead &&
		    (other->ring == ring || other->progress == p || other->ring == p ||
		     other->progress == ring)) {
			return -EBUSY;
		}
	}
	if ((used & 3) == 3)
		return -ENOSPC;
	if (!c->db_page) {
		unsigned int page =
			find_first_zero_bit(&dev->db_pages, GPGPU_QUEUE_DOORBELL_SIZE / PAGE_SIZE);

		if (page >= GPGPU_QUEUE_DOORBELL_SIZE / PAGE_SIZE)
			return -ENOSPC;
		set_bit(page, &dev->db_pages);
		c->db_page = page;
	}
	initial = le32_to_cpu(READ_ONCE(*(u32 *)(p->cpu + r->rptr_offset)));
	if (initial != le32_to_cpu(READ_ONCE(*(u32 *)(p->cpu + r->wptr_offset))))
		return -EBUSY;
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->mqd = dma_alloc_coherent(&dev->pdev->dev, sizeof(*q->mqd), &q->mqd_dma, GFP_KERNEL);
	if (!q->mqd) {
		kfree(q);
		return -ENOMEM;
	}
	q->client = c;
	q->ring = ring;
	q->progress = p;
	q->rptr = p->cpu + r->rptr_offset;
	q->wptr = p->cpu + r->wptr_offset;
	q->db_index = (used & 1) ? 1 : 0;
	q->id = c->next_queue++;
	q->state = GPGPU_QUEUE_UNLOADING;
	q->uncertain = true;
	ring->users++;
	p->users++;
	*q->mqd = (GpgpuMqd) {
		.ring_base = cpu_to_le64(ring->dma + r->ring_offset),
		.rptr_report_addr = cpu_to_le64(p->dma + r->rptr_offset),
		.wptr_poll_addr = cpu_to_le64(p->dma + r->wptr_offset),
		.pq_control = cpu_to_le32(r->ring_size),
		.doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE |
						(c->db_page * PAGE_SIZE + q->db_index * 8)),
		.aql_control = cpu_to_le32(1),
		.saved_rptr = cpu_to_le64(initial),
	};
	for (u32 i = 0; i < r->ring_size; i += sizeof(GpgpuAqlPacket)) {
		WRITE_ONCE(*(u32 *)(ring->cpu + r->ring_offset + i),
			   cpu_to_le32(GPGPU_AQL_INVALID));
	}
	list_add_tail(&q->node, &c->queues);
	ret = management_submit(dev, 1, GPGPU_MAP_QUEUES, GPGPU_QUEUE_USER_AQL, q->mqd_dma, &status,
				&published);
	q->management_status = status;
	if (ret || status) {
		if (!published ||
		    (!ret && status != GPGPU_MANAGEMENT_MQD_FAULT &&
		     status != GPGPU_MANAGEMENT_RECOVERY_REQUIRED)) {
			delete_queue(q);
		} else {
			q->state = GPGPU_QUEUE_FAULTED;
		}
		return ret ? ret : (status == GPGPU_MANAGEMENT_NO_SLOT ? -ENOSPC : -EIO);
	}
	if (dev->fatal) {
		q->state = GPGPU_QUEUE_FAULTED;
		return -EIO;
	}
	q->uncertain = false;
	q->state = GPGPU_QUEUE_READY;
	r->queue_id = q->id;
	r->doorbell_offset = q->db_index * 8;
	r->doorbell_mmap_offset = (u64)GPGPU_DB_MMAP_PAGE << PAGE_SHIFT;
	r->doorbell_map_size = PAGE_SIZE;
	return 0;
}

/*
 * queue_operation() - 查询队列或提交正常卸载
 *
 * 持设备锁检查排空条件，CP 确认卸载后消费旧 IH 并放下内存引用。
 */
static int queue_operation(struct gpgpu_qclient *c, unsigned int command,
			   struct gpgpu_queue_state *r)
{
	struct gpgpu_qdev *dev = c->dev;
	struct gpgpu_user_queue *q = gpgpu_queue_find(c, r->queue_id);
	bool published;
	u32 status, previous_state;
	int ret = 0;

	if (!q || q->dead || r->reserved)
		return -EINVAL;
	if (command == GPGPU_IOC_QUEUE_DESTROY) {
		if (q->uncertain || q->state == GPGPU_QUEUE_UNLOADING)
			return -EBUSY;
		dma_rmb();
		if (q->state == GPGPU_QUEUE_READY && READ_ONCE(*q->rptr) != READ_ONCE(*q->wptr))
			return -EBUSY;
		previous_state = q->state;
		q->state = GPGPU_QUEUE_UNLOADING;
		q->uncertain = true;
		ret = management_submit(dev, 1, GPGPU_UNMAP_QUEUES, GPGPU_QUEUE_USER_AQL,
					q->mqd_dma, &status, &published);
		q->management_status = status;
		if (!ret && !status) {
			gpgpu_ih_drain_locked(dev);
			q->ring->users--;
			q->progress->users--;
			q->dead = true;
			q->uncertain = false;
		} else if (!published) {
			q->uncertain = false;
			q->state = dev->fatal ? GPGPU_QUEUE_FAULTED : previous_state;
		} else if (!ret) {
			q->state = GPGPU_QUEUE_FAULTED;
			ret = -EIO;
		}
	}
	r->state = q->state;
	r->error = q->error;
	r->management_status = q->management_status;
	return ret;
}

/*
 * memory_vma_open() - 登记一个 Host 内存映射引用
 *
 * 原子增加 VMA 数，防止用户映射仍在时释放 DMA 内存。
 */
static void memory_vma_open(struct vm_area_struct *vma)
{
	struct gpgpu_qmemory *m = vma->vm_private_data;

	atomic_inc(&m->maps);
}

/*
 * memory_vma_close() - 放下一个 Host 内存映射引用
 *
 * 这里只减少计数，实际释放由对象生命周期路径负责。
 */
static void memory_vma_close(struct vm_area_struct *vma)
{
	struct gpgpu_qmemory *m = vma->vm_private_data;

	atomic_dec(&m->maps);
}
static const struct vm_operations_struct memory_vm_ops = {
	.open = memory_vma_open,
	.close = memory_vma_close,
};

/*
 * doorbell_vma_open() - 登记会话 Doorbell 页的映射引用
 *
 * 原子增加计数，旧映射存活时不得复用页内入口。
 */
static void doorbell_vma_open(struct vm_area_struct *vma)
{
	struct gpgpu_qclient *c = vma->vm_private_data;

	atomic_inc(&c->db_maps);
}

/*
 * doorbell_vma_close() - 放下 Doorbell 页的映射引用
 *
 * 最后一个映射退出后，后续队列收集才可回收旧入口。
 */
static void doorbell_vma_close(struct vm_area_struct *vma)
{
	struct gpgpu_qclient *c = vma->vm_private_data;

	atomic_dec(&c->db_maps);
}
static const struct vm_operations_struct doorbell_vm_ops = {
	.open = doorbell_vma_open,
	.close = doorbell_vma_close,
};

/*
 * gpgpu_qmmap() - 建立队列内存或 Doorbell 用户映射
 *
 * 核对进程与共享权限，按偏移令牌选择一致性 RAM 或 MMIO，并登记 VMA
 * 引用。
 */
int gpgpu_qmmap(struct file *file, struct vm_area_struct *vma)
{
	struct gpgpu_qclient *c = file->private_data;
	struct gpgpu_qdev *dev = c->dev;
	struct gpgpu_qmemory *m;
	unsigned long token = vma->vm_pgoff;
	int ret = -EINVAL;

	if (task_tgid_nr(current) != c->owner)
		return -EOPNOTSUPP;
	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	mutex_lock(&dev->lock);
	if (!dev->present) {
		ret = -ENODEV;
		goto out;
	}
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vm_flags_clear(vma, VM_MAYEXEC);
	if (token == GPGPU_DB_MMAP_PAGE) {
		if (!c->db_page || vma->vm_end - vma->vm_start != PAGE_SIZE)
			goto out;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		ret = io_remap_pfn_range(vma, vma->vm_start,
					 (pci_resource_start(dev->pdev, 4) >> PAGE_SHIFT) +
						 c->db_page,
					 PAGE_SIZE, vma->vm_page_prot);
		if (!ret) {
			vma->vm_private_data = c;
			vma->vm_ops = &doorbell_vm_ops;
			doorbell_vma_open(vma);
		}
	} else {
		m = token <= U32_MAX ? gpgpu_memory_find(c, token) : NULL;
		if (!m || m->kind != GPGPU_MEMORY_HOST || vma->vm_end - vma->vm_start != m->size)
			goto out;
		vma->vm_pgoff = 0;
		ret = dma_mmap_coherent(&dev->pdev->dev, vma, m->cpu, m->dma, m->size);
		if (!ret) {
			vma->vm_private_data = m;
			vma->vm_ops = &memory_vm_ops;
			memory_vma_open(vma);
		}
	}
out:
	mutex_unlock(&dev->lock);
	return ret;
}

/*
 * gpgpu_qioctl() - 校验并分发队列模式 ioctl
 *
 * 按命令确定固定请求大小，
 * 校验版本后持设备锁执行对象操作并返回诊断。
 */
long gpgpu_qioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct gpgpu_qclient *c = file->private_data;
	struct gpgpu_qdev *dev = c->dev;
	void __user *arg = (void __user *)argument;
	union {
		struct gpgpu_queue_info info;	    /* 能力查询请求。 */
		struct gpgpu_memory_request memory; /* 内存申请请求。 */
		struct gpgpu_object_request object; /* 按句柄释放对象。 */
		struct gpgpu_memory_copy copy;	    /* 对象间 DMA 请求。 */
		struct gpgpu_queue_create queue;    /* 队列创建请求。 */
		struct gpgpu_queue_state state;	    /* 队列查询或卸载结果。 */
		struct gpgpu_event_request event;   /* 事件生命周期或等待请求。 */
	} r;
	size_t size;
	int ret;

	if (task_tgid_nr(current) != c->owner)
		return -EOPNOTSUPP;
	switch (command) {
	case GPGPU_IOC_QINFO:
		size = sizeof(r.info);
		break;
	case GPGPU_IOC_MEMORY_ALLOC:
		size = sizeof(r.memory);
		break;
	case GPGPU_IOC_MEMORY_FREE:
		size = sizeof(r.object);
		break;
	case GPGPU_IOC_MEMORY_COPY:
		size = sizeof(r.copy);
		break;
	case GPGPU_IOC_QUEUE_CREATE:
		size = sizeof(r.queue);
		break;
	case GPGPU_IOC_QUEUE_QUERY:
	case GPGPU_IOC_QUEUE_DESTROY:
		size = sizeof(r.state);
		break;
	case GPGPU_IOC_EVENT_CREATE:
	case GPGPU_IOC_EVENT_QUERY:
	case GPGPU_IOC_EVENT_WAIT:
	case GPGPU_IOC_EVENT_DESTROY:
		size = sizeof(r.event);
		break;
	default:
		return -ENOTTY;
	}
	memset(&r, 0, sizeof(r));
	if (copy_from_user(&r, arg, size))
		return -EFAULT;
	if (r.object.version != GPGPU_QABI_VERSION)
		return -EPROTONOSUPPORT;
	mutex_lock(&dev->lock);
	if (!dev->present) {
		ret = -ENODEV;
		goto out;
	}
	switch (command) {
	case GPGPU_IOC_QINFO:
		if (r.info.flags) {
			ret = -EINVAL;
			break;
		}
		r.info = (struct gpgpu_queue_info) {
			.version = GPGPU_QABI_VERSION,
			.vram_size = GPGPU_FIRMWARE_STACK_TOP,
			.vram_usable = GPGPU_VRAM_USABLE_SIZE,
			.firmware_base = GPGPU_FIRMWARE_BASE,
			.firmware_size = GPGPU_FIRMWARE_SIZE,
			.reserved_page_size = PAGE_SIZE,
			.max_queues = GPGPU_USER_QUEUE_MAX,
			.packet_size = sizeof(GpgpuAqlPacket),
			.ring_min = GPGPU_QUEUE_RING_MIN,
			.ring_max = GPGPU_QUEUE_RING_MAX,
			.host_address_bits = 64,
			.progress_bits = 32,
			.capabilities = 1,
		};
		ret = 0;
		break;
	case GPGPU_IOC_MEMORY_ALLOC:
		ret = gpgpu_memory_alloc(c, &r.memory);
		break;
	case GPGPU_IOC_MEMORY_FREE:
		ret = gpgpu_memory_free(c, r.object.handle);
		break;
	case GPGPU_IOC_MEMORY_COPY:
		ret = gpgpu_memory_copy(c, &r.copy);
		break;
	case GPGPU_IOC_QUEUE_CREATE:
		ret = queue_create(c, &r.queue);
		break;
	case GPGPU_IOC_QUEUE_QUERY:
	case GPGPU_IOC_QUEUE_DESTROY:
		ret = queue_operation(c, command, &r.state);
		break;
	default:
		ret = gpgpu_event_operation(c, command, &r.event);
		break;
	}
	/*
	 * 故障返回也携带已经保存的诊断；
	 * 没有副作用的等待可安全重试。
	 */
	if ((_IOC_DIR(command) & _IOC_READ) && copy_to_user(arg, &r, size)) {
		if (!ret && command == GPGPU_IOC_MEMORY_ALLOC)
			gpgpu_memory_free(c, r.memory.handle);
		ret = -EFAULT;
	}
out:
	mutex_unlock(&dev->lock);
	return ret;
}
