// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：连接的打开、关闭与公共辅助函数
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "kmt_internal.h"

static GpgpuKmtContext kmt_connection = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.fd = -1,
};

GpgpuKmtStatus kmt_status(int error)
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

/* 连接可用：已打开、属于当前进程、有引用，且没有被其他线程关闭。调用者持 lock。 */
static bool kmt_usable(GpgpuKmtContext *c)
{
	return c == &kmt_connection && c->fd >= 0 && c->process == getpid() && c->references &&
		(!c->closing || pthread_equal(c->closer, pthread_self()));
}

GpgpuKmtStatus kmt_enter(GpgpuKmtContext *c)
{
	GpgpuKmtStatus status = GPGPU_KMT_INVALID_PARAMETER;

	if (c != &kmt_connection)
		return status;
	pthread_mutex_lock(&c->lock);
	if (kmt_usable(c)) {
		c->calls++;
		status = GPGPU_KMT_SUCCESS;
	}
	pthread_mutex_unlock(&c->lock);
	return status;
}

void kmt_leave(GpgpuKmtContext *c)
{
	pthread_mutex_lock(&c->lock);
	c->calls--;
	pthread_mutex_unlock(&c->lock);
}

GpgpuKmtStatus kmt_ioctl(GpgpuKmtContext *c, unsigned long request, void *args)
{
	return ioctl(c->fd, request, args) < 0 ? kmt_status(errno) : GPGPU_KMT_SUCCESS;
}

GpgpuKmtStatus kmt_call(GpgpuKmtContext *c, unsigned long request, void *args)
{
	GpgpuKmtStatus status = kmt_enter(c);

	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = kmt_ioctl(c, request, args);
	kmt_leave(c);
	return status;
}

void kmt_mark_uncertain(GpgpuKmtContext *c)
{
	pthread_mutex_lock(&c->lock);
	c->uncertain = true;
	pthread_mutex_unlock(&c->lock);
}

bool kmt_range_ok(KmtMemory *m, uint64_t offset, uint64_t size)
{
	return m && size && size <= m->pub.size && offset <= m->pub.size - size;
}

/* 驱动必须是本库支持的版本：4 KiB 页、32 位 GPUVA、至多一个可分配区间。 */
static bool kmt_vm_info_supported(const struct gpgpu_vm_info *info)
{
	return info->version == GPGPU_DRM_VERSION && info->page_size == KMT_PAGE_SIZE &&
		info->address_bits == 32 && info->range_count <= 1;
}

/*
 * 首次打开时：1. 打开 render node；2. 查询并核对 GPUVM 能力；3. 登记连接。
 * 已打开时：同一进程、同一路径只增加引用。
 */
GpgpuKmtStatus gpgpuKmtOpen(const char *path, GpgpuKmtContext **context)
{
	GpgpuKmtContext *c = &kmt_connection;
	struct gpgpu_vm_info info = { .version = GPGPU_DRM_VERSION };
	GpgpuKmtStatus status = GPGPU_KMT_SUCCESS;
	int fd;

	if (!context)
		return GPGPU_KMT_INVALID_PARAMETER;
	*context = NULL;
	path = path ? path : KMT_DEFAULT_DEVICE;
	pthread_mutex_lock(&c->lock);

	/* 已打开。 */
	if (c->fd >= 0) {
		if (c->closing || c->process != getpid() || strcmp(c->path, path))
			status = GPGPU_KMT_UNAVAILABLE;
		else
			c->references++;
		goto out;
	}

	/* 1. 打开。 */
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		status = kmt_status(errno);
		goto out;
	}

	/* 2. 能力。 */
	if (ioctl(fd, GPGPU_DRM_INFO, &info) < 0)
		status = kmt_status(errno);
	else if (!kmt_vm_info_supported(&info))
		status = GPGPU_KMT_VERSION_MISMATCH;
	else if (!(c->path = strdup(path)))
		status = GPGPU_KMT_NO_MEMORY;
	if (status != GPGPU_KMT_SUCCESS) {
		close(fd);
		goto out;
	}

	/* 3. 登记。 */
	c->fd = fd;
	c->process = getpid();
	c->references = 1;
	c->gpuvm = info.range_count == 1;
	c->vm_info = info;
out:
	if (status == GPGPU_KMT_SUCCESS)
		*context = c;
	pthread_mutex_unlock(&c->lock);
	return status;
}

/* 最后一次 Close 时回收剩余对象：队列先排空再卸载，再销毁事件，最后释放内存。 */
static GpgpuKmtStatus kmt_release_objects(GpgpuKmtContext *c)
{
	GpgpuKmtStatus status;

	while (c->queues) {
		status = gpgpuKmtDrainQueue(c->queues, 5000);
		if (status != GPGPU_KMT_SUCCESS && status != GPGPU_KMT_WAIT_FAILURE)
			return status;
		status = gpgpuKmtDestroyQueue(c->queues);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	while (c->events) {
		status = gpgpuKmtDestroyEvent(&c->events->pub);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	while (c->memory) {
		status = gpgpuKmtFreeMemory(&c->memory->pub);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	return GPGPU_KMT_SUCCESS;
}

static bool kmt_has_objects(GpgpuKmtContext *c)
{
	return c->memory || c->queues || c->events;
}

/*
 * 非最后一次只减少引用。最后一次：
 * 1. 有其他调用进行中，或存在结果未知的资源时拒绝；
 * 2. 标记 closing，解锁后回收剩余对象（回收函数会重新进入本连接）；
 * 3. 全部回收后关闭文件描述符并清空连接。
 */
GpgpuKmtStatus gpgpuKmtClose(GpgpuKmtContext *context)
{
	GpgpuKmtContext *c = &kmt_connection;
	GpgpuKmtStatus status;

	if (context != c)
		return GPGPU_KMT_INVALID_PARAMETER;
	pthread_mutex_lock(&c->lock);
	if (!kmt_usable(c)) {
		status = c->closing ? GPGPU_KMT_UNAVAILABLE : GPGPU_KMT_INVALID_PARAMETER;
		goto out;
	}
	if (c->references > 1) {
		c->references--;
		status = GPGPU_KMT_SUCCESS;
		goto out;
	}

	/* 1. 前提。 */
	if (c->calls || c->uncertain) {
		status = GPGPU_KMT_UNAVAILABLE;
		goto out;
	}

	/* 2. 回收。 */
	if (kmt_has_objects(c)) {
		c->closing = true;
		c->closer = pthread_self();
		pthread_mutex_unlock(&c->lock);
		status = kmt_release_objects(c);
		pthread_mutex_lock(&c->lock);
		c->closing = false;
		if (status != GPGPU_KMT_SUCCESS)
			goto out;
	}

	/* 3. 关闭。 */
	close(c->fd);
	free(c->path);
	c->fd = -1;
	c->path = NULL;
	c->references = 0;
	c->gpuvm = false;
	c->uncertain = false;
	status = GPGPU_KMT_SUCCESS;
out:
	pthread_mutex_unlock(&c->lock);
	return status;
}

GpgpuKmtStatus gpgpuKmtGetVmInfo(GpgpuKmtContext *context, struct gpgpu_vm_info *info)
{
	if (!info)
		return GPGPU_KMT_INVALID_PARAMETER;
	*info = (struct gpgpu_vm_info) { .version = GPGPU_DRM_VERSION };
	return kmt_call(context, GPGPU_DRM_INFO, info);
}

const char *gpgpuKmtStatusString(GpgpuKmtStatus status)
{
	static const char *const strings[] = {
		[GPGPU_KMT_SUCCESS] = "成功",
		[GPGPU_KMT_INVALID_PARAMETER] = "参数或对象非法",
		[GPGPU_KMT_VERSION_MISMATCH] = "版本不匹配",
		[GPGPU_KMT_NO_MEMORY] = "内存不足",
		[GPGPU_KMT_OUT_OF_RESOURCES] = "资源不足",
		[GPGPU_KMT_UNAVAILABLE] = "暂不可用",
		[GPGPU_KMT_NOT_SUPPORTED] = "不支持",
		[GPGPU_KMT_WAIT_TIMEOUT] = "等待超时",
		[GPGPU_KMT_WAIT_FAILURE] = "等待失败",
		[GPGPU_KMT_ERROR] = "失败",
	};

	return (unsigned int)status < sizeof(strings) / sizeof(strings[0]) ? strings[status] :
									      "未知状态";
}
