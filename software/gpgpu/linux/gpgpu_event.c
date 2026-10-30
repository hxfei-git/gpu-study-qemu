// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：完成事件
 *
 * 每个事件独占会话通知页中的一个 8 字节通知槽。应用把事件编号和通知槽地址
 * 写进 Signal；任务完成时 CP 依次写 Signal、通知槽、IH 记录。驱动消费 IH
 * 记录时核对通知槽内容，把事件置为 NOTIFIED，清零通知槽，再唤醒等待者。
 *
 * 事件状态只前进不回退：PENDING → NOTIFIED 或 FAILED。等待不消费状态，
 * 同一任务可以唤醒多个等待者。队列出错或设备停止时，相关的 PENDING 事件
 * 置为 FAILED 并唤醒。
 */
#include <linux/slab.h>
#include "gpgpu_drv.h"

static struct gpgpu_event *event_lookup(struct gpgpu_fpriv *fpriv, u32 id)
{
	struct gpgpu_event *e;

	list_for_each_entry(e, &fpriv->events, node)
		if (e->id == id)
			return e;
	return NULL;
}

/* 先写状态再唤醒：等待者登记后会重新检查状态，不会错过通知。 */
static void event_complete(struct gpgpu_event *e, u32 state, u32 error)
{
	e->error = error;
	WRITE_ONCE(e->state, state);
	wake_up_all(&e->wait);
}

/**
 * gpgpu_fail_queues() - 让队列出错并结束相关等待
 * @dev: 设备；调用者持 dev->lock。
 * @queue: 出错的队列；为 NULL 时作用于全部会话的全部队列。
 * @error: 错误值；指定队列时记为该队列的首次错误。
 *
 * 未卸载的目标队列先保存所在槽的 GPUVM 故障，再置为 FAULTED；相关的
 * PENDING 事件以 @error 失败结束并唤醒等待者。
 */
void gpgpu_fail_queues(struct gpgpu_device *dev, struct gpgpu_queue *queue, u32 error)
{
	struct gpgpu_fpriv *fpriv;
	struct gpgpu_queue *q;
	struct gpgpu_event *e;

	list_for_each_entry(fpriv, &dev->fprivs, node) {
		list_for_each_entry(q, &fpriv->queues, node) {
			if (q->dead || (queue && q != queue))
				continue;
			if (queue && !q->error)
				q->error = error;
			gpgpu_vm_save_fault(q);
			q->state = GPGPU_QUEUE_FAULTED;
		}
		list_for_each_entry(e, &fpriv->events, node)
			if ((!queue || e->queue == queue) && e->state == GPGPU_EVENT_PENDING)
				event_complete(e, GPGPU_EVENT_FAILED, error);
	}
}

/**
 * gpgpu_event_signal() - 处理一条 SIGNAL_NOTIFY 记录
 * @q: 记录来源队列；调用者持 dev->lock。
 * @event_id: 记录中的事件编号。
 *
 * 只有通知槽中确实写着同一编号时才接受，防止过期记录误唤醒新事件。
 */
void gpgpu_event_signal(struct gpgpu_queue *q, u32 event_id)
{
	struct gpgpu_event *e;

	list_for_each_entry(e, &q->fpriv->events, node) {
		if (e->queue != q || e->id != event_id ||
		    le64_to_cpu(READ_ONCE(*e->mailbox)) != event_id)
			continue;
		if (e->state == GPGPU_EVENT_PENDING)
			WRITE_ONCE(e->state, GPGPU_EVENT_NOTIFIED);
		WRITE_ONCE(*e->mailbox, cpu_to_le64(0));
		dma_wmb();
		wake_up_all(&e->wait);
		return;
	}
}

static void event_report(struct gpgpu_event *e, struct gpgpu_event_request *args)
{
	args->state = e->state;
	args->error = e->error;
	args->queue_id = e->queue->id;
}

/*
 * gpgpu_event_create() - EVENT_CREATE：为一个就绪队列创建完成事件
 *
 * 应用随后把返回的事件编号和通知槽地址写进 Signal，CP 完成任务时据此
 * 写通知槽并提交 SIGNAL_NOTIFY。调用者持 dev->lock。
 *
 * 1. 来源队列必须存在、未卸载且处于 READY。事件编号回绕为 0
 *    后不再创建，避免与仍存活的旧事件同号；
 * 2. 在会话通知页中找一个空闲的 8 字节槽并清零。通知页在第一次 MAP 时
 *    才分配，还没有通知页时返回 -ENOSPC；
 * 3. 登记事件并返回编号与通知槽地址：gpuvm=on 时是通知页的固定 GPUVA，
 *    gpuvm=off 时是通知页的 GPU 物理地址，再加槽号 * 8。
 */
int gpgpu_event_create(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args)
{
	struct gpgpu_vm *vm = &fpriv->vm;
	struct gpgpu_queue *q = gpgpu_queue_lookup(fpriv, args->queue_id);
	struct gpgpu_event *e;
	u32 slot;

	/* 1. 只有 READY 的队列才会产生新的完成通知。 */
	if (!q || q->dead || q->state != GPGPU_QUEUE_READY || !fpriv->next_event)
		return -EINVAL;

	/* 2. 占用一个空闲通知槽，CP 写到这里的值就是事件编号。 */
	slot = find_first_zero_bit(vm->notify_slots, GPGPU_NOTIFY_SLOTS);
	if (!vm->notify.cpu || slot >= GPGPU_NOTIFY_SLOTS)
		return -ENOSPC;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	set_bit(slot, vm->notify_slots);
	e->slot = slot;
	e->mailbox = (u64 *)vm->notify.cpu + slot;
	e->mailbox_addr = (fpriv->dev->gpuvm ? GPGPU_VM_NOTIFY_VA : vm->notify.addr) + slot * 8;
	*e->mailbox = 0;

	/* 3. 编号在会话内递增且非零，事件挂到会话链表后返回给应用。 */
	e->queue = q;
	e->id = fpriv->next_event++;
	init_waitqueue_head(&e->wait);
	list_add_tail(&e->node, &fpriv->events);

	args->event_id = e->id;
	args->mailbox_address = e->mailbox_addr;
	event_report(e, args);
	return 0;
}

int gpgpu_event_query(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args)
{
	struct gpgpu_event *e = event_lookup(fpriv, args->event_id);

	if (!e)
		return -EINVAL;
	event_report(e, args);
	return 0;
}

/*
 * gpgpu_event_wait() - 等待事件离开 PENDING
 *
 * timeout_ms 为 0 只检查一次；为 U32_MAX 不设期限；其余为毫秒期限。
 * 等待期间释放 dev->lock，waiters 计数阻止事件被销毁。
 *
 * Return: 0 表示已通知；-ETIMEDOUT、-ERESTARTSYS，或事件失败时 -EIO。
 */
int gpgpu_event_wait(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_event *e = event_lookup(fpriv, args->event_id);
	long waited;
	int ret = 0;

	if (!e)
		return -EINVAL;

	/* waiters 非零时 gpgpu_event_destroy() 返回 -EBUSY，解锁后 e 仍然有效。 */
	e->waiters++;
	mutex_unlock(&dev->lock);

	if (!args->timeout_ms) {
		waited = READ_ONCE(e->state) != GPGPU_EVENT_PENDING;
	} else if (args->timeout_ms == U32_MAX) {
		waited = wait_event_interruptible(e->wait,
						  READ_ONCE(e->state) != GPGPU_EVENT_PENDING);
		/* 返回 0 表示条件已满足，记为等到。 */
		if (!waited)
			waited = 1;
	} else {
		waited = wait_event_interruptible_timeout(
			e->wait, READ_ONCE(e->state) != GPGPU_EVENT_PENDING,
			msecs_to_jiffies(args->timeout_ms));
	}

	mutex_lock(&dev->lock);
	e->waiters--;
	if (waited < 0)
		ret = waited;
	else if (!waited)
		ret = -ETIMEDOUT;
	event_report(e, args);
	if (e->state == GPGPU_EVENT_FAILED)
		ret = -EIO;
	return ret;
}

static void event_free(struct gpgpu_fpriv *fpriv, struct gpgpu_event *e)
{
	WRITE_ONCE(*e->mailbox, cpu_to_le64(0));
	clear_bit(e->slot, fpriv->vm.notify_slots);
	list_del(&e->node);
	kfree(e);
}

/*
 * gpgpu_event_destroy() - EVENT_DESTROY：销毁事件并归还通知槽
 *
 * 先消费旧 IH，让已发布的 SIGNAL_NOTIFY 都处理完，再检查两件事：没有
 * 线程正在等待；来源队列已卸载，或事件已通知。否则 CP 或尚未消费的 IH
 * 记录可能还会写这个通知槽，此时返回 -EBUSY 并保留事件。
 * 调用者持 dev->lock。
 */
int gpgpu_event_destroy(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args)
{
	struct gpgpu_event *e = event_lookup(fpriv, args->event_id);

	if (!e)
		return -EINVAL;
	gpgpu_ih_drain(fpriv->dev);
	if (e->waiters || (!e->queue->dead && e->state != GPGPU_EVENT_NOTIFIED))
		return -EBUSY;
	event_free(fpriv, e);
	return 0;
}

/*
 * 释放会话全部事件。调用者持 dev->lock，并已确认 CP 不会再写这些
 * 通知槽：设备已停止，或本 VM 已无存活队列且旧 IH 已消费。
 */
void gpgpu_event_release_all(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_event *e, *next;

	list_for_each_entry_safe(e, next, &fpriv->events, node)
		event_free(fpriv, e);
}
