// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：完成事件
 */
#include <stdlib.h>
#include "kmt_internal.h"

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
	args = (struct gpgpu_event_request) { .version = GPGPU_DRM_VERSION, .queue_id = q->id };
	status = kmt_call(c, GPGPU_DRM_EVENT_CREATE, &args);
	if (status != GPGPU_KMT_SUCCESS) {
		free(e);
		return status;
	}
	e->pub = (GpgpuKmtEvent) { .id = args.event_id, .mailbox_address = args.mailbox_address };
	e->context = c;
	pthread_mutex_lock(&c->lock);
	e->next = c->events;
	c->events = e;
	pthread_mutex_unlock(&c->lock);
	*event = &e->pub;
	return GPGPU_KMT_SUCCESS;
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

/* 驱动确认可以销毁后才摘除并释放；失败时对象保持可用。 */
GpgpuKmtStatus gpgpuKmtDestroyEvent(GpgpuKmtEvent *event)
{
	KmtEvent *e = kmt_event(event);
	struct gpgpu_event_request args;
	GpgpuKmtContext *c;
	GpgpuKmtStatus status;

	if (!e)
		return GPGPU_KMT_INVALID_PARAMETER;
	c = e->context;
	args = (struct gpgpu_event_request) { .version = GPGPU_DRM_VERSION, .event_id = e->pub.id };
	status = kmt_call(c, GPGPU_DRM_EVENT_DESTROY, &args);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	pthread_mutex_lock(&c->lock);
	KMT_UNLINK(&c->events, e);
	pthread_mutex_unlock(&c->lock);
	free(e);
	return GPGPU_KMT_SUCCESS;
}
