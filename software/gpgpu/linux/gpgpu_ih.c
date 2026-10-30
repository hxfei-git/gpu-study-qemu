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

/*
 * QUEUE_ERROR：按记录中的 MQD 地址区分出错的是哪条队列。
 *
 * 用户队列出错只让本队列失败，并结束它的事件等待。HIQ 出错只
 * 停止 HIQ，之后不能再建或卸载用户队列，已有用户队列继续运行。
 * KIQ 出错后 CP 停止全部取包，按设备故障处理，所有队列失败。
 */
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

/* 按类型处理一条 IH 记录；找不到来源队列的记录和未知类型直接忽略。 */
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
 * 除 IH 工作项外，卸载队列、销毁事件、拆除 VM 和建队列前也会
 * 同步调用，保证旧队列的通知和故障先处理完，再回收或复用相关对象。
 *
 * 1. 从寄存器读设备发布的 W（下一条待写记录编号）。W - R 超过 Ring 槽数
 *    说明设备覆盖了未消费的记录，记录已不可信，标记设备故障并让全部
 *    队列失败，不再读 Ring；
 * 2. 设备先写记录、再发布 W。读到 W 后执行 dma_rmb()，再读记录内容，
 *    保证读到的是完整记录；
 * 3. 从 R 到 W 依次处理，编号对槽数取模得到物理槽；
 * 4. 全部处理完后 dma_mb()，再把新 R 写回寄存器。设备看到新 R 才会复用
 *    这些槽，屏障保证此前对记录的读取已经结束。
 *
 * 调用者持 dev->lock。
 */
void gpgpu_ih_drain(struct gpgpu_device *dev)
{
	const GpgpuIhRecord *records = dev->ih.cpu;
	u32 wptr = gpgpu_rreg(dev, GPGPU_REG_IH_WPTR);

	/* 1. W 越过 R 超过一整圈，记录已被覆盖，设备故障。 */
	if (wptr - dev->ih_rptr > IH_SLOTS) {
		WRITE_ONCE(dev->fatal, true);
		gpgpu_fail_queues(dev, NULL, GPGPU_ERR_IH_FAULT);
		return;
	}

	/* 2. 3. 先读 W 再读记录，然后逐条处理。 */
	dma_rmb();
	for (; dev->ih_rptr != wptr; dev->ih_rptr++)
		ih_handle(dev, &records[dev->ih_rptr % IH_SLOTS]);

	/* 4. 读完全部记录后再回报 R，把槽还给设备。 */
	dma_mb();
	gpgpu_wreg(dev, GPGPU_REG_IH_RPTR, dev->ih_rptr);
}

/*
 * IH 工作项：在可睡眠上下文中持 dev->lock 消费记录。
 *
 * 上半部收到 IH 中断时已屏蔽 IH 中断，并保留状态位不确认。
 * 这里先消费记录；设备已故障（含上半部发现的 IH 或固件错误）
 * 时让全部队列失败；最后确认 IH 中断并恢复全部使能。确认放在
 * 消费之后，设备若还有未消费的记录会再次置位 IH 中断。
 * 设备已移除时不再访问 BAR。
 */
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

/*
 * 处理 IRQ_ERROR，调用者持 irq_lock，处在中断上下文。DMA 故障只记下失败
 * 并唤醒正在等待的 COPY；IH 或固件故障标记设备故障，队列失败需要
 * dev->lock，交给 IH 工作项处理。
 */
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

/*
 * 所有 MSI-X 向量共用的中断处理，data 为 gpgpu_device。
 *
 * 持 irq_lock 读取 IRQ_STATUS，按位转交给错误处理、DMA 完成或 IH 工作项。
 * 除 IH 外的位在这里立即确认；IH 位先屏蔽使能、保留不确认，由工作项
 * 消费完记录后确认，避免记录未消费时中断反复触发。
 */
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
