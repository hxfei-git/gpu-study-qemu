/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 独立的客体库，本文件不依赖 QEMU 头文件。 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "gpgpu.h"

/**
 * struct GpgpuKernel - 同步运行时装载的 RV32 代码对象
 * @address: 代码入口在 VRAM 中的字节偏移。
 * @bytes: 上传的代码字节数。
 * @next: 同一运行时上下文中的下一个 Kernel。
 */
struct GpgpuKernel {
	GpgpuDevicePtr address;
	uint32_t bytes;
	struct GpgpuKernel *next;
};

/**
 * struct GpgpuContext - 同步运行时的独占连接、暂存区与 Kernel 链表
 * @fd: 设备文件描述符。
 * @staging: 用户态映射的 DMA 暂存区。
 * @mutex: 串行保护暂存区、ioctl 和 Kernel 链表操作。
 * @info: 初始化时保存的设备能力与暂存区大小。
 * @kernels: 本上下文装载的 Kernel 链表。
 */
struct GpgpuContext {
	int fd;
	void *staging;
	pthread_mutex_t mutex;
	struct gpgpu_info info;
	GpgpuKernel *kernels;
};

/*
 * request() - 执行同步运行时的一次 ioctl
 *
 * 将系统调用失败转换为负 errno，成功返回零。
 */
static int request(GpgpuContext *ctx, unsigned long command, void *arg)
{
	int rc;

	do {
		rc = ioctl(ctx->fd, command, arg);
	} while (rc < 0 && errno == EINTR);
	return rc < 0 ? -errno : 0;
}

/*
 * lock_context() - 校验并锁定运行时上下文
 *
 * 拒绝空指针，将 pthread 锁错误转换为负错误码。
 */
static int lock_context(GpgpuContext *ctx)
{
	return ctx ? -pthread_mutex_lock(&ctx->mutex) : -EINVAL;
}

/*
 * allocate() - 向驱动申请同步 VRAM 字节范围
 *
 * 调用方已持上下文锁；检查长度能由 ABI 的 32 位字段表示。
 */
static int allocate(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes)
{
	struct gpgpu_alloc allocation = { .size = bytes };
	int rc;

	if (!out || !bytes || bytes > UINT32_MAX)
		return -EINVAL;
	rc = request(ctx, GPGPU_IOC_ALLOC, &allocation);
	if (!rc)
		*out = allocation.address;
	return rc;
}

/*
 * copy() - 借助暂存区完成任意长度的同步拷贝
 *
 * 调用方已持上下文锁；按暂存区容量分块，
 * 每块 DMA 完成后再复用缓冲区。
 */
static int copy(GpgpuContext *ctx, GpgpuDevicePtr address, void *host, size_t bytes,
		GpgpuMemcpyKind kind)
{
	struct gpgpu_copy transfer = { .direction = kind };
	size_t offset = 0;
	int rc;

	if ((!host && bytes) || bytes > UINT32_MAX || bytes > UINT32_MAX - address ||
	    (kind != GPGPU_MEMCPY_HOST_TO_DEVICE && kind != GPGPU_MEMCPY_DEVICE_TO_HOST)) {
		return -EINVAL;
	}
	while (offset < bytes) {
		size_t remaining = bytes - offset;

		transfer.address = address + offset;
		transfer.size =
			remaining < ctx->info.staging_size ? remaining : ctx->info.staging_size;
		if (kind == GPGPU_MEMCPY_HOST_TO_DEVICE)
			memcpy(ctx->staging, (char *)host + offset, transfer.size);
		rc = request(ctx, GPGPU_IOC_COPY, &transfer);
		if (rc)
			return rc;
		if (kind == GPGPU_MEMCPY_DEVICE_TO_HOST)
			memcpy((char *)host + offset, ctx->staging, transfer.size);
		offset += transfer.size;
	}
	return 0;
}

/*
 * gpgpuInit() - 建立同步运行时上下文
 *
 * 打开设备并查询 ABI，随后映射 DMA 暂存区和初始化互斥锁。
 */
int gpgpuInit(GpgpuContext **out, const char *device)
{
	GpgpuContext *ctx;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->fd = open(device ? device : "/dev/gpgpu0", O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0) {
		rc = -errno;
		goto fail;
	}
	rc = request(ctx, GPGPU_IOC_INFO, &ctx->info);
	if (rc)
		goto fail_fd;
	if (ctx->info.abi_version != GPGPU_ABI_VERSION ||
	    ctx->info.staging_size != GPGPU_STAGING_SIZE) {
		rc = -EPROTO;
		goto fail_fd;
	}
	ctx->staging =
		mmap(NULL, ctx->info.staging_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
	if (ctx->staging == MAP_FAILED) {
		rc = -errno;
		goto fail_fd;
	}
	rc = pthread_mutex_init(&ctx->mutex, NULL);
	if (rc) {
		munmap(ctx->staging, ctx->info.staging_size);
		rc = -rc;
		goto fail_fd;
	}
	*out = ctx;
	return 0;

fail_fd:
	close(ctx->fd);
fail:
	free(ctx);
	return rc;
}

/*
 * gpgpuDestroy() - 销毁同步运行时上下文
 *
 * 调用方须先结束其他线程的访问；释放 Kernel，再解除映射并关闭设备。
 */
void gpgpuDestroy(GpgpuContext *ctx)
{
	GpgpuKernel *kernel;

	if (!ctx)
		return;
	while ((kernel = ctx->kernels)) {
		ctx->kernels = kernel->next;
		free(kernel);
	}
	munmap(ctx->staging, ctx->info.staging_size);
	close(ctx->fd); /* 驱动释放本会话所属的全部显存分配。 */
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

/*
 * gpgpuMalloc() - 线程安全地申请同步 VRAM
 *
 * 持上下文锁调用分配接口，返回设备字节偏移及零或负 errno。
 */
int gpgpuMalloc(GpgpuContext *ctx, GpgpuDevicePtr *out, size_t bytes)
{
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	rc = allocate(ctx, out, bytes);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * gpgpuFree() - 线程安全地释放同步 VRAM
 *
 * 持上下文锁按分配起始偏移调用驱动，不接受内部地址。
 */
int gpgpuFree(GpgpuContext *ctx, GpgpuDevicePtr address)
{
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	rc = request(ctx, GPGPU_IOC_FREE, &address);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * gpgpuMemcpy() - 在应用缓冲区与同步 VRAM 之间拷贝
 *
 * 持上下文锁保护共用暂存区，通过分块 DMA 完成后返回。
 */
int gpgpuMemcpy(GpgpuContext *ctx, GpgpuDevicePtr device_address, void *host, size_t bytes,
		GpgpuMemcpyKind kind)
{
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	rc = copy(ctx, device_address, host, bytes, kind);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * gpgpuLoadKernel() - 装载 RV32 代码并登记 Kernel 对象
 *
 * 校验指令长度后分配 VRAM 并上传，只有全部成功才加入上下文链表。
 */
int gpgpuLoadKernel(GpgpuContext *ctx, GpgpuKernel **out, const void *code, size_t bytes)
{
	GpgpuKernel *kernel;
	int rc;

	if (!out || !code || !bytes || bytes % 4 || bytes > UINT32_MAX)
		return -EINVAL;
	*out = NULL;
	kernel = calloc(1, sizeof(*kernel));
	if (!kernel)
		return -ENOMEM;
	rc = lock_context(ctx);
	if (rc) {
		free(kernel);
		return rc;
	}
	rc = allocate(ctx, &kernel->address, bytes);
	if (!rc) {
		rc = copy(ctx, kernel->address, (void *)code, bytes, GPGPU_MEMCPY_HOST_TO_DEVICE);
		if (rc)
			request(ctx, GPGPU_IOC_FREE, &kernel->address);
	}
	if (!rc) {
		kernel->bytes = bytes;
		kernel->next = ctx->kernels;
		ctx->kernels = kernel;
		*out = kernel;
	} else {
		free(kernel);
	}
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * find_kernel() - 定位本上下文中的 Kernel 链表项
 *
 * 调用方持上下文锁，返回指向链表链接的指针以便安全摘除。
 */
static GpgpuKernel **find_kernel(GpgpuContext *ctx, GpgpuKernel *kernel)
{
	GpgpuKernel **cursor;

	for (cursor = &ctx->kernels; *cursor; cursor = &(*cursor)->next)
		if (*cursor == kernel)
			return cursor;
	return NULL;
}

/*
 * gpgpuUnloadKernel() - 卸载所属上下文的 Kernel
 *
 * 驱动成功释放代码 VRAM 后才摘链并释放用户态对象。
 */
int gpgpuUnloadKernel(GpgpuContext *ctx, GpgpuKernel *kernel)
{
	GpgpuKernel **cursor;
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	cursor = find_kernel(ctx, kernel);
	if (!cursor) {
		rc = -EINVAL;
	} else {
		rc = request(ctx, GPGPU_IOC_FREE, &kernel->address);
		if (!rc) {
			*cursor = kernel->next;
			free(kernel);
		}
	}
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * validate_dimensions() - 检查三维 grid/block 是否满足同步接口限制
 *
 * 逐维检查非零和乘积上限，以先除后乘避免截断掩盖超限。
 */
static int validate_dimensions(GpgpuDim3 grid, GpgpuDim3 block)
{
	uint32_t grids[] = { grid.x, grid.y, grid.z };
	uint32_t blocks[] = { block.x, block.y, block.z };
	uint64_t block_count = 1, thread_count = 1;
	unsigned int i;

	for (i = 0; i < 3; i++) {
		if (!grids[i] || !blocks[i] || grids[i] > GPGPU_MAX_BLOCKS / block_count ||
		    blocks[i] > GPGPU_MAX_BLOCK_THREADS / thread_count) {
			return -EINVAL;
		}
		block_count *= grids[i];
		thread_count *= blocks[i];
	}
	return block_count * thread_count > GPGPU_MAX_THREADS ? -EINVAL : 0;
}

/*
 * gpgpuLaunchKernel() - 同步上传参数并执行指定 Kernel
 *
 * 持上下文锁确认 Kernel 归属，临时申请参数 VRAM，执行结束后回收。
 */
int gpgpuLaunchKernel(GpgpuContext *ctx, GpgpuKernel *kernel, GpgpuDim3 grid, GpgpuDim3 block,
		      const void *args, size_t args_bytes, uint32_t shared_bytes)
{
	uint32_t empty_args = 0;
	size_t transfer_bytes = args_bytes ? args_bytes : sizeof(empty_args);
	struct gpgpu_launch launch = {
		.grid = { grid.x, grid.y, grid.z },
		.block = { block.x, block.y, block.z },
		.args_size = transfer_bytes,
		.shared_size = shared_bytes,
	};
	int rc = lock_context(ctx);
	int free_rc;

	if (rc)
		return rc;
	if (!find_kernel(ctx, kernel) || (!args && args_bytes) || args_bytes > UINT32_MAX ||
	    args_bytes % 4) {
		rc = -EINVAL;
		goto out;
	}
	rc = validate_dimensions(grid, block);
	if (rc)
		goto out;
	if (shared_bytes) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	launch.code = kernel->address;
	launch.code_size = kernel->bytes;
	rc = allocate(ctx, &launch.args, transfer_bytes);
	if (rc)
		goto out;
	rc = copy(ctx, launch.args, args_bytes ? (void *)args : &empty_args, transfer_bytes,
		  GPGPU_MEMCPY_HOST_TO_DEVICE);
	if (!rc)
		rc = request(ctx, GPGPU_IOC_LAUNCH, &launch);
	free_rc = request(ctx, GPGPU_IOC_FREE, &launch.args);
	if (!rc)
		rc = free_rc;
out:
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * gpgpuDeviceSynchronize() - 等待同一上下文的同步操作退出
 *
 * 取得互斥锁即表明此前持锁的 DMA/派发调用已完成，本版没有后台队列。
 */
int gpgpuDeviceSynchronize(GpgpuContext *ctx)
{
	int rc = lock_context(ctx);

	if (!rc) {
		/* DMA 和内核执行均在对应 ioctl 返回前完成。 */
		pthread_mutex_unlock(&ctx->mutex);
	}
	return rc;
}

/*
 * gpgpuGetInfo() - 查询同步设备能力
 *
 * 持上下文锁发起 INFO ioctl，并检查输出指针。
 */
int gpgpuGetInfo(GpgpuContext *ctx, struct gpgpu_info *info)
{
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	rc = info ? request(ctx, GPGPU_IOC_INFO, info) : -EINVAL;
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

/*
 * gpgpuGetStats() - 读取同步设备的中断统计
 *
 * 持上下文锁获取驱动保存的计算、DMA 和错误计数。
 */
int gpgpuGetStats(GpgpuContext *ctx, struct gpgpu_stats *stats)
{
	int rc = lock_context(ctx);

	if (rc)
		return rc;
	rc = stats ? request(ctx, GPGPU_IOC_STATS, stats) : -EINVAL;
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}
