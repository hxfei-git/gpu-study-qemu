/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_DRM_H
#define GPGPU_DRM_H
#include "gpgpu_uapi.h"
#include "gpgpu_vm.h"

/* GPUVM 使用独立的 DRM 命令空间；所有 version 均为此值。 */
#define GPGPU_DRM_VERSION 1U
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
	struct gpgpu_va_range ranges[3];
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
 * struct gpgpu_vm_map - 整对象映射或按 GPUVA 撤销映射
 * @version: 接口版本。
 * @handle: MAP 的本文件 GEM handle；UNMAP 必须为零。
 * @address: 整页对齐的 GPUVA；UNMAP 仅用它定位已有映射。
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
#define GPGPU_DRM_MAP _IOW('d', 0x42, struct gpgpu_vm_map)
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
#endif
