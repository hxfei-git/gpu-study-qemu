/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU KMT 内部：连接、对象与公共辅助函数
 *
 * 进程内只有一个连接（kmt_connection）。连接的 lock 保护 references、calls、
 * closing、uncertain 与三条对象链表；ioctl 本身不持锁，由 calls 计数阻止
 * 最后一次 Close 与进行中的调用并发。
 */
#ifndef GPGPU_KMT_INTERNAL_H
#define GPGPU_KMT_INTERNAL_H

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "gpgpu_kmt.h"

/* GPUVM 页大小与 DRM 页大小一致。 */
#define KMT_PAGE_SIZE 4096u
/* 默认 render node。 */
#define KMT_DEFAULT_DEVICE "/dev/dri/renderD128"

/**
 * struct KmtMemory - 内存对象
 * @pub: 公开信息，返回给调用者的指针指向这里。
 * @context: 所属连接。
 * @next: 连接 memory 链表的下一项。
 * @handle: GEM 句柄。
 * @bound: 仍持有 GPU 绑定；释放时解绑成功后清除，失败重试不会重复解绑。
 * @queue_users: 把本对象用作 Ring 或进度的存活队列数；非零时不能释放。
 */
typedef struct KmtMemory {
	GpgpuKmtMemory pub;
	GpgpuKmtContext *context;
	struct KmtMemory *next;
	uint32_t handle;
	bool bound;
	unsigned int queue_users;
} KmtMemory;

/**
 * struct GpgpuKmtQueue - 用户 AQL 队列
 * @context: 所属连接。
 * @next: 连接 queues 链表的下一项。
 * @id: 驱动分配的队列编号。
 * @ring_memory: Ring 所在对象。
 * @progress_memory: 进度所在对象。
 * @ring: Ring 的用户态映射。
 * @slots: Ring 槽数，2 的幂。
 * @rptr: CP 写回的 R。
 * @wptr: 生产者原子递增的 W。
 * @doorbell: Doorbell 入口。
 * @doorbell_page: Doorbell 整页映射，用于 munmap。
 * @doorbell_page_size: Doorbell 映射长度。
 */
struct GpgpuKmtQueue {
	GpgpuKmtContext *context;
	struct GpgpuKmtQueue *next;
	uint32_t id;
	KmtMemory *ring_memory;
	KmtMemory *progress_memory;
	GpgpuAqlPacket *ring;
	uint32_t slots;
	uint32_t *rptr;
	uint32_t *wptr;
	volatile uint64_t *doorbell;
	void *doorbell_page;
	size_t doorbell_page_size;
};

/**
 * struct KmtEvent - 完成事件
 * @pub: 公开信息。
 * @context: 所属连接。
 * @next: 连接 events 链表的下一项。
 */
typedef struct KmtEvent {
	GpgpuKmtEvent pub;
	GpgpuKmtContext *context;
	struct KmtEvent *next;
} KmtEvent;

/**
 * struct GpgpuKmtContext - 进程内共享的设备连接
 * @lock: 保护下列除 fd、path、process、vm_info 外的字段。
 * @fd: render node 文件描述符；-1 表示未打开。
 * @path: 打开的路径；重复打开须路径一致。
 * @process: 打开连接的进程；fork 后的子进程不能复用。
 * @references: Open 成功且尚未 Close 的次数。
 * @calls: 正在执行的库调用数。
 * @closing: 最后一次 Close 正在回收对象。
 * @closer: 执行回收的线程；它自己仍可调用销毁函数。
 * @uncertain: 存在结果未知、驱动仍在保留的资源；Close 不能报告成功。
 * @gpuvm: 设备以 GPUVM 模式运行。
 * @vm_info: 打开时查询到的 GPUVM 能力与可分配区间。
 * @memory: 内存对象链表。
 * @queues: 队列链表。
 * @events: 事件链表。
 */
struct GpgpuKmtContext {
	pthread_mutex_t lock;
	int fd;
	char *path;
	pid_t process;
	unsigned int references;
	unsigned int calls;
	bool closing;
	pthread_t closer;
	bool uncertain;
	bool gpuvm;
	struct gpgpu_vm_info vm_info;
	KmtMemory *memory;
	GpgpuKmtQueue *queues;
	KmtEvent *events;
};

static inline KmtMemory *kmt_memory(GpgpuKmtMemory *pub)
{
	return pub ? (KmtMemory *)((char *)pub - offsetof(KmtMemory, pub)) : NULL;
}

static inline KmtEvent *kmt_event(GpgpuKmtEvent *pub)
{
	return pub ? (KmtEvent *)((char *)pub - offsetof(KmtEvent, pub)) : NULL;
}

/*
 * KMT_UNLINK() - 从单向链表中摘除一项
 * @head: 链表头指针的地址。
 * @item: 要摘除的项；不在链表中时不做任何事。
 */
#define KMT_UNLINK(head, item)                                              \
	do {                                                                \
		__typeof__(*(head)) *link_ = (head);                        \
		while (*link_ && *link_ != (item))                          \
			link_ = &(*link_)->next;                            \
		if (*link_)                                                 \
			*link_ = (item)->next;                              \
	} while (0)

/* 把 errno 映射为库状态。 */
GpgpuKmtStatus kmt_status(int error);
/* 登记一次库调用；连接无效、属于其他进程或正在被其他线程关闭时失败。 */
GpgpuKmtStatus kmt_enter(GpgpuKmtContext *context);
/* 结束一次库调用。 */
void kmt_leave(GpgpuKmtContext *context);
/* 发起 ioctl，失败时返回映射后的状态。调用者已 kmt_enter()。 */
GpgpuKmtStatus kmt_ioctl(GpgpuKmtContext *context, unsigned long request, void *args);
/* 一次完整调用：kmt_enter()、kmt_ioctl()、kmt_leave()。 */
GpgpuKmtStatus kmt_call(GpgpuKmtContext *context, unsigned long request, void *args);
/* 标记存在结果未知的资源。 */
void kmt_mark_uncertain(GpgpuKmtContext *context);
/* [offset, offset + size) 非空且位于对象请求长度内。 */
bool kmt_range_ok(KmtMemory *m, uint64_t offset, uint64_t size);

#endif /* GPGPU_KMT_INTERNAL_H */
