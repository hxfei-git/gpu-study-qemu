/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_DRM_H
#define GPGPU_DRM_H
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

#define GPGPU_QABI_VERSION 2U
#define GPGPU_COPY_H2D 0U
#define GPGPU_COPY_D2H 1U
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
 * @mailbox_address: 创建时输出通知槽的 GPUVA 或 GPU 物理地址。
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

#include "gpgpu_vm.h"

/* 统一 DRM 命令空间；所有 version 均为此值。 */
#define GPGPU_DRM_VERSION 2U
/**
 * struct gpgpu_va_range - 可分配的 GPUVA 半开区间
 * @start: 起始字节地址，包含此值。
 * @end: 结束字节地址，不包含此值；64 位可表示 4 GiB。
 */
struct gpgpu_va_range {
	gpgpu_u64 start;
	gpgpu_u64 end;
};
/**
 * struct gpgpu_vm_info - GPUVM 能力及当前绑定数量
 * @version: 输入输出接口版本，必须为 GPGPU_DRM_VERSION。
 * @flags: 保留，输入必须为零。
 * @address_bits: GPUVA 的有效位数。
 * @page_size: 页大小，单位为字节。
 * @user_vmids: 全设备用户 VMID 数量。
 * @queues_per_vm: 每个 VM 可同时持有的用户队列数量。
 * @range_count: ranges 中有效的区间数量。
 * @bindings: 全设备已绑定的用户 VMID 数量，包含异常保留项。
 * @ranges: 可分配区间，已经排除设备和驱动保留地址。
 */
struct gpgpu_vm_info {
	gpgpu_u32 version;
	gpgpu_u32 flags;
	gpgpu_u32 address_bits;
	gpgpu_u32 page_size;
	gpgpu_u32 user_vmids;
	gpgpu_u32 queues_per_vm;
	gpgpu_u32 range_count;
	gpgpu_u32 bindings;
	struct gpgpu_va_range ranges[1];
};
/**
 * struct gpgpu_bo_create - 创建固定存储的 GEM 对象
 * @version: 接口版本。
 * @kind: Host coherent 或 VRAM 存储类型。
 * @size: 逻辑大小，单位为字节。
 * @alignment: 存储对齐，单位为字节，必须为 2 的幂。
 * @handle: 输出的本文件 GEM handle，不暴露设备物理地址。
 * @reserved: 保留，输入必须为零。
 * @mapped_size: 输出的整页映射大小，包含已清零的尾部。
 * @mmap_offset: Host 对象的 CPU 映射偏移；VRAM 对象通过 COPY 访问。
 */
struct gpgpu_bo_create {
	gpgpu_u32 version;
	gpgpu_u32 kind;
	gpgpu_u64 size;
	gpgpu_u64 alignment;
	gpgpu_u32 handle;
	gpgpu_u32 reserved;
	gpgpu_u64 mapped_size;
	gpgpu_u64 mmap_offset;
};
/**
 * struct gpgpu_vm_map - 整对象绑定或按 GPU 地址撤销绑定
 * @version: 接口版本。
 * @handle: MAP 的本文件 GEM handle；UNMAP 必须为零。
 * @address: MAP 开启翻译时输入 GPUVA，关闭时输入零；输出最终 GPU 地址。
 * UNMAP 用此地址定位已有绑定。
 * @permissions: MAP 的读、写、取指权限组合；UNMAP 必须为零。
 * @reserved: 保留，输入必须为零。
 */
struct gpgpu_vm_map {
	gpgpu_u32 version;
	gpgpu_u32 handle;
	gpgpu_u64 address;
	gpgpu_u32 permissions;
	gpgpu_u32 reserved;
};
/**
 * struct gpgpu_vm_fault - 所属队列的首次 GPUVM 故障
 * @version: 接口版本。
 * @queue_id: 本文件的队列编号，必须仍持有队列对象。
 * @address: 出错的完整用户字节地址。
 * @vmid: 出错时绑定的 VMID。
 * @reason: 故障原因；零表示尚无 GPUVM 故障。
 * @access: 读、写或取指类型。
 * @phase: CP 或计算执行器中发生错误的阶段。
 */
struct gpgpu_vm_fault {
	gpgpu_u32 version;
	gpgpu_u32 queue_id;
	gpgpu_u64 address;
	gpgpu_u32 vmid;
	gpgpu_u32 reason;
	gpgpu_u32 access;
	gpgpu_u32 phase;
};
/**
 * struct gpgpu_gem_close - 标准 DRM GEM_CLOSE 输入
 * @handle: 需要关闭的本文件 GEM handle。
 * @pad: 必须为零。
 */
struct gpgpu_gem_close {
	gpgpu_u32 handle;
	gpgpu_u32 pad;
};
#define GPGPU_DRM_GEM_CLOSE _IOW('d', 0x09, struct gpgpu_gem_close)
#define GPGPU_DRM_INFO _IOWR('d', 0x40, struct gpgpu_vm_info)
#define GPGPU_DRM_BO_CREATE _IOWR('d', 0x41, struct gpgpu_bo_create)
#define GPGPU_DRM_MAP _IOWR('d', 0x42, struct gpgpu_vm_map)
#define GPGPU_DRM_UNMAP _IOW('d', 0x43, struct gpgpu_vm_map)
#define GPGPU_DRM_COPY _IOW('d', 0x44, struct gpgpu_memory_copy)
#define GPGPU_DRM_QUEUE_CREATE _IOWR('d', 0x45, struct gpgpu_queue_create)
#define GPGPU_DRM_QUEUE_QUERY _IOWR('d', 0x46, struct gpgpu_queue_state)
#define GPGPU_DRM_QUEUE_DESTROY _IOWR('d', 0x47, struct gpgpu_queue_state)
#define GPGPU_DRM_EVENT_CREATE _IOWR('d', 0x48, struct gpgpu_event_request)
#define GPGPU_DRM_EVENT_QUERY _IOWR('d', 0x49, struct gpgpu_event_request)
#define GPGPU_DRM_EVENT_WAIT _IOWR('d', 0x4a, struct gpgpu_event_request)
#define GPGPU_DRM_EVENT_DESTROY _IOWR('d', 0x4b, struct gpgpu_event_request)
#define GPGPU_DRM_FAULT _IOWR('d', 0x4c, struct gpgpu_vm_fault)
_Static_assert(sizeof(struct gpgpu_memory_copy) == 40, "copy ABI");
_Static_assert(sizeof(struct gpgpu_queue_create) == 64, "queue create ABI");
_Static_assert(sizeof(struct gpgpu_queue_state) == 24, "queue state ABI");
_Static_assert(sizeof(struct gpgpu_event_request) == 32, "event ABI");
_Static_assert(__builtin_offsetof(struct gpgpu_queue_create, doorbell_mmap_offset) == 48,
	       "doorbell ABI");
_Static_assert(__builtin_offsetof(struct gpgpu_event_request, mailbox_address) == 24,
	       "mailbox ABI");

#endif
