/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_KMT_INTERNAL_H
#define GPGPU_KMT_INTERNAL_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "gpgpu_kmt.h"

/**
 * struct GpgpuKmtContext - 同一进程共享的设备连接，由 gpgpu_kmt_lock 保护
 * @fd: 设备文件描述符；未打开时为 -1。
 * @process: 创建连接的进程号，用于拒绝 fork 后复用旧上下文。
 * @references: 成功打开且尚未关闭的连接引用数。
 * @objects: 尚未释放的内存、队列和事件总数。
 * @calls: 正在执行的库调用数，防止最后关闭与调用并发。
 * @info: 首次打开时查询到的队列 ABI 信息。
 * @path: 打开的设备路径，用于核对重复打开的目标。
 */
struct GpgpuKmtContext {
	int fd;
	pid_t process;
	unsigned int references;
	unsigned int objects;
	unsigned int calls;
	struct gpgpu_queue_info info;
	char *path;
};
extern pthread_mutex_t gpgpu_kmt_lock;
/* 将正 errno 转换为库状态码。 */
GpgpuKmtStatus gpgpu_kmt_error(int error);
/* 为一次库调用持有连接使用计数。 */
GpgpuKmtStatus gpgpu_kmt_begin(GpgpuKmtContext *context);
/* 结束一次已登记的库调用。 */
void gpgpu_kmt_end(GpgpuKmtContext *context);
/* 调整连接持有的对象计数。 */
void gpgpu_kmt_objects(GpgpuKmtContext *context, int change);
/* 执行一次带连接保护的 ioctl。 */
GpgpuKmtStatus gpgpu_kmt_ioctl(GpgpuKmtContext *context, unsigned long command, void *request);
/*
 * gpgpu_kmt_range() - 校验内存对象中的一个非空范围
 *
 * 要求对象仍属于连接，并用减法检查偏移，避免加法溢出。
 */
static inline bool gpgpu_kmt_range(GpgpuKmtMemory *m, uint64_t off, uint64_t size)
{
	return m && m->context && size && size <= m->size && off <= m->size - size;
}
#endif
