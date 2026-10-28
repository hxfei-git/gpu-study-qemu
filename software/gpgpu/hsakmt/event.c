/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"

/*
 * gpgpuKmtCreateEvent() - 为源队列创建完成事件
 *
 * 驱动返回事件编号与通知槽物理地址，成功后计入连接对象数。
 */
GpgpuKmtStatus gpgpuKmtCreateEvent(GpgpuKmtQueue *queue, GpgpuKmtEvent *event)
{
	struct gpgpu_event_request request;
	GpgpuKmtStatus status;

	if (!queue || !queue->context || !event)
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(event, 0, sizeof(*event));
	request = (struct gpgpu_event_request) {
		.version = GPGPU_QABI_VERSION,
		.queue_id = queue->id,
	};
	status = gpgpu_kmt_begin(queue->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	if (gpgpu_kmt_raw_ioctl(queue->context, GPGPU_DRM_EVENT_CREATE, &request) < 0) {
		status = gpgpu_kmt_error(errno);
	} else {
		*event = (GpgpuKmtEvent) {
			.context = queue->context,
			.id = request.event_id,
			.queue_id = queue->id,
			.mailbox_address = request.mailbox_address,
		};
		{
			pthread_mutex_lock(&gpgpu_kmt_lock);
			event->next = queue->context->events;
			queue->context->events = event;
			pthread_mutex_unlock(&gpgpu_kmt_lock);
		}
		gpgpu_kmt_objects(queue->context, 1);
	}
	gpgpu_kmt_end(queue->context);
	return status;
}

/*
 * gpgpuKmtQueryEvent() - 查询驱动已保存的事件状态
 *
 * 按事件编号发起查询，不消耗已到达的通知。
 */
GpgpuKmtStatus gpgpuKmtQueryEvent(GpgpuKmtEvent *event, struct gpgpu_event_request *state)
{
	if (!event || !event->context || !state)
		return GPGPU_KMT_INVALID_PARAMETER;
	*state = (struct gpgpu_event_request) {
		.version = GPGPU_QABI_VERSION,
		.event_id = event->id,
	};
	return gpgpu_kmt_ioctl(event->context, GPGPU_DRM_EVENT_QUERY, state);
}

/*
 * gpgpuKmtWaitEvent() - 等待驱动事件状态改变
 *
 * timeout_ms 为毫秒，0 只检查、UINT32_MAX 不设期限；Signal 由调用方复查。
 */
GpgpuKmtStatus gpgpuKmtWaitEvent(GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	struct gpgpu_event_request request;

	if (!event || !event->context)
		return GPGPU_KMT_INVALID_PARAMETER;
	request = (struct gpgpu_event_request) {
		.version = GPGPU_QABI_VERSION,
		.event_id = event->id,
		.timeout_ms = timeout_ms,
	};
	return gpgpu_kmt_ioctl(event->context, GPGPU_DRM_EVENT_WAIT, &request);
}

/*
 * gpgpuKmtDestroyEvent() - 销毁通知事件并释放库对象引用
 *
 * 仅驱动确认可销毁后清空用户对象，失败时保留其身份。
 */
GpgpuKmtStatus gpgpuKmtDestroyEvent(GpgpuKmtEvent *event)
{
	struct gpgpu_event_request request;
	GpgpuKmtStatus status;
	GpgpuKmtContext *context;

	if (!event || !event->context)
		return GPGPU_KMT_INVALID_PARAMETER;
	context = event->context;
	request = (struct gpgpu_event_request) {
		.version = GPGPU_QABI_VERSION,
		.event_id = event->id,
	};
	status = gpgpu_kmt_begin(context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	if (gpgpu_kmt_raw_ioctl(context, GPGPU_DRM_EVENT_DESTROY, &request) < 0) {
		status = gpgpu_kmt_error(errno);
	} else {
		{
			GpgpuKmtEvent **link;

			pthread_mutex_lock(&gpgpu_kmt_lock);
			for (link = &context->events; *link && *link != event;
			     link = &(*link)->next) {
			}
			if (*link)
				*link = event->next;
			pthread_mutex_unlock(&gpgpu_kmt_lock);
		}
		gpgpu_kmt_objects(context, -1);
		memset(event, 0, sizeof(*event));
	}
	gpgpu_kmt_end(context);
	return status;
}
