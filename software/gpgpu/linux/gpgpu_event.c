// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/io.h>
#include <linux/slab.h>
#include "gpgpu_queue_drv.h"

/*
 * gpgpu_events_fault() - 向受影响队列及等待者发布失败
 *
 * 调用方持 dev->lock；先保存首次错误与事件状态，再唤醒全部相关等待者。
 */
void gpgpu_events_fault(struct gpgpu_qdev *dev, struct gpgpu_user_queue *queue, u32 error)
{
	struct gpgpu_qclient *c;
	struct gpgpu_qevent *e;
	struct gpgpu_user_queue *q;

	list_for_each_entry(c, &dev->clients, node) {
		list_for_each_entry(q, &c->queues, node) {
			if (!q->dead && (!queue || q == queue)) {
				if (queue && !q->error)
					q->error = error;
				q->state = GPGPU_QUEUE_FAULTED;
			}
		}
		list_for_each_entry(e, &c->events, node) {
			if ((!queue || e->queue == queue) && e->state == GPGPU_EVENT_PENDING) {
				e->error = error;
				/* 条件先保存，wait_event 登记后会再次检查。 */
				WRITE_ONCE(e->state, GPGPU_EVENT_FAILED);
				wake_up_all(&e->wait);
			}
		}
	}
}

/*
 * find_mqd() - 按完整 MQD 地址查找用户队列
 *
 * 调用方持 dev->lock，在所有会话中匹配 IH 记录的来源。
 */
static struct gpgpu_user_queue *find_mqd(struct gpgpu_qdev *dev, u64 mqd)
{
	struct gpgpu_qclient *c;
	struct gpgpu_user_queue *q;

	list_for_each_entry(c, &dev->clients, node) {
		list_for_each_entry(q, &c->queues, node) {
			if (q->mqd_dma == mqd)
				return q;
		}
	}
	return NULL;
}

/*
 * gpgpu_ih_drain_locked() - 消费当前已发布的 IH 记录
 *
 * 调用方持 dev->lock；读 W 后同步记录，保存事件再清通知槽，最后归还 R。
 */
void gpgpu_ih_drain_locked(struct gpgpu_qdev *dev)
{
	u32 wptr = readl(dev->regs + GPGPU_REG_IH_WPTR);

	if ((u32)(wptr - dev->ih_rptr) > GPGPU_IH_BYTES / sizeof(*dev->ih)) {
		WRITE_ONCE(dev->fatal, true);
		gpgpu_events_fault(dev, NULL, GPGPU_QUEUE_ERR_IH);
		return;
	}
	/*
	 * WPTR 发布在记录之后，
	 * 读到编号后才能读取对应记录。
	 */
	dma_rmb();
	while (dev->ih_rptr != wptr) {
		GpgpuIhRecord *r = &dev->ih[dev->ih_rptr % (GPGPU_IH_BYTES / sizeof(*dev->ih))];
		u32 type = le32_to_cpu(r->type), data = le32_to_cpu(r->data);
		u64 mqd = le64_to_cpu(r->mqd_addr);
		struct gpgpu_user_queue *q = find_mqd(dev, mqd);

		if (type == GPGPU_IH_SIGNAL_NOTIFY && q) {
			struct gpgpu_qevent *e;

			list_for_each_entry(e, &q->client->events, node) {
				if (e->queue == q && e->id == data &&
				    le64_to_cpu(READ_ONCE(*e->mailbox)) == data) {
					if (e->state == GPGPU_EVENT_PENDING)
						WRITE_ONCE(e->state, GPGPU_EVENT_NOTIFIED);
					/*
					 * 状态保留，通知槽清空后再唤醒；
					 * 等待不消费状态。
					 */
					WRITE_ONCE(*e->mailbox, cpu_to_le64(0));
					dma_wmb();
					wake_up_all(&e->wait);
					break;
				}
			}
		} else if (type == GPGPU_IH_QUEUE_ERROR) {
			if (q) {
				gpgpu_events_fault(dev, q, data);
			} else if (mqd == dev->management[1].mqd_dma) {
				WRITE_ONCE(dev->management[1].failed, true);
			} else if (mqd == dev->management[0].mqd_dma) {
				WRITE_ONCE(dev->management[0].failed, true);
				WRITE_ONCE(dev->fatal, true);
				gpgpu_events_fault(dev, NULL, data);
			}
		} else if (type == GPGPU_IH_DEVICE_ERROR) {
			WRITE_ONCE(dev->fatal, true);
			gpgpu_events_fault(dev, NULL, data);
		}
		dev->ih_rptr++;
	}
	/* 完成记录读取及事件更新后，才把槽位归还设备。 */
	dma_mb();
	writel(dev->ih_rptr, dev->regs + GPGPU_REG_IH_RPTR);
}

/*
 * gpgpu_ih_work() - 在工作队列中处理 IH 通知
 *
 * 取得设备锁消费记录，随后在中断锁下 ACK 并重新使能 IH 通知。
 */
void gpgpu_ih_work(struct work_struct *work)
{
	struct gpgpu_qdev *dev = container_of(work, struct gpgpu_qdev, ih_work);
	unsigned long flags;

	mutex_lock(&dev->lock);
	if (dev->present) {
		gpgpu_ih_drain_locked(dev);
		if (dev->fatal)
			gpgpu_events_fault(dev, NULL, GPGPU_QUEUE_ERR_IH);
		spin_lock_irqsave(&dev->irq_lock, flags);
		writel(GPGPU_QUEUE_IRQ_IH, dev->regs + GPGPU_REG_IRQ_ACK);
		writel(GPGPU_Q_IRQ_ALL, dev->regs + GPGPU_REG_IRQ_ENABLE);
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	mutex_unlock(&dev->lock);
}

/*
 * gpgpu_qdev_irq() - 处理队列模式的中断上半部
 *
 * 中断锁下确认 DMA/错误，IH 先屏蔽再交给工作线程，避免反复触发。
 */
irqreturn_t gpgpu_qdev_irq(struct gpgpu_qdev *dev)
{
	unsigned long flags;
	u32 pending, error;

	spin_lock_irqsave(&dev->irq_lock, flags);
	if (!READ_ONCE(dev->present)) {
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		return IRQ_NONE;
	}
	pending = readl(dev->regs + GPGPU_REG_IRQ_STATUS) & GPGPU_Q_IRQ_ALL;
	if (!pending) {
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		return IRQ_NONE;
	}
	if (pending & GPGPU_IRQ_ERROR) {
		error = readl(dev->regs + GPGPU_REG_ERROR_STATUS);
		if (error & (1U << 3)) {
			WRITE_ONCE(dev->dma_failed, true);
			complete(&dev->dma_done);
		}
		if (error & (GPGPU_QUEUE_ERR_IH | GPGPU_QUEUE_ERR_FIRMWARE)) {
			WRITE_ONCE(dev->fatal, true);
			schedule_work(&dev->ih_work);
		}
	}
	if (pending & GPGPU_IRQ_DMA)
		complete(&dev->dma_done);
	if (pending & GPGPU_QUEUE_IRQ_IH) {
		/*
		 * 工作线程消费后再 ACK 并恢复使能，
		 * 避免未消费记录反复中断。
		 */
		writel(GPGPU_Q_IRQ_ALL & ~GPGPU_QUEUE_IRQ_IH, dev->regs + GPGPU_REG_IRQ_ENABLE);
		schedule_work(&dev->ih_work);
	}
	writel(pending & ~GPGPU_QUEUE_IRQ_IH, dev->regs + GPGPU_REG_IRQ_ACK);
	spin_unlock_irqrestore(&dev->irq_lock, flags);
	return IRQ_HANDLED;
}

/*
 * find_event() - 查找会话内的事件编号
 *
 * 调用方持 dev->lock，未找到时返回 NULL。
 */
static struct gpgpu_qevent *find_event(struct gpgpu_qclient *client, u32 id)
{
	struct gpgpu_qevent *e;

	list_for_each_entry(e, &client->events, node) {
		if (e->id == id)
			return e;
	}
	return NULL;
}

/*
 * delete_event() - 释放事件的通知槽与链表节点
 *
 * 调用方已确认没有等待者及后续设备通知，并持有设备锁。
 */
static void delete_event(struct gpgpu_qclient *client, struct gpgpu_qevent *e)
{
	dma_free_coherent(&client->dev->pdev->dev, sizeof(*e->mailbox), e->mailbox, e->mailbox_dma);
	list_del(&e->node);
	kfree(e);
}

/*
 * gpgpu_event_operation() - 执行事件创建、查询、等待或销毁
 *
 * 入口持 dev->lock；等待期间释放锁并保持 waiters 计数，返回前重新持锁。
 */
int gpgpu_event_operation(struct gpgpu_qclient *client, unsigned int command,
			  struct gpgpu_event_request *r)
{
	struct gpgpu_qdev *dev = client->dev;
	struct gpgpu_qevent *e;
	long waited;
	int ret = 0;

	if (command == GPGPU_IOC_EVENT_CREATE) {
		struct gpgpu_user_queue *q = gpgpu_queue_find(client, r->queue_id);

		if (!q || q->dead || q->state != GPGPU_QUEUE_READY || !client->next_event)
			return -EINVAL;
		e = kzalloc(sizeof(*e), GFP_KERNEL);
		if (!e)
			return -ENOMEM;
		e->mailbox = dma_alloc_coherent(&dev->pdev->dev, sizeof(*e->mailbox),
						&e->mailbox_dma, GFP_KERNEL);
		if (!e->mailbox) {
			kfree(e);
			return -ENOMEM;
		}
		*e->mailbox = 0;
		e->queue = q;
		e->id = client->next_event++;
		init_waitqueue_head(&e->wait);
		list_add_tail(&e->node, &client->events);
		r->event_id = e->id;
		r->mailbox_address = e->mailbox_dma;
	} else {
		e = find_event(client, r->event_id);
		if (!e)
			return -EINVAL;
	}
	if (command == GPGPU_IOC_EVENT_DESTROY) {
		gpgpu_ih_drain_locked(dev);
		if (e->waiters || (!e->queue->dead && e->state != GPGPU_EVENT_NOTIFIED))
			return -EBUSY;
		delete_event(client, e);
		return 0;
	}
	if (command == GPGPU_IOC_EVENT_WAIT) {
		e->waiters++;
		mutex_unlock(&dev->lock);
		if (!r->timeout_ms) {
			waited = READ_ONCE(e->state) != GPGPU_EVENT_PENDING;
		} else if (r->timeout_ms == U32_MAX) {
			waited = wait_event_interruptible(
				e->wait, READ_ONCE(e->state) != GPGPU_EVENT_PENDING);
			if (!waited)
				waited = 1;
		} else {
			waited = wait_event_interruptible_timeout(
				e->wait, READ_ONCE(e->state) != GPGPU_EVENT_PENDING,
				msecs_to_jiffies(r->timeout_ms));
		}
		mutex_lock(&dev->lock);
		e->waiters--;
		if (waited < 0)
			ret = waited;
		else if (!waited)
			ret = -ETIMEDOUT;
	}
	r->state = e->state;
	r->error = e->error;
	r->queue_id = e->queue->id;
	if (command == GPGPU_IOC_EVENT_WAIT && e->state == GPGPU_EVENT_FAILED)
		ret = -EIO;
	return ret;
}

/*
 * gpgpu_event_release_all() - 释放会话的所有事件
 *
 * 调用方在设备访问已结束或设备停机后持锁调用。
 */
void gpgpu_event_release_all(struct gpgpu_qclient *client)
{
	struct gpgpu_qevent *e, *next;

	list_for_each_entry_safe(e, next, &client->events, node) {
		delete_event(client, e);
	}
}
