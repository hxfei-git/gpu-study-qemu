// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：中断与 IH Ring
 *
 * 中断上半部（gpgpu_irq_handler）只做确认与转交：
 *   - DMA 完成或 DMA 错误：唤醒等待中的 COPY；
 *   - IH 故障或固件错误：标记设备故障，交给工作项传播失败；
 *   - IH 有记录：先屏蔽 IH 中断，交给工作项消费，消费后再确认并恢复，
 *     避免未消费的记录反复触发中断。
 *
 * 工作项（gpgpu_ih_work）持 dev->lock 消费记录：
 *   SIGNAL_NOTIFY → 按 MQD 地址找到队列，再按事件编号唤醒等待者
 *   QUEUE_ERROR   → 用户队列失败；HIQ 出错停止管理；KIQ 出错设备故障
 *   DEVICE_ERROR  → 设备故障，所有队列失败
 */
#include "gpgpu_drv.h"

/* IH Ring 的记录槽数。 */
#define IH_SLOTS (GPGPU_IH_BYTES / sizeof(GpgpuIhRecord))

/* 在全部会话中按 MQD 地址查找用户队列；调用者持 dev->lock。 */
static struct gpgpu_queue *find_queue_by_mqd(struct gpgpu_device *dev, u64 mqd)
{
	struct gpgpu_fpriv *fpriv;
	struct gpgpu_queue *q;

	list_for_each_entry(fpriv, &dev->fprivs, node)
		list_for_each_entry(q, &fpriv->queues, node)
			if (q->mqd.addr == mqd)
				return q;
	return NULL;
}

/* QUEUE_ERROR：区分用户队列、HIQ 与 KIQ。 */
static void ih_queue_error(struct gpgpu_device *dev, u64 mqd, u32 error)
{
	struct gpgpu_queue *q = find_queue_by_mqd(dev, mqd);

	if (q) {
		gpgpu_fail_queues(dev, q, error);
	} else if (mqd == dev->mgmt[GPGPU_HQD_SLOT_HIQ].mqd.addr) {
		WRITE_ONCE(dev->mgmt[GPGPU_HQD_SLOT_HIQ].failed, true);
	} else if (mqd == dev->mgmt[GPGPU_HQD_SLOT_KIQ].mqd.addr) {
		WRITE_ONCE(dev->mgmt[GPGPU_HQD_SLOT_KIQ].failed, true);
		WRITE_ONCE(dev->fatal, true);
		gpgpu_fail_queues(dev, NULL, error);
	}
}

static void ih_handle(struct gpgpu_device *dev, const GpgpuIhRecord *r)
{
	u32 type = le32_to_cpu(r->type);
	u32 data = le32_to_cpu(r->data);
	u64 mqd = le64_to_cpu(r->mqd_addr);
	struct gpgpu_queue *q;

	switch (type) {
	case GPGPU_IH_SIGNAL_NOTIFY:
		q = find_queue_by_mqd(dev, mqd);
		if (q)
			gpgpu_event_signal(q, data);
		break;
	case GPGPU_IH_QUEUE_ERROR:
		ih_queue_error(dev, mqd, data);
		break;
	case GPGPU_IH_DEVICE_ERROR:
		WRITE_ONCE(dev->fatal, true);
		gpgpu_fail_queues(dev, NULL, data);
		break;
	}
}

/*
 * gpgpu_ih_drain() - 消费当前已发布的全部 IH 记录
 *
 * 1. 读 W；W 越过 R 超过 Ring 容量说明协议已乱，设备故障；
 * 2. W 在记录写完后才发布，读到 W 后再读记录；
 * 3. 依次处理记录；
 * 4. 处理完毕再写回 R，把槽还给设备。
 *
 * 调用者持 dev->lock。
 */
void gpgpu_ih_drain(struct gpgpu_device *dev)
{
	const GpgpuIhRecord *records = dev->ih.cpu;
	u32 wptr = gpgpu_rreg(dev, GPGPU_REG_IH_WPTR);

	/* 1. 范围。 */
	if (wptr - dev->ih_rptr > IH_SLOTS) {
		WRITE_ONCE(dev->fatal, true);
		gpgpu_fail_queues(dev, NULL, GPGPU_ERR_IH_FAULT);
		return;
	}

	/* 2. 3. 读取并处理。 */
	dma_rmb();
	for (; dev->ih_rptr != wptr; dev->ih_rptr++)
		ih_handle(dev, &records[dev->ih_rptr % IH_SLOTS]);

	/* 4. 归还。 */
	dma_mb();
	gpgpu_wreg(dev, GPGPU_REG_IH_RPTR, dev->ih_rptr);
}

/* 消费记录；设备故障时让全部队列失败；最后确认 IH 中断并恢复使能。 */
void gpgpu_ih_work(struct work_struct *work)
{
	struct gpgpu_device *dev = container_of(work, struct gpgpu_device, ih_work);
	unsigned long flags;

	mutex_lock(&dev->lock);
	if (dev->present) {
		gpgpu_ih_drain(dev);
		if (dev->fatal)
			gpgpu_fail_queues(dev, NULL, GPGPU_ERR_IH_FAULT);
		spin_lock_irqsave(&dev->irq_lock, flags);
		gpgpu_wreg(dev, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_IH);
		gpgpu_wreg(dev, GPGPU_REG_IRQ_ENABLE, GPGPU_IRQ_ALL);
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	mutex_unlock(&dev->lock);
}

/* 全局错误：DMA 故障唤醒 COPY，IH/固件故障标记设备故障。 */
static void irq_handle_error(struct gpgpu_device *dev)
{
	u32 error = gpgpu_rreg(dev, GPGPU_REG_ERROR_STATUS);

	if (error & GPGPU_ERR_DMA_FAULT) {
		WRITE_ONCE(dev->dma_failed, true);
		complete(&dev->dma_done);
	}
	if (error & (GPGPU_ERR_IH_FAULT | GPGPU_ERR_FIRMWARE)) {
		WRITE_ONCE(dev->fatal, true);
		schedule_work(&dev->ih_work);
	}
}

/* 所有 MSI-X 向量共用的中断处理，data 为 gpgpu_device。 */
irqreturn_t gpgpu_irq_handler(int irq, void *data)
{
	struct gpgpu_device *dev = data;
	unsigned long flags;
	u32 pending;

	spin_lock_irqsave(&dev->irq_lock, flags);
	if (!READ_ONCE(dev->present))
		goto none;
	pending = gpgpu_rreg(dev, GPGPU_REG_IRQ_STATUS) & GPGPU_IRQ_ALL;
	if (!pending)
		goto none;

	if (pending & GPGPU_IRQ_ERROR)
		irq_handle_error(dev);
	if (pending & GPGPU_IRQ_DMA_DONE)
		complete(&dev->dma_done);
	if (pending & GPGPU_IRQ_IH) {
		gpgpu_wreg(dev, GPGPU_REG_IRQ_ENABLE, GPGPU_IRQ_ALL & ~GPGPU_IRQ_IH);
		schedule_work(&dev->ih_work);
	}
	gpgpu_wreg(dev, GPGPU_REG_IRQ_ACK, pending & ~GPGPU_IRQ_IH);
	spin_unlock_irqrestore(&dev->irq_lock, flags);
	return IRQ_HANDLED;
none:
	spin_unlock_irqrestore(&dev->irq_lock, flags);
	return IRQ_NONE;
}
