/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_KMT_H
#define GPGPU_KMT_H

#include <stddef.h>
#include <stdint.h>
#include "gpgpu_uapi.h"
#include "gpgpu_queue.h"

typedef struct GpgpuKmtContext GpgpuKmtContext;
/**
 * enum GpgpuKmtStatus - 用户态库返回的 HSAKMT 风格状态
 * @GPGPU_KMT_SUCCESS: 操作成功。
 * @GPGPU_KMT_INVALID_PARAMETER: 参数、句柄或上下文非法。
 * @GPGPU_KMT_VERSION_MISMATCH: 请求版本与驱动 ABI 不符。
 * @GPGPU_KMT_NO_MEMORY: Host 或 VRAM 分配失败。
 * @GPGPU_KMT_OUT_OF_RESOURCES: 队列名额或其他编号资源耗尽。
 * @GPGPU_KMT_UNAVAILABLE: 对象仍被引用或当前暂不能操作。
 * @GPGPU_KMT_NOT_SUPPORTED: 当前模式或设备不支持所请求功能。
 * @GPGPU_KMT_WAIT_TIMEOUT: 本次等待到期，不代表设备访问已经结束。
 * @GPGPU_KMT_WAIT_FAILURE: 源队列或设备错误导致等待失败。
 * @GPGPU_KMT_ERROR: 不能归入上述类别的错误。
 */
typedef enum GpgpuKmtStatus {
	GPGPU_KMT_SUCCESS,
	GPGPU_KMT_INVALID_PARAMETER,
	GPGPU_KMT_VERSION_MISMATCH,
	GPGPU_KMT_NO_MEMORY,
	GPGPU_KMT_OUT_OF_RESOURCES,
	GPGPU_KMT_UNAVAILABLE,
	GPGPU_KMT_NOT_SUPPORTED,
	GPGPU_KMT_WAIT_TIMEOUT,
	GPGPU_KMT_WAIT_FAILURE,
	GPGPU_KMT_ERROR,
} GpgpuKmtStatus;

/**
 * struct GpgpuKmtMemory - 用户态内存对象，区分 CPU 映射与设备可用地址
 * @context: 所属共享连接，对象销毁前连接必须存活。
 * @handle: 驱动分配的会话内句柄。
 * @kind: GPGPU_MEMORY_HOST 或 GPGPU_MEMORY_VRAM。
 * @vram_offset: VRAM 对象的字节偏移；Host 对象为零。
 * @size: 实际分配的字节数，可能大于请求长度。
 * @host_address: Host 对象的完整物理地址；不是 CPU 指针。
 * @mmap_offset: 传给 mmap 的字节偏移令牌；VRAM 对象不映射。
 * @cpu: Host 对象的用户态映射；VRAM 对象为 NULL。
 * @queue_users: 库内队列持有的引用数，由 gpgpu_kmt_lock 保护。
 */
typedef struct GpgpuKmtMemory {
	GpgpuKmtContext *context;
	uint32_t handle;
	uint32_t kind;
	uint32_t vram_offset;
	uint64_t size;
	uint64_t host_address;
	uint64_t mmap_offset;
	void *cpu;
	unsigned int queue_users;
} GpgpuKmtMemory;

/**
 * struct GpgpuKmtQueue - 用户态 AQL 队列及其 Ring、进度和 Doorbell 映射
 * @context: 所属共享连接。
 * @id: 驱动分配的会话内队列编号。
 * @ring_size: AQL Ring 的字节容量，必须为 2 的幂。
 * @ring_memory: 保存 Ring 的内存对象，队列存活时保留引用。
 * @progress_memory: 保存 R/W 的内存对象，队列存活时保留引用。
 * @ring: 用户态 AQL 槽数组，不是 Host 物理地址。
 * @rptr: CP 写回的下一消费编号，按完整 32 位读取。
 * @wptr: 生产者共享的下一预留编号，使用 32 位原子取号。
 * @doorbell: 映射后的 8 字节 MMIO 入口，发布后写包编号。
 * @doorbell_mapping: Doorbell 整页映射的起始地址，用于 munmap。
 * @doorbell_map_size: Doorbell 映射的字节长度。
 */
typedef struct GpgpuKmtQueue {
	GpgpuKmtContext *context;
	uint32_t id;
	uint32_t ring_size;
	GpgpuKmtMemory *ring_memory;
	GpgpuKmtMemory *progress_memory;
	GpgpuAqlPacket *ring;
	uint32_t *rptr;
	uint32_t *wptr;
	volatile uint64_t *doorbell;
	void *doorbell_mapping;
	size_t doorbell_map_size;
} GpgpuKmtQueue;

/**
 * struct GpgpuKmtEvent - 与一个源队列绑定的完成通知对象
 * @context: 所属共享连接。
 * @id: 会话内非零事件编号，存活期间不复用。
 * @queue_id: 产生此事件通知的队列编号。
 * @mailbox_address: 驱动分配的 8 字节通知槽的 Host 物理地址。
 */
typedef struct GpgpuKmtEvent {
	GpgpuKmtContext *context;
	uint32_t id;
	uint32_t queue_id;
	uint64_t mailbox_address;
} GpgpuKmtEvent;

/*
 * 同进程重复打开共享连接并计数；
 * 最后关闭要求所有对象及调用均已结束。
 */
/* 打开或引用进程内共享设备连接。 */
GpgpuKmtStatus gpgpuKmtOpen(const char *device, GpgpuKmtContext **context);
/* 释放一个共享连接引用。 */
GpgpuKmtStatus gpgpuKmtClose(GpgpuKmtContext *context);
/* 查询队列模式的设备能力。 */
GpgpuKmtStatus gpgpuKmtGetInfo(GpgpuKmtContext *context, struct gpgpu_queue_info *info);
/* 取得库状态码的中文说明。 */
const char *gpgpuKmtStatusName(GpgpuKmtStatus status);

/*
 * 成功返回句柄及独立的 CPU/Host 物理地址/VRAM 偏移；释放前停止所有使用。
 */
/* 申请 Host 或 VRAM 内存对象。 */
GpgpuKmtStatus gpgpuKmtAllocMemory(GpgpuKmtContext *context, uint32_t kind, uint64_t size,
				   uint64_t alignment, GpgpuKmtMemory *memory);
/* 释放不再使用的内存对象。 */
GpgpuKmtStatus gpgpuKmtFreeMemory(GpgpuKmtMemory *memory);
/* 在 Host 与 VRAM 对象间同步拷贝。 */
GpgpuKmtStatus gpgpuKmtCopyMemory(GpgpuKmtMemory *host, uint64_t host_offset, GpgpuKmtMemory *vram,
				  uint64_t vram_offset, uint64_t size, uint32_t direction);

/*
 *
 * 创建使用内存句柄和对象内偏移；销毁前由调用方停止生产并排空。
 * 创建后 CPU 映射失败且卸载未确认时，失败输出不含有效队列，内部
 * 引用继续阻止释放和最后关闭；本版不自动复位恢复这些资源。
 */
/* 用已有 Ring 和进度对象建立 AQL 队列。 */
GpgpuKmtStatus gpgpuKmtCreateQueue(GpgpuKmtContext *context, GpgpuKmtMemory *ring,
				   uint64_t ring_offset, uint32_t ring_size,
				   GpgpuKmtMemory *progress, uint64_t rptr_offset,
				   uint64_t wptr_offset, GpgpuKmtQueue *queue);
/* 查询队列状态和最近管理结果。 */
GpgpuKmtStatus gpgpuKmtQueryQueue(GpgpuKmtQueue *queue, struct gpgpu_queue_state *state);
/* 卸载队列并释放用户态映射。 */
GpgpuKmtStatus gpgpuKmtDestroyQueue(GpgpuKmtQueue *queue);

/* 事件等待只调用内核事件接口；Signal 主动检查由实验调用方负责。 */
/* 为源队列创建完成事件。 */
GpgpuKmtStatus gpgpuKmtCreateEvent(GpgpuKmtQueue *queue, GpgpuKmtEvent *event);
/* 查询驱动已保存的事件状态。 */
GpgpuKmtStatus gpgpuKmtQueryEvent(GpgpuKmtEvent *event, struct gpgpu_event_request *state);
/* 等待驱动事件状态改变。 */
GpgpuKmtStatus gpgpuKmtWaitEvent(GpgpuKmtEvent *event, uint32_t timeout_ms);
/* 销毁通知事件并释放库对象引用。 */
GpgpuKmtStatus gpgpuKmtDestroyEvent(GpgpuKmtEvent *event);
#endif
