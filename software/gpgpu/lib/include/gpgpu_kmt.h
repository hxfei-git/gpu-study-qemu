/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU KMT：HSAKMT 风格的薄用户态接口
 *
 * 每个函数基本对应一个驱动 ioctl，只做参数检查、用户态映射和对象登记，
 * 不隐藏提交与等待的细节。对象由库分配，调用者只读其中的公开字段，用对应
 * 的 Free/Destroy 函数释放。
 *
 * 使用顺序（地址空间在队列存活期间冻结）：
 *
 *   Open → AllocMemory（全部对象）→ CreateQueue → CreateEvent
 *        → Reserve → Publish → WaitSignal（可重复）
 *        → DrainQueue → DestroyQueue → DestroyEvent → FreeMemory → Close
 *
 * 地址：gpu_address 是写进 AQL 包、Signal 与参数块的设备地址；GPUVM 开启时
 * 为 GPUVA，关闭时为 GPU 物理地址。应用无需区分两种模式。
 */
#ifndef GPGPU_KMT_H
#define GPGPU_KMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gpgpu/gpgpu_queue_abi.h"
#include "gpgpu/gpgpu_vm_abi.h"
#include "gpgpu/uapi/gpgpu_drm.h"

/**
 * enum GpgpuKmtStatus - 库返回的状态
 * @GPGPU_KMT_SUCCESS: 成功。
 * @GPGPU_KMT_INVALID_PARAMETER: 参数、对象或上下文非法。
 * @GPGPU_KMT_VERSION_MISMATCH: 驱动接口版本或能力不符。
 * @GPGPU_KMT_NO_MEMORY: Host、VRAM 或库内存分配失败。
 * @GPGPU_KMT_OUT_OF_RESOURCES: 队列名额、VMID、GPUVA 或编号耗尽。
 * @GPGPU_KMT_UNAVAILABLE: 对象仍被使用，或当前状态不允许该操作。
 * @GPGPU_KMT_NOT_SUPPORTED: 当前设备模式不支持该功能。
 * @GPGPU_KMT_WAIT_TIMEOUT: 等待到期；不代表设备已停止访问。
 * @GPGPU_KMT_WAIT_FAILURE: 来源队列或设备出错。
 * @GPGPU_KMT_ERROR: 其他错误。
 */
enum GpgpuKmtStatus {
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
};
typedef enum GpgpuKmtStatus GpgpuKmtStatus;

/* 进程内共享的设备连接。 */
typedef struct GpgpuKmtContext GpgpuKmtContext;
/* 用户 AQL 队列。 */
typedef struct GpgpuKmtQueue GpgpuKmtQueue;

/**
 * struct GpgpuKmtMemory - 内存对象的公开信息（只读）
 * @kind: GPGPU_MEMORY_HOST 或 GPGPU_MEMORY_VRAM。
 * @size: 请求的字节数。
 * @mapped_size: 页对齐后的字节数，即 CPU 映射与 GPU 绑定的长度。
 * @cpu: Host 对象的用户态映射；VRAM 对象为 NULL，经 CopyMemory 访问。
 * @gpu_address: 设备地址。
 */
typedef struct GpgpuKmtMemory {
	uint32_t kind;
	uint64_t size;
	uint64_t mapped_size;
	void *cpu;
	uint64_t gpu_address;
} GpgpuKmtMemory;

/**
 * struct GpgpuKmtEvent - 完成事件的公开信息（只读）
 * @id: 写进 Signal.event_id 的事件编号。
 * @mailbox_address: 写进 Signal.event_mailbox_ptr 的通知槽设备地址。
 */
typedef struct GpgpuKmtEvent {
	uint32_t id;
	uint64_t mailbox_address;
} GpgpuKmtEvent;

/* ---- 连接 ---- */

/*
 * 打开 render node（NULL 表示 /dev/dri/renderD128）。同一进程重复打开同一路径
 * 只增加引用；最后一次 Close 按队列、事件、内存的顺序回收剩余对象。
 */
GpgpuKmtStatus gpgpuKmtOpen(const char *path, GpgpuKmtContext **context);
GpgpuKmtStatus gpgpuKmtClose(GpgpuKmtContext *context);
/* 查询 GPUVM 能力：range_count 为 1 表示 GPUVM 开启，0 表示关闭。 */
GpgpuKmtStatus gpgpuKmtGetVmInfo(GpgpuKmtContext *context, struct gpgpu_vm_info *info);
/* 状态的中文说明。 */
const char *gpgpuKmtStatusString(GpgpuKmtStatus status);

/* ---- 内存 ---- */

/*
 * 创建并绑定一个整对象。permissions 为 GPGPU_VM_READ/WRITE/EXEC 的组合；
 * fixed_va 为 0 时由库在可分配区间中挑选 GPUVA，GPUVM 关闭时必须为 0。
 * 队列存活期间不能创建或释放内存。
 */
GpgpuKmtStatus gpgpuKmtAllocMemory(GpgpuKmtContext *context, uint32_t kind, uint64_t size,
				   uint64_t alignment, uint32_t permissions, uint64_t fixed_va,
				   GpgpuKmtMemory **memory);
GpgpuKmtStatus gpgpuKmtFreeMemory(GpgpuKmtMemory *memory);
/* 在 Host 对象与 VRAM 对象之间同步搬运；direction 为 GPGPU_COPY_H2D 或 D2H。 */
GpgpuKmtStatus gpgpuKmtCopyMemory(GpgpuKmtMemory *host, uint64_t host_offset,
				  GpgpuKmtMemory *vram, uint64_t vram_offset, uint64_t size,
				  uint32_t direction);

/* ---- 队列 ---- */

/*
 * 用 ring 中 [ring_offset, ring_offset + ring_size) 作为 AQL Ring，progress 中
 * 两个 8 字节对齐的位置作为 R 与 W。驱动把 Ring 全部置为 INVALID。
 * 创建失败且结果未知时，库保留相关资源，Close 返回 UNAVAILABLE。
 */
GpgpuKmtStatus gpgpuKmtCreateQueue(GpgpuKmtContext *context, GpgpuKmtMemory *ring,
				   uint64_t ring_offset, uint32_t ring_size,
				   GpgpuKmtMemory *progress, uint64_t rptr_offset,
				   uint64_t wptr_offset, GpgpuKmtQueue **queue);
GpgpuKmtStatus gpgpuKmtQueryQueue(GpgpuKmtQueue *queue, struct gpgpu_queue_state *state);
/* 读取队列的首次 GPUVM 故障。 */
GpgpuKmtStatus gpgpuKmtGetVmFault(GpgpuKmtQueue *queue, struct gpgpu_vm_fault *fault);
/* 调用者已停止提交；队列须已排空或已出错。 */
GpgpuKmtStatus gpgpuKmtDestroyQueue(GpgpuKmtQueue *queue);

/* ---- 事件 ---- */

GpgpuKmtStatus gpgpuKmtCreateEvent(GpgpuKmtQueue *queue, GpgpuKmtEvent **event);
GpgpuKmtStatus gpgpuKmtQueryEvent(GpgpuKmtEvent *event, struct gpgpu_event_request *state);
/* timeout_ms 为 0 只检查，UINT32_MAX 不设期限。 */
GpgpuKmtStatus gpgpuKmtWaitEvent(GpgpuKmtEvent *event, uint32_t timeout_ms);
GpgpuKmtStatus gpgpuKmtDestroyEvent(GpgpuKmtEvent *event);

/* ---- 提交 ---- */

/* Host 普通内存访问与 Doorbell 写入之间的屏障。 */
void gpgpuKmtHostFence(void);
/*
 * 原子取得一个包编号并等待对应槽空出。多个线程可并发调用。
 * 失败时编号已被占用、不会回滚，该队列不能再使用。
 */
GpgpuKmtStatus gpgpuKmtReserve(GpgpuKmtQueue *queue, uint32_t *number);
/* 写入包体，最后以完整 32 位写入 Header 发布，再写 Doorbell。 */
GpgpuKmtStatus gpgpuKmtPublish(GpgpuKmtQueue *queue, uint32_t number,
			       const GpgpuAqlPacket *packet);
/* 先轮询约 200 μs，再阻塞等待事件；每次唤醒都复查 Signal 是否为 0。 */
GpgpuKmtStatus gpgpuKmtWaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms);
/* 等待全部已预留的包完成（R == W）；队列出错时返回 WAIT_FAILURE。 */
GpgpuKmtStatus gpgpuKmtDrainQueue(GpgpuKmtQueue *queue, uint32_t timeout_ms);

#endif /* GPGPU_KMT_H */
