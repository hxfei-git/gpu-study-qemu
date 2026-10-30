/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：内存中的队列对象
 *
 * Host 在内存中准备、CP 读取或写回的对象布局：MQD、管理命令与结果区、
 * AQL 派发包、Kernel 描述对象和 Signal。寄存器见 gpgpu_regs.h。
 *
 * 队列提交的整体关系：
 *
 *   Host ── 管理命令 ──▶ KIQ(槽0) ──映射──▶ HIQ(槽1) ──映射──▶ 用户 AQL(槽2/3)
 *   Host ── AQL 包 ───▶ 用户 Ring ──CP 取包──▶ RV32 执行器
 *                        │ kernel_object ─▶ GpgpuKernelDescriptor ─▶ 代码
 *                        │ kernarg_address ─▶ 参数块（a0）
 *                        └ completion_signal ─▶ GpgpuSignal ─▶ 通知槽 ─▶ IH
 */
#ifndef GPGPU_QUEUE_ABI_H
#define GPGPU_QUEUE_ABI_H

#include "gpgpu_abi_types.h"

/* 管理 Ring 与 AQL Ring 的最小字节容量。 */
#define GPGPU_QUEUE_RING_MIN 64u
/* 管理 Ring 与 AQL Ring 的最大字节容量，须为 2 的幂。 */
#define GPGPU_QUEUE_RING_MAX 65536u

/* ---- 管理队列 ---- */

/**
 * enum GpgpuQueueKind - 管理命令操作的目标队列种类
 * @GPGPU_QUEUE_HIQ: 由 KIQ 管理，固定使用 HQD 槽 1。
 * @GPGPU_QUEUE_USER_AQL: 由 HIQ 管理，使用 HQD 槽 2 或 3。
 */
enum GpgpuQueueKind {
	GPGPU_QUEUE_HIQ = 1,
	GPGPU_QUEUE_USER_AQL = 2,
};

/**
 * enum GpgpuManagementOpcode - 32 字节管理命令的操作码
 * @GPGPU_MAP_QUEUES: 装载 MQD 并激活队列。
 * @GPGPU_UNMAP_QUEUES: 停止队列，把消费进度与错误保存回 MQD。
 * @GPGPU_WRITE_COMPLETION: 把前一条命令的结果和请求序号写回结果区。
 */
enum GpgpuManagementOpcode {
	GPGPU_MAP_QUEUES = 1,
	GPGPU_UNMAP_QUEUES = 2,
	GPGPU_WRITE_COMPLETION = 3,
};

/**
 * enum GpgpuManagementStatus - 管理结果区 status 的取值，依次为 0 到 6
 * @GPGPU_MANAGEMENT_OK: 操作成功。
 * @GPGPU_MANAGEMENT_NO_SLOT: 允许范围内没有空闲 HQD 槽。
 * @GPGPU_MANAGEMENT_DUPLICATE: 同一 MQD 地址已经绑定。
 * @GPGPU_MANAGEMENT_NOT_MAPPED: 指定 MQD 当前没有绑定。
 * @GPGPU_MANAGEMENT_INVALID: 参数、MQD 内容或保存的错误状态非法。
 * @GPGPU_MANAGEMENT_MQD_FAULT: 访问或保存 MQD 失败，槽可能仍被占用。
 * @GPGPU_MANAGEMENT_RECOVERY_REQUIRED: 当前状态不允许普通操作（如队列未排空）。
 */
enum GpgpuManagementStatus {
	GPGPU_MANAGEMENT_OK,
	GPGPU_MANAGEMENT_NO_SLOT,
	GPGPU_MANAGEMENT_DUPLICATE,
	GPGPU_MANAGEMENT_NOT_MAPPED,
	GPGPU_MANAGEMENT_INVALID,
	GPGPU_MANAGEMENT_MQD_FAULT,
	GPGPU_MANAGEMENT_RECOVERY_REQUIRED,
};

/* 管理结果区 status 的初值，表示尚未完成。 */
#define GPGPU_MANAGEMENT_PENDING 0xffffffffu

/**
 * struct GpgpuMqd - 64 字节 MQD，每条存活队列独占，至少按 8 字节对齐
 * @ring_base: Ring 地址，按 64 字节对齐；用户队列为用户地址，管理队列为 GPU 物理地址。
 * @rptr_report_addr: CP 写回 32 位消费进度 R 的地址，按 8 字节对齐。
 * @wptr_poll_addr: CP 轮询 32 位发布进度 W 的地址，按 8 字节对齐。
 * @pq_control: Ring 字节容量。
 * @doorbell_control: Doorbell 的 ENABLE 位与 BAR4 字节偏移。
 * @vmid: 用户队列所属 VMID，管理队列为 0。
 * @aql_control: 管理 Ring 为 0，用户 AQL Ring 为 1。
 * @saved_rptr: 卸载时 CP 保存的 R（32 位编号零扩展），重装时恢复。
 * @saved_error: 卸载时 CP 保存的首次队列错误；非零时拒绝普通重装。
 * @reserved: 必须为零。
 */
typedef struct GpgpuMqd {
	gpgpu_q_u64 ring_base;
	gpgpu_q_u64 rptr_report_addr;
	gpgpu_q_u64 wptr_poll_addr;
	gpgpu_q_u32 pq_control;
	gpgpu_q_u32 doorbell_control;
	gpgpu_q_u32 vmid;
	gpgpu_q_u32 aql_control;
	gpgpu_q_u64 saved_rptr;
	gpgpu_q_u32 saved_error;
	gpgpu_q_u32 reserved[3];
} GpgpuMqd;

/**
 * struct GpgpuManagementPacket - 32 字节管理命令，在 Ring 内按 32 字节对齐
 * @opcode: GpgpuManagementOpcode。
 * @queue_kind: MAP/UNMAP 的目标种类；WRITE_COMPLETION 填 0。
 * @address: MAP/UNMAP 为 MQD GPU 物理地址；WRITE_COMPLETION 为结果区地址。
 * @value: WRITE_COMPLETION 写入的非零请求序号；其他命令填 0。
 * @reserved: 必须为零。
 *
 * 每条 MAP/UNMAP 之后必须紧跟一条 WRITE_COMPLETION。
 */
typedef struct GpgpuManagementPacket {
	gpgpu_q_u32 opcode;
	gpgpu_q_u32 queue_kind;
	gpgpu_q_u64 address;
	gpgpu_q_u64 value;
	gpgpu_q_u64 reserved;
} GpgpuManagementPacket;

/**
 * struct GpgpuManagementResult - 16 字节结果区，每个在途请求独占
 * @completion: 初值 0；CP 在 status 可见后写入请求序号。
 * @status: 初值 GPGPU_MANAGEMENT_PENDING，完成后为 GpgpuManagementStatus。
 * @reserved: 必须为零。
 */
typedef struct GpgpuManagementResult {
	gpgpu_q_u64 completion;
	gpgpu_q_u32 status;
	gpgpu_q_u32 reserved;
} GpgpuManagementResult;

/* ---- 队列错误 ---- */

/**
 * enum GpgpuQueueError - HQD_ERROR 与 MQD saved_error 中的首次错误，依次为 0 到 10
 * @GPGPU_QUEUE_ERROR_NONE: 无错误。
 * @GPGPU_QUEUE_ERROR_RING: 读取 Ring、W 或相关地址失败。
 * @GPGPU_QUEUE_ERROR_PACKET: 已发布包的类型、格式或尺寸非法。
 * @GPGPU_QUEUE_ERROR_DESCRIPTOR: Kernel 描述对象非法或不可读。
 * @GPGPU_QUEUE_ERROR_EXECUTOR: 执行器拒绝派发或执行失败。
 * @GPGPU_QUEUE_ERROR_SIGNAL: Signal 非法、不可读或完成值写入失败。
 * @GPGPU_QUEUE_ERROR_MAILBOX: 通知槽地址非法或写入失败。
 * @GPGPU_QUEUE_ERROR_RETIRE: Header 回收或 R 写回失败。
 * @GPGPU_QUEUE_ERROR_PAIR: 管理命令没有与 WRITE_COMPLETION 成对出现。
 * @GPGPU_QUEUE_ERROR_COMPLETION: 管理结果区非法或写回失败。
 * @GPGPU_QUEUE_ERROR_SAVE: 卸载时保存 MQD 状态失败。
 *
 * 这些编号同时作为 CP 访存窗口的阶段（GPGPU_VM_FAULT_PHASE）。
 */
enum GpgpuQueueError {
	GPGPU_QUEUE_ERROR_NONE,
	GPGPU_QUEUE_ERROR_RING,
	GPGPU_QUEUE_ERROR_PACKET,
	GPGPU_QUEUE_ERROR_DESCRIPTOR,
	GPGPU_QUEUE_ERROR_EXECUTOR,
	GPGPU_QUEUE_ERROR_SIGNAL,
	GPGPU_QUEUE_ERROR_MAILBOX,
	GPGPU_QUEUE_ERROR_RETIRE,
	GPGPU_QUEUE_ERROR_PAIR,
	GPGPU_QUEUE_ERROR_COMPLETION,
	GPGPU_QUEUE_ERROR_SAVE,
};

/* ---- 用户 AQL ---- */

/* AQL Header 类型：INVALID，表示槽未发布或已被 CP 回收。 */
#define GPGPU_AQL_INVALID 1u
/* AQL Header 类型：Kernel Dispatch。 */
#define GPGPU_AQL_DISPATCH 2u
/*
 * 唯一支持的 Dispatch Header：位 7:0 为类型 DISPATCH，位 8 为 barrier（0），
 * 位 10:9、12:11 为 acquire、release scope，均为 SYSTEM（2）。
 */
#define GPGPU_AQL_HEADER 0x1402u
/* Header 的 barrier 位；本版串行执行，允许设置。 */
#define GPGPU_AQL_BARRIER 0x0100u
/* full_header 中 Setup（维度数 1 到 3）所在的起始位。 */
#define GPGPU_AQL_SETUP_SHIFT 16

/**
 * struct GpgpuAqlPacket - 64 字节 Kernel Dispatch 包；Header 最后以完整 32 位写入发布
 * @full_header: 低 16 位为 Header，高 16 位为维度数 Setup。
 * @workgroup_size_x: 工作组 X 方向线程数。
 * @workgroup_size_y: 工作组 Y 方向线程数。
 * @workgroup_size_z: 工作组 Z 方向线程数。
 * @reserved0: 必须为零。
 * @grid_size_x: X 方向总线程数，须为 workgroup_size_x 的整数倍。
 * @grid_size_y: Y 方向总线程数。
 * @grid_size_z: Z 方向总线程数。
 * @private_segment_size: 每线程私有段字节数，本版只接受 0。
 * @group_segment_size: 每工作组共享段字节数，本版只接受 0。
 * @kernel_object: GpgpuKernelDescriptor 的用户地址，按 8 字节对齐。
 * @kernarg_address: 参数块的用户地址，按 8 字节对齐；Kernel 从 a0 读取。
 * @reserved2: 必须为零。
 * @completion_signal: GpgpuSignal 的用户地址，按 8 字节对齐。
 */
typedef struct GpgpuAqlPacket {
	gpgpu_q_u32 full_header;
	gpgpu_q_u16 workgroup_size_x;
	gpgpu_q_u16 workgroup_size_y;
	gpgpu_q_u16 workgroup_size_z;
	gpgpu_q_u16 reserved0;
	gpgpu_q_u32 grid_size_x;
	gpgpu_q_u32 grid_size_y;
	gpgpu_q_u32 grid_size_z;
	gpgpu_q_u32 private_segment_size;
	gpgpu_q_u32 group_segment_size;
	gpgpu_q_u64 kernel_object;
	gpgpu_q_u64 kernarg_address;
	gpgpu_q_u64 reserved2;
	gpgpu_q_u64 completion_signal;
} GpgpuAqlPacket;

/**
 * struct GpgpuKernelDescriptor - 16 字节 Kernel 描述对象，CP 经用户域读取
 * @code_entry: RV32 代码入口的用户地址，按 4 字节对齐。
 * @code_size: 代码字节数，非零且为 4 的倍数。
 * @kernarg_size: 参数块字节数，4 到 GPGPU_KERNEL_ARGS_MAX，按 4 字节对齐。
 * @kernel_abi_version: 必须为 GPGPU_KERNEL_ABI_VERSION。
 */
typedef struct GpgpuKernelDescriptor {
	gpgpu_q_u32 code_entry;
	gpgpu_q_u32 code_size;
	gpgpu_q_u32 kernarg_size;
	gpgpu_q_u32 kernel_abi_version;
} __attribute__((aligned(8))) GpgpuKernelDescriptor;

/**
 * struct GpgpuSignal - 24 字节完成对象，至少按 8 字节对齐
 * @value: 64 位有符号完成值；Host 提交前置 1，CP 成功收尾后写 0。
 * @event_id: 进程内非零且唯一的存活事件编号。
 * @reserved: 必须为零。
 * @event_mailbox_ptr: 独占的 8 字节通知槽用户地址；CP 完成时写入 event_id。
 */
typedef struct GpgpuSignal {
	gpgpu_q_s64 value;
	gpgpu_q_u32 event_id;
	gpgpu_q_u32 reserved;
	gpgpu_q_u64 event_mailbox_ptr;
} GpgpuSignal;

GPGPU_LAYOUT_ASSERT(GpgpuMqd, 64, saved_rptr, 0x28);
GPGPU_LAYOUT_ASSERT(GpgpuMqd, 64, saved_error, 0x30);
GPGPU_LAYOUT_ASSERT(GpgpuManagementPacket, 32, address, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuManagementPacket, 32, value, 0x10);
GPGPU_LAYOUT_ASSERT(GpgpuManagementResult, 16, status, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuAqlPacket, 64, kernel_object, 0x20);
GPGPU_LAYOUT_ASSERT(GpgpuAqlPacket, 64, completion_signal, 0x38);
GPGPU_LAYOUT_ASSERT(GpgpuKernelDescriptor, 16, kernarg_size, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuKernelDescriptor, 16, kernel_abi_version, 0x0c);
GPGPU_LAYOUT_ASSERT(GpgpuSignal, 24, event_mailbox_ptr, 0x10);

#endif /* GPGPU_QUEUE_ABI_H */
