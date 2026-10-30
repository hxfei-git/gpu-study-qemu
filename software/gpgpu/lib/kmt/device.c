// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：连接的打开、关闭与公共辅助函数
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "kmt_internal.h"

/* 最后一次 Close 排空每条剩余队列的等待上限，单位毫秒。 */
#define KMT_CLOSE_DRAIN_TIMEOUT_MS 5000

/* 进程内唯一的连接；fd 为 -1 表示尚未打开。 */
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
 * gpgpuKmtOpen() - 打开或复用进程内唯一的设备连接
 *
 * 一个进程只有一个连接 kmt_connection，运行时和应用可以各自 Open，靠引用
 * 计数共享。整个过程持连接锁。
 *
 * 已打开时：同一进程、同一路径且连接不在关闭中才增加引用，否则返回
 * UNAVAILABLE。fork 出的子进程继承了 fd，但不能复用父进程的连接。
 *
 * 首次打开时：
 * 1. 以 O_CLOEXEC 打开 render node；
 * 2. 用 GPGPU_DRM_INFO 查询 GPUVM 能力，核对版本、页大小和地址位数，
 *    不支持时关闭 fd 并返回 VERSION_MISMATCH；
 * 3. 记录 fd、进程号和能力。range_count 为 1 表示 GPUVM 开启，之后由库
 *    挑选 GPUVA；为 0 时设备地址就是驱动返回的 GPU 物理地址。
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

	/* 已打开：只增加引用。 */
	if (c->fd >= 0) {
		if (c->closing || c->process != getpid() || strcmp(c->path, path))
			status = GPGPU_KMT_UNAVAILABLE;
		else
			c->references++;
		goto out;
	}

	/* 1. 打开 render node，exec 时自动关闭。 */
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		status = kmt_status(errno);
		goto out;
	}

	/* 2. 查询并核对能力，再复制路径；任一步失败都关闭 fd。 */
	if (ioctl(fd, GPGPU_DRM_INFO, &info) < 0)
		status = kmt_status(errno);
	else if (!kmt_vm_info_supported(&info))
		status = GPGPU_KMT_VERSION_MISMATCH;
	if (status == GPGPU_KMT_SUCCESS) {
		c->path = strdup(path);
		if (!c->path)
			status = GPGPU_KMT_NO_MEMORY;
	}
	if (status != GPGPU_KMT_SUCCESS) {
		close(fd);
		goto out;
	}

	/* 3. 记录连接状态；是否使用 GPUVM 由可分配区间数决定。 */
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

/*
 * 最后一次 Close 时回收剩余对象，顺序由对象之间的依赖决定：
 * 1. 队列先排空再卸载。排空返回 WAIT_FAILURE（队列已出错）时也继续卸载；
 * 2. 再销毁事件。驱动只在来源队列已卸载或事件已通知时允许销毁；
 * 3. 最后释放内存。队列存活期间地址空间冻结，内存不能先于队列释放。
 * 任一步失败就停下并返回，剩余对象留在链表中。
 */
static GpgpuKmtStatus kmt_release_objects(GpgpuKmtContext *c)
{
	GpgpuKmtStatus status;

	/* 1. 销毁成功的队列会从链表摘除，所以总是处理表头。 */
	while (c->queues) {
		status = gpgpuKmtDrainQueue(c->queues, KMT_CLOSE_DRAIN_TIMEOUT_MS);
		if (status != GPGPU_KMT_SUCCESS && status != GPGPU_KMT_WAIT_FAILURE)
			return status;
		status = gpgpuKmtDestroyQueue(c->queues);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}

	/* 2. 队列都已卸载，事件不会再收到通知。 */
	while (c->events) {
		status = gpgpuKmtDestroyEvent(&c->events->pub);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}

	/* 3. 地址空间已解冻，释放全部内存对象。 */
	while (c->memory) {
		status = gpgpuKmtFreeMemory(&c->memory->pub);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
	return GPGPU_KMT_SUCCESS;
}

/* 连接上还有未回收的内存、队列或事件。调用者持 lock。 */
static bool kmt_has_objects(GpgpuKmtContext *c)
{
	return c->memory || c->queues || c->events;
}

/*
 * gpgpuKmtClose() - 放下一次 Open 的引用
 *
 * 不是最后一次引用时只减引用。最后一次：
 * 1. 其他线程仍有库调用进行中，或存在结果未知、驱动仍在保留的
 *    资源时，返回 UNAVAILABLE，连接保持打开；
 * 2. 标记 closing 并记下当前线程，然后解锁回收剩余对象。回收函数会再次
 *    kmt_enter() 本连接，只有记下的线程能通过，其他线程的新调用被拒绝。
 *    回收失败时清除 closing 并返回错误；
 * 3. 全部回收后关闭 fd、清空连接，之后可以重新 Open。
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

	/* 1. 有并发调用或结果未知的资源时不能关闭。 */
	if (c->calls || c->uncertain) {
		status = GPGPU_KMT_UNAVAILABLE;
		goto out;
	}

	/* 2. 回收函数会重新进入本连接并加锁，所以先解锁。 */
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

	/* 3. 关闭 fd 并复位连接。 */
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
