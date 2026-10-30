// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：完成事件
 *
 * 事件由驱动创建并绑定一条来源队列，返回事件编号和 8 字节通知槽的
 * 设备地址。调用者把二者写进 Signal 的 event_id 和 event_mailbox_ptr。
 * CP 完成任务时先把 Signal 减为 0，再写通知槽并提交 IH 记录，驱动据此
 * 唤醒等待者。库只在本地登记事件，等待和事件状态都在驱动中。
 */
#include <stdlib.h>
#include "kmt_internal.h"

/* 为队列创建事件并登记到连接；驱动拒绝时不留下本地对象。 */
GpgpuKmtStatus gpgpuKmtCreateEvent(GpgpuKmtQueue *q, GpgpuKmtEvent **event)
{
	struct gpgpu_event_request args;
	GpgpuKmtContext *c = q ? q->context : NULL;
	GpgpuKmtStatus status;
	KmtEvent *e;

	if (!c || !event)
		return GPGPU_KMT_INVALID_PARAMETER;
	*event = NULL;
	e = calloc(1, sizeof(*e));
	if (!e)
		return GPGPU_KMT_NO_MEMORY;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS) {
		free(e);
		return status;
	}

	args = (struct gpgpu_event_request) { .version = GPGPU_DRM_VERSION, .queue_id = q->id };
	status = kmt_ioctl(c, GPGPU_DRM_EVENT_CREATE, &args);
	if (status == GPGPU_KMT_SUCCESS) {
		e->pub = (GpgpuKmtEvent) { .id = args.event_id,
					   .mailbox_address = args.mailbox_address };
		e->context = c;
		pthread_mutex_lock(&c->lock);
		e->next = c->events;
		c->events = e;
		pthread_mutex_unlock(&c->lock);
		*event = &e->pub;
		e = NULL;
	}
	kmt_leave(c);
	free(e);
	return status;
}

GpgpuKmtStatus gpgpuKmtQueryEvent(GpgpuKmtEvent *event, struct gpgpu_event_request *state)
{
	KmtEvent *e = kmt_event(event);

	if (!e || !state)
		return GPGPU_KMT_INVALID_PARAMETER;
	*state = (struct gpgpu_event_request) { .version = GPGPU_DRM_VERSION,
						.event_id = e->pub.id };
	return kmt_call(e->context, GPGPU_DRM_EVENT_QUERY, state);
}

GpgpuKmtStatus gpgpuKmtWaitEvent(GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	KmtEvent *e = kmt_event(event);
	struct gpgpu_event_request args;

	if (!e)
		return GPGPU_KMT_INVALID_PARAMETER;
	args = (struct gpgpu_event_request) {
		.version = GPGPU_DRM_VERSION,
		.event_id = e->pub.id,
		.timeout_ms = timeout_ms,
	};
	return kmt_call(e->context, GPGPU_DRM_EVENT_WAIT, &args);
}

/*
 * 驱动确认可以销毁后才摘除并释放；失败时对象保持可用。
 * 有等待者，或来源队列仍存活且事件尚未通知时，驱动返回忙：
 * CP 或旧 IH 记录还可能写这个通知槽。
 */
GpgpuKmtStatus gpgpuKmtDestroyEvent(GpgpuKmtEvent *event)
{
	KmtEvent *e = kmt_event(event);
	struct gpgpu_event_request args;
	GpgpuKmtContext *c;
	GpgpuKmtStatus status;

	if (!e)
		return GPGPU_KMT_INVALID_PARAMETER;
	c = e->context;
	status = kmt_enter(c);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	args = (struct gpgpu_event_request) { .version = GPGPU_DRM_VERSION, .event_id = e->pub.id };
	status = kmt_ioctl(c, GPGPU_DRM_EVENT_DESTROY, &args);
	if (status == GPGPU_KMT_SUCCESS) {
		pthread_mutex_lock(&c->lock);
		KMT_UNLINK(&c->events, e);
		pthread_mutex_unlock(&c->lock);
		free(e);
	}
	kmt_leave(c);
	return status;
}
