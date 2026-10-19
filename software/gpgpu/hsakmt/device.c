/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdlib.h>
#include "internal.h"

pthread_mutex_t gpgpu_kmt_lock = PTHREAD_MUTEX_INITIALIZER;
static GpgpuKmtContext connection = { .fd = -1 };

/*
 * gpgpu_kmt_error() - 将正 errno 转换为库状态码
 *
 * 按参数、资源、等待等错误类别映射，未知值返回通用错误。
 */
GpgpuKmtStatus gpgpu_kmt_error(int error)
{
	switch (error) {
	case 0:
		return GPGPU_KMT_SUCCESS;
	case EINVAL:
	case EBADF:
	case ENOENT:
		return GPGPU_KMT_INVALID_PARAMETER;
	case EPROTONOSUPPORT:
		return GPGPU_KMT_VERSION_MISMATCH;
	case ENOMEM:
		return GPGPU_KMT_NO_MEMORY;
	case ENOSPC:
		return GPGPU_KMT_OUT_OF_RESOURCES;
	case EBUSY:
	case EAGAIN:
		return GPGPU_KMT_UNAVAILABLE;
	case EOPNOTSUPP:
	case ENOTTY:
		return GPGPU_KMT_NOT_SUPPORTED;
	case ETIMEDOUT:
		return GPGPU_KMT_WAIT_TIMEOUT;
	case EIO:
	case ENODEV:
		return GPGPU_KMT_WAIT_FAILURE;
	default:
		return GPGPU_KMT_ERROR;
	}
}

/*
 * gpgpu_kmt_begin() - 为一次库调用持有连接使用计数
 *
 * 在全局锁下校验连接及进程归属，阻止最后关闭与进行中的调用并发。
 */
GpgpuKmtStatus gpgpu_kmt_begin(GpgpuKmtContext *context)
{
	GpgpuKmtStatus status = GPGPU_KMT_INVALID_PARAMETER;

	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (context == &connection && connection.fd >= 0 && connection.process == getpid() &&
	    connection.references) {
		connection.calls++;
		status = GPGPU_KMT_SUCCESS;
	}
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	return status;
}

/*
 * gpgpu_kmt_end() - 结束一次已登记的库调用
 *
 * 在全局锁下减去调用计数，与 gpgpu_kmt_begin 配对。
 */
void gpgpu_kmt_end(GpgpuKmtContext *context)
{
	pthread_mutex_lock(&gpgpu_kmt_lock);
	context->calls--;
	pthread_mutex_unlock(&gpgpu_kmt_lock);
}

/*
 * gpgpu_kmt_objects() - 调整连接持有的对象计数
 *
 * 在全局锁下更新，最后关闭必须等待对象计数归零。
 */
void gpgpu_kmt_objects(GpgpuKmtContext *context, int change)
{
	pthread_mutex_lock(&gpgpu_kmt_lock);
	context->objects += change;
	pthread_mutex_unlock(&gpgpu_kmt_lock);
}

/* 只转换明确共用的操作，不把旧物理地址接口带入 GPUVM 模式。 */
int gpgpu_kmt_raw_ioctl(GpgpuKmtContext *context, unsigned long command, void *request)
{
	if (context->gpuvm) {
		switch (command) {
		case GPGPU_IOC_MEMORY_COPY:
			command = GPGPU_DRM_COPY;
			break;
		case GPGPU_IOC_QUEUE_CREATE:
			command = GPGPU_DRM_QUEUE_CREATE;
			break;
		case GPGPU_IOC_QUEUE_QUERY:
			command = GPGPU_DRM_QUEUE_QUERY;
			break;
		case GPGPU_IOC_QUEUE_DESTROY:
			command = GPGPU_DRM_QUEUE_DESTROY;
			break;
		case GPGPU_IOC_EVENT_CREATE:
			command = GPGPU_DRM_EVENT_CREATE;
			break;
		case GPGPU_IOC_EVENT_QUERY:
			command = GPGPU_DRM_EVENT_QUERY;
			break;
		case GPGPU_IOC_EVENT_WAIT:
			command = GPGPU_DRM_EVENT_WAIT;
			break;
		case GPGPU_IOC_EVENT_DESTROY:
			command = GPGPU_DRM_EVENT_DESTROY;
			break;
		}
	}
	return ioctl(context->fd, command, request);
}

/*
 * gpgpu_kmt_ioctl() - 执行一次带连接保护的 ioctl
 *
 * 调用前后维护使用计数，并将 errno 转成 HSAKMT 风格状态。
 */
GpgpuKmtStatus gpgpu_kmt_ioctl(GpgpuKmtContext *context, unsigned long command, void *request)
{
	GpgpuKmtStatus status = gpgpu_kmt_begin(context);

	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = gpgpu_kmt_raw_ioctl(context, command, request) < 0 ? gpgpu_kmt_error(errno) :
								      GPGPU_KMT_SUCCESS;
	gpgpu_kmt_end(context);
	return status;
}

/*
 * gpgpuKmtOpen() - 打开或引用进程内共享设备连接
 *
 * 首次打开校验队列 ABI；重复打开相同路径只增加引用，拒绝 fork 后复用。
 */
GpgpuKmtStatus gpgpuKmtOpen(const char *device, GpgpuKmtContext **context)
{
	GpgpuKmtStatus status = GPGPU_KMT_SUCCESS;
	struct gpgpu_queue_info info = { .version = GPGPU_QABI_VERSION };
	struct gpgpu_vm_info vm_info = { .version = GPGPU_DRM_VERSION };
	bool gpuvm = false;
	int fd;

	if (!context)
		return GPGPU_KMT_INVALID_PARAMETER;
	*context = NULL;
	device = device ?
		device :
		(access("/dev/dri/renderD128", F_OK) == 0 ? "/dev/dri/renderD128" : "/dev/gpgpu0");
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (connection.fd >= 0) {
		if (connection.closing || connection.process != getpid() ||
		    strcmp(connection.path, device)) {
			status = GPGPU_KMT_UNAVAILABLE;
		} else {
			connection.references++;
			*context = &connection;
		}
		goto done;
	}
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		status = gpgpu_kmt_error(errno);
		goto done;
	}
	if (ioctl(fd, GPGPU_DRM_INFO, &vm_info) == 0) {
		gpuvm = true;
		if (vm_info.version != GPGPU_DRM_VERSION || vm_info.page_size != 4096 ||
		    vm_info.range_count != 3) {
			status = GPGPU_KMT_VERSION_MISMATCH;
			close(fd);
			goto done;
		}
	} else if (ioctl(fd, GPGPU_IOC_QINFO, &info) < 0 || info.version != GPGPU_QABI_VERSION) {
		status = gpgpu_kmt_error(errno);
		close(fd);
		goto done;
	}
	connection.path = strdup(device);
	if (!connection.path) {
		close(fd);
		status = GPGPU_KMT_NO_MEMORY;
		goto done;
	}
	connection.fd = fd;
	connection.process = getpid();
	connection.references = 1;
	connection.info = info;
	connection.gpuvm = gpuvm;
	connection.vm_info = vm_info;
	*context = &connection;
done:
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	return status;
}

/* 调用方已停止提交；最多等待五秒，故障 Queue 直接使用错误卸载协议。 */
static GpgpuKmtStatus vm_close_objects(GpgpuKmtContext *c)
{
	GpgpuKmtStatus status;

	while (c->queues) {
		GpgpuKmtQueue *q = c->queues;
		unsigned int attempts = 0;

		while (__atomic_load_n(q->rptr, __ATOMIC_ACQUIRE) !=
		       __atomic_load_n(q->wptr, __ATOMIC_ACQUIRE)) {
			struct gpgpu_queue_state state;

			status = gpgpuKmtQueryQueue(q, &state);
			if (status != GPGPU_KMT_SUCCESS)
				return status;
			if (state.state == GPGPU_QUEUE_FAULTED)
				break;
			if (++attempts >= 5000)
				return GPGPU_KMT_WAIT_TIMEOUT;
			usleep(1000);
		}
		status = gpgpuKmtDestroyQueue(q);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	while (c->events) {
		status = gpgpuKmtDestroyEvent(c->events);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	while (c->memory) {
		status = gpgpuKmtFreeMemory(c->memory);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	return GPGPU_KMT_SUCCESS;
}

/*
 * gpgpuKmtClose() - 释放一个共享连接引用
 *
 * GPUVM 最后关闭按队列、事件、映射顺序清理；有并发调用或未知状态时保留。
 * 旧模式仍要求调用方先显式销毁全部对象。
 */
GpgpuKmtStatus gpgpuKmtClose(GpgpuKmtContext *context)
{
	GpgpuKmtStatus status = GPGPU_KMT_SUCCESS;

	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (context != &connection || connection.fd < 0 || connection.process != getpid() ||
	    !connection.references) {
		status = GPGPU_KMT_INVALID_PARAMETER;
	} else if (connection.closing && !pthread_equal(connection.closer, pthread_self())) {
		status = GPGPU_KMT_UNAVAILABLE;
	} else if (connection.references > 1) {
		connection.references--;
	} else if (connection.gpuvm && connection.objects && !connection.calls &&
		   !connection.uncertain) {
		connection.closing = true;
		connection.closer = pthread_self();
		pthread_mutex_unlock(&gpgpu_kmt_lock);
		status = vm_close_objects(context);
		if (status == GPGPU_KMT_SUCCESS)
			return gpgpuKmtClose(context);
		pthread_mutex_lock(&gpgpu_kmt_lock);
		connection.closing = false;
		pthread_mutex_unlock(&gpgpu_kmt_lock);
		return status;
	} else if (connection.objects || connection.calls || connection.uncertain) {
		status = GPGPU_KMT_UNAVAILABLE;
	} else {
		close(connection.fd);
		free(connection.path);
		memset(&connection, 0, sizeof(connection));
		connection.fd = -1;
	}
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	return status;
}

/*
 * gpgpuKmtGetInfo() - 查询队列模式的设备能力
 *
 * 初始化请求版本后通过驱动返回容量、地址位宽与队列上限。
 */
GpgpuKmtStatus gpgpuKmtGetInfo(GpgpuKmtContext *context, struct gpgpu_queue_info *info)
{
	if (!info)
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(info, 0, sizeof(*info));
	info->version = GPGPU_QABI_VERSION;
	return gpgpu_kmt_ioctl(context, GPGPU_IOC_QINFO, info);
}

GpgpuKmtStatus gpgpuKmtGetVmInfo(GpgpuKmtContext *context, struct gpgpu_vm_info *info)
{
	if (!info)
		return GPGPU_KMT_INVALID_PARAMETER;
	memset(info, 0, sizeof(*info));
	info->version = GPGPU_DRM_VERSION;
	return gpgpu_kmt_ioctl(context, GPGPU_DRM_INFO, info);
}

GpgpuKmtStatus gpgpuKmtGetVmFault(GpgpuKmtQueue *queue, struct gpgpu_vm_fault *fault)
{
	if (!queue || !queue->context || !fault)
		return GPGPU_KMT_INVALID_PARAMETER;
	*fault = (struct gpgpu_vm_fault) { .version = GPGPU_DRM_VERSION, .queue_id = queue->id };
	return gpgpu_kmt_ioctl(queue->context, GPGPU_DRM_FAULT, fault);
}

/*
 * gpgpuKmtStatusName() - 取得库状态码的中文说明
 *
 * 按枚举索引查询静态字符串，越界返回未知状态。
 */
const char *gpgpuKmtStatusName(GpgpuKmtStatus status)
{
	static const char *const names[] = {
		"成功",	    "参数或句柄非法", "版本不匹配", "内存不足", "名额不足",
		"暂不可用", "不支持",	      "等待超时",   "等待失败", "失败",
	};

	return (unsigned int)status < sizeof(names) / sizeof(names[0]) ? names[status] : "未知状态";
}
