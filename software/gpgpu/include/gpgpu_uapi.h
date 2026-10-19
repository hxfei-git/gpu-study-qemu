/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_UAPI_H
#define GPGPU_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __u32 gpgpu_u32;
typedef __u64 gpgpu_u64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t gpgpu_u32;
typedef uint64_t gpgpu_u64;
#endif

/* 同步 ioctl 接口的版本号。 */
#define GPGPU_ABI_VERSION 1
/* 同步会话一致性 DMA 暂存区的字节容量。 */
#define GPGPU_STAGING_SIZE (64U * 1024)
/* 一次派发允许的工作组总数上限。 */
#define GPGPU_MAX_BLOCKS 4096U
/* 一个工作组允许的线程数上限。 */
#define GPGPU_MAX_BLOCK_THREADS 1024U
/* 同步软件接口一次派发允许的总线程数上限。 */
#define GPGPU_MAX_THREADS 65536U

/**
 * struct gpgpu_info - 同步接口的只读设备信息
 * @abi_version: 同步 ioctl ABI 版本。
 * @vram_size: 设备 VRAM 总字节数。
 * @staging_size: 每个同步会话的 DMA 暂存区字节数。
 * @caps: DEV_CAPS 寄存器的计算单元与 warp 能力编码。
 */
struct gpgpu_info {
	gpgpu_u32 abi_version;
	gpgpu_u32 vram_size;
	gpgpu_u32 staging_size;
	gpgpu_u32 caps;
};

/**
 * struct gpgpu_alloc - 同步模式 VRAM 分配请求及返回地址
 * @size: 请求分配的字节数。
 * @address: 成功返回的 VRAM 字节偏移，不是 CPU 指针。
 */
struct gpgpu_alloc {
	gpgpu_u32 size;
	gpgpu_u32 address;
};

/* DMA 方向：Host RAM 复制到 VRAM。 */
#define GPGPU_COPY_H2D 0U
/* DMA 方向：VRAM 复制到 Host RAM。 */
#define GPGPU_COPY_D2H 1U

/**
 * struct gpgpu_copy - 同步模式下暂存区与 VRAM 之间的一次 DMA 拷贝
 * @address: VRAM 分配内的字节偏移，Host 端固定使用暂存区起点。
 * @size: 本次拷贝的字节数，不超过暂存区容量。
 * @direction: GPGPU_COPY_H2D 或 GPGPU_COPY_D2H。
 * @reserved: 保留字段，必须为零。
 */
struct gpgpu_copy {
	gpgpu_u32 address;
	gpgpu_u32 size;
	gpgpu_u32 direction;
	gpgpu_u32 reserved;
};

/**
 * struct gpgpu_launch - 同步派发请求，所有设备地址均为 VRAM 字节偏移
 * @code: RV32 代码入口偏移，按 4 字节对齐。
 * @code_size: 代码范围的字节数。
 * @args: 参数块偏移，启动时写入 RV32 的 a0。
 * @args_size: 参数块字节数。
 * @grid: X/Y/Z 方向的工作组数。
 * @block: 每工作组 X/Y/Z 方向的线程数。
 * @shared_size: 每工作组共享内存字节数，本版只接受零。
 * @reserved: 保留字段，必须为零。
 */
struct gpgpu_launch {
	gpgpu_u32 code;
	gpgpu_u32 code_size;
	gpgpu_u32 args;
	gpgpu_u32 args_size;
	gpgpu_u32 grid[3];
	gpgpu_u32 block[3];
	gpgpu_u32 shared_size;
	gpgpu_u32 reserved;
};

/**
 * struct gpgpu_stats - 驱动累计记录的中断次数
 * @kernel_irqs: Kernel 完成事件次数。
 * @dma_irqs: DMA 完成事件次数。
 * @error_irqs: 设备错误事件次数。
 */
struct gpgpu_stats {
	gpgpu_u64 kernel_irqs;
	gpgpu_u64 dma_irqs;
	gpgpu_u64 error_irqs;
};

/* 同步模式查询设备信息。 */
#define GPGPU_IOC_INFO _IOR('G', 0, struct gpgpu_info)
/* 同步模式申请 VRAM 并返回偏移。 */
#define GPGPU_IOC_ALLOC _IOWR('G', 1, struct gpgpu_alloc)
/* 同步模式按分配起始偏移释放 VRAM。 */
#define GPGPU_IOC_FREE _IOW('G', 2, gpgpu_u32)
/* 同步模式在暂存区与 VRAM 间搬运。 */
#define GPGPU_IOC_COPY _IOW('G', 3, struct gpgpu_copy)
/* 同步模式提交计算并等待完成。 */
#define GPGPU_IOC_LAUNCH _IOW('G', 4, struct gpgpu_launch)
/* 同步模式显式复位设备。 */
#define GPGPU_IOC_RESET _IO('G', 5)
/* 查询同步模式中断计数。 */
#define GPGPU_IOC_STATS _IOR('G', 6, struct gpgpu_stats)

/*
 * 队列接口独立版本；所有保留字段必须为零。
 * 句柄只在本文件上下文有效。
 */
/* 队列 ioctl 接口版本，与同步 ABI 独立校验。 */
#define GPGPU_QABI_VERSION 1U
/* 内存种类：可映射的一致性 Host RAM。 */
#define GPGPU_MEMORY_HOST 1U
/* 内存种类：gpu_buddy 管理的设备 VRAM。 */
#define GPGPU_MEMORY_VRAM 2U
/* 队列可提交，尚未记录故障。 */
#define GPGPU_QUEUE_READY 1U
/* 队列正进行管理操作，禁止重复卸载。 */
#define GPGPU_QUEUE_UNLOADING 2U
/* 队列已故障，资源可能仍需保留。 */
#define GPGPU_QUEUE_FAULTED 3U
/* 事件尚未收到完成或失败通知。 */
#define GPGPU_EVENT_PENDING 0U
/* 已保存完成通知；等待者观察后不清除此状态。 */
#define GPGPU_EVENT_NOTIFIED 1U
/* 源队列或设备发生错误，等待应返回失败。 */
#define GPGPU_EVENT_FAILED 2U

/**
 * struct gpgpu_queue_info - 队列接口的能力查询，输入版本与零 flags
 * @version: 输入/输出队列 ABI 版本 GPGPU_QABI_VERSION。
 * @flags: 查询标志，本版必须为零。
 * @vram_size: 设备 VRAM 总字节数。
 * @vram_usable: 固件区之前的字节数，分配器仍会保留首个页。
 * @firmware_base: 固件保留区的 VRAM 起始偏移。
 * @firmware_size: 固件保留区字节数。
 * @reserved_page_size: VRAM 首个保留页的字节数。
 * @max_queues: 全设备同时存活的用户队列名额。
 * @packet_size: 每个 AQL 包的字节数。
 * @ring_min: Ring 字节容量下限。
 * @ring_max: Ring 字节容量上限。
 * @host_address_bits: Host 物理地址位宽，本版为 64。
 * @progress_bits: 共享 R/W 编号位宽，本版为 32。
 * @capabilities: 队列实现能力标记，本版返回 1。
 */
struct gpgpu_queue_info {
	gpgpu_u32 version;
	gpgpu_u32 flags;
	gpgpu_u32 vram_size;
	gpgpu_u32 vram_usable;
	gpgpu_u32 firmware_base;
	gpgpu_u32 firmware_size;
	gpgpu_u32 reserved_page_size;
	gpgpu_u32 max_queues;
	gpgpu_u32 packet_size;
	gpgpu_u32 ring_min;
	gpgpu_u32 ring_max;
	gpgpu_u32 host_address_bits;
	gpgpu_u32 progress_bits;
	gpgpu_u32 capabilities;
};

/**
 * struct gpgpu_memory_request - 队列模式内存申请，返回独立句柄与对应地址
 * @version: 输入队列 ABI 版本。
 * @kind: 输入 GPGPU_MEMORY_HOST 或 GPGPU_MEMORY_VRAM。
 * @size: 输入请求字节数，输出实际分配字节数。
 * @alignment: 输入字节对齐要求，必须为非零的 2 的幂。
 * @handle: 输出会话内的内存句柄。
 * @vram_offset: 输出 VRAM 字节偏移；Host 内存为零。
 * @mmap_offset: 输出 Host 内存 mmap 字节偏移令牌；VRAM 为零。
 * @host_address: 输出 Host 物理地址；VRAM 内存为零。
 */
struct gpgpu_memory_request {
	gpgpu_u32 version;
	gpgpu_u32 kind;
	gpgpu_u64 size;
	gpgpu_u64 alignment;
	gpgpu_u32 handle;
	gpgpu_u32 vram_offset;
	gpgpu_u64 mmap_offset;
	gpgpu_u64 host_address;
};

/**
 * struct gpgpu_object_request - 按会话内句柄释放内存对象
 * @version: 输入队列 ABI 版本。
 * @handle: 输入需要释放的内存句柄。
 */
struct gpgpu_object_request {
	gpgpu_u32 version;
	gpgpu_u32 handle;
};

/**
 * struct gpgpu_memory_copy - 按内存对象与对象内偏移提交同步 DMA
 * @version: 输入队列 ABI 版本。
 * @direction: H2D 或 D2H 拷贝方向。
 * @host_handle: Host 内存对象句柄。
 * @vram_handle: VRAM 内存对象句柄。
 * @host_offset: Host 对象内的字节偏移。
 * @vram_offset: VRAM 对象内的字节偏移。
 * @size: 本次拷贝的字节数。
 */
struct gpgpu_memory_copy {
	gpgpu_u32 version;
	gpgpu_u32 direction;
	gpgpu_u32 host_handle;
	gpgpu_u32 vram_handle;
	gpgpu_u64 host_offset;
	gpgpu_u64 vram_offset;
	gpgpu_u64 size;
};

/**
 * struct gpgpu_queue_create - 使用已有 Ring 和进度对象创建用户队列
 * @version: 输入队列 ABI 版本。
 * @ring_handle: 输入保存 AQL Ring 的 Host 内存句柄。
 * @progress_handle: 输入保存 R/W 的 Host 内存句柄。
 * @ring_size: 输入 Ring 字节容量，必须为 2 的幂。
 * @ring_offset: 输入 Ring 在对象内的字节偏移，按 64 字节对齐。
 * @rptr_offset: 输入 R 在进度对象内的字节偏移，按 8 字节对齐。
 * @wptr_offset: 输入 W 在进度对象内的字节偏移，与 R 不重叠。
 * @queue_id: 成功返回的会话内队列编号。
 * @doorbell_offset: 输出 Doorbell 在映射页内的字节偏移。
 * @doorbell_mmap_offset: 输出传给 mmap 的字节偏移令牌。
 * @doorbell_map_size: 输出 Doorbell 映射的字节长度。
 * @reserved: 保留字段，必须为零。
 */
struct gpgpu_queue_create {
	gpgpu_u32 version;
	gpgpu_u32 ring_handle;
	gpgpu_u32 progress_handle;
	gpgpu_u32 ring_size;
	gpgpu_u64 ring_offset;
	gpgpu_u64 rptr_offset;
	gpgpu_u64 wptr_offset;
	gpgpu_u32 queue_id;
	gpgpu_u32 doorbell_offset;
	gpgpu_u64 doorbell_mmap_offset;
	gpgpu_u32 doorbell_map_size;
	gpgpu_u32 reserved;
};

/**
 * struct gpgpu_queue_state - 队列查询或卸载请求及诊断结果
 * @version: 输入队列 ABI 版本。
 * @queue_id: 输入会话内队列编号。
 * @state: 输出 READY、UNLOADING 或 FAULTED 状态。
 * @error: 输出队列首次错误的 GpgpuQueueError 编码。
 * @management_status: 输出最近一次管理请求结果，未完成时为 PENDING。
 * @reserved: 保留字段，必须为零。
 */
struct gpgpu_queue_state {
	gpgpu_u32 version;
	gpgpu_u32 queue_id;
	gpgpu_u32 state;
	gpgpu_u32 error;
	gpgpu_u32 management_status;
	gpgpu_u32 reserved;
};

/**
 * struct gpgpu_event_request - 事件创建、查询、等待及销毁共用的请求
 * @version: 输入队列 ABI 版本。
 * @event_id: 创建时输出事件编号，其余操作输入已有编号。
 * @queue_id: 创建时输入源队列编号；查询和等待时回报源队列。
 * @timeout_ms: 等待毫秒数；零只检查，UINT32_MAX 不设调用方期限。
 * @state: 输出 PENDING、NOTIFIED 或 FAILED 状态。
 * @error: 输出事件失败时保存的错误值。
 * @mailbox_address: 创建时输出独占通知槽的 Host 物理地址。
 */
struct gpgpu_event_request {
	gpgpu_u32 version;
	gpgpu_u32 event_id;
	gpgpu_u32 queue_id;
	gpgpu_u32 timeout_ms;
	gpgpu_u32 state;
	gpgpu_u32 error;
	gpgpu_u64 mailbox_address;
};

/* 查询队列模式能力并核对版本。 */
#define GPGPU_IOC_QINFO _IOWR('G', 0x20, struct gpgpu_queue_info)
/* 申请 Host 或 VRAM 对象并返回句柄。 */
#define GPGPU_IOC_MEMORY_ALLOC _IOWR('G', 0x21, struct gpgpu_memory_request)
/* 释放无存活引用的内存对象。 */
#define GPGPU_IOC_MEMORY_FREE _IOW('G', 0x22, struct gpgpu_object_request)
/* 按对象句柄与偏移执行同步 DMA。 */
#define GPGPU_IOC_MEMORY_COPY _IOW('G', 0x23, struct gpgpu_memory_copy)
/* 建立 MQD 并通过 HIQ 映射用户队列。 */
#define GPGPU_IOC_QUEUE_CREATE _IOWR('G', 0x24, struct gpgpu_queue_create)
/* 查询队列状态和管理结果。 */
#define GPGPU_IOC_QUEUE_QUERY _IOWR('G', 0x25, struct gpgpu_queue_state)
/* 请求 CP 卸载队列并回报管理结果。 */
#define GPGPU_IOC_QUEUE_DESTROY _IOWR('G', 0x26, struct gpgpu_queue_state)
/* 为源队列创建事件及独占通知槽。 */
#define GPGPU_IOC_EVENT_CREATE _IOWR('G', 0x27, struct gpgpu_event_request)
/* 读取已保存的事件状态。 */
#define GPGPU_IOC_EVENT_QUERY _IOWR('G', 0x28, struct gpgpu_event_request)
/* 按毫秒期限等待状态改变，不消费通知。 */
#define GPGPU_IOC_EVENT_WAIT _IOWR('G', 0x29, struct gpgpu_event_request)
/* 在等待与设备访问结束后销毁事件。 */
#define GPGPU_IOC_EVENT_DESTROY _IOWR('G', 0x2a, struct gpgpu_event_request)

_Static_assert(sizeof(struct gpgpu_queue_info) == 56, "queue info ABI");
_Static_assert(sizeof(struct gpgpu_memory_request) == 48, "memory ABI");
_Static_assert(sizeof(struct gpgpu_object_request) == 8, "object ABI");
_Static_assert(sizeof(struct gpgpu_memory_copy) == 40, "copy ABI");
_Static_assert(sizeof(struct gpgpu_queue_create) == 64, "queue create ABI");
_Static_assert(sizeof(struct gpgpu_queue_state) == 24, "queue state ABI");
_Static_assert(sizeof(struct gpgpu_event_request) == 32, "event ABI");
_Static_assert(__builtin_offsetof(struct gpgpu_queue_create, doorbell_mmap_offset) == 48,
	       "doorbell ABI");
_Static_assert(__builtin_offsetof(struct gpgpu_event_request, mailbox_address) == 24,
	       "mailbox ABI");

#endif /* GPGPU_UAPI_H */
