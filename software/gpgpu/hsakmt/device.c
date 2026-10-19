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
	status = ioctl(context->fd, command, request) < 0 ? gpgpu_kmt_error(errno) :
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
	int fd;

	if (!context)
		return GPGPU_KMT_INVALID_PARAMETER;
	*context = NULL;
	device = device ? device : "/dev/gpgpu0";
	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (connection.fd >= 0) {
		if (connection.process != getpid() || strcmp(connection.path, device)) {
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
	if (ioctl(fd, GPGPU_IOC_QINFO, &info) < 0) {
		status = gpgpu_kmt_error(errno);
		close(fd);
		goto done;
	}
	if (info.version != GPGPU_QABI_VERSION) {
		status = GPGPU_KMT_VERSION_MISMATCH;
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
	*context = &connection;
done:
	pthread_mutex_unlock(&gpgpu_kmt_lock);
	return status;
}

/*
 * gpgpuKmtClose() - 释放一个共享连接引用
 *
 * 最后关闭须确认没有存活对象和调用，否则返回暂不可用。
 */
GpgpuKmtStatus gpgpuKmtClose(GpgpuKmtContext *context)
{
	GpgpuKmtStatus status = GPGPU_KMT_SUCCESS;

	pthread_mutex_lock(&gpgpu_kmt_lock);
	if (context != &connection || connection.fd < 0 || connection.process != getpid() ||
	    !connection.references) {
		status = GPGPU_KMT_INVALID_PARAMETER;
	} else if (connection.references > 1) {
		connection.references--;
	} else if (connection.objects || connection.calls) {
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
