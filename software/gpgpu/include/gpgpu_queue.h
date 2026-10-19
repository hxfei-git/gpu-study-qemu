/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_QUEUE_H
#define GPGPU_QUEUE_H

/* 同一份小端协议供设备、RV64 裸机固件、内核和用户态使用。 */
typedef __UINT16_TYPE__ gpgpu_q_u16;
typedef __UINT32_TYPE__ gpgpu_q_u32;
typedef __UINT64_TYPE__ gpgpu_q_u64;
typedef __INT64_TYPE__ gpgpu_q_s64;

/* 设备、固件与 Host 共用的队列协议版本。 */
#define GPGPU_QUEUE_ABI_VERSION 1
/* HQD 总槽数：KIQ、HIQ 和两个用户槽。 */
#define GPGPU_QUEUE_SLOTS 4
/* 全设备共享的用户队列名额。 */
#define GPGPU_USER_QUEUE_MAX 2
/* 固件保留区的 VRAM 起始偏移，也是用户区上界。 */
#define GPGPU_FIRMWARE_BASE 0x03c00000u
/* 末尾固件保留区的字节数，包含代码、数据与栈。 */
#define GPGPU_FIRMWARE_SIZE 0x00400000u
/* 固件栈区的 VRAM 起始偏移。 */
#define GPGPU_FIRMWARE_STACK_BASE 0x03ff0000u
/* 固件初始栈顶，向低地址增长。 */
#define GPGPU_FIRMWARE_STACK_TOP 0x04000000u
/* 固件区之前的字节容量；驱动另行保留首页。 */
#define GPGPU_VRAM_USABLE_SIZE GPGPU_FIRMWARE_BASE
/* 管理与 AQL Ring 的最小字节容量。 */
#define GPGPU_QUEUE_RING_MIN 64u
/* 管理与 AQL Ring 的最大字节容量。 */
#define GPGPU_QUEUE_RING_MAX 65536u
/* Doorbell BAR 的总字节容量。 */
#define GPGPU_QUEUE_DOORBELL_SIZE 65536u
/* CP 本地控制寄存器窗口基址，非 Host BAR 地址。 */
#define GPGPU_CP_REG_BASE 0xfffd0000u
/* CP 本地 HQD 直接访问窗口基址。 */
#define GPGPU_CP_HQD_BASE 0xffff0000u
/* CP 直接窗口中相邻 HQD 槽的字节间隔。 */
#define GPGPU_HQD_STRIDE 0x100u
/* Host 间接窗口的槽选择寄存器，取值 0 到 3。 */
#define GPGPU_REG_HQD_SELECT 0x0600
/* Host 访问已选 HQD 槽的 BAR0 窗口偏移。 */
#define GPGPU_REG_HQD_WINDOW 0x0700
/* IH Ring Host 物理基址低 32 位，停用时由 Host 配置。 */
#define GPGPU_REG_IH_BASE_LO 0x0800
/* IH Ring Host 物理基址高 32 位，停用时由 Host 配置。 */
#define GPGPU_REG_IH_BASE_HI 0x0804
/* IH Ring 字节容量，停用时由 Host 配置。 */
#define GPGPU_REG_IH_SIZE 0x0808
/* 设备发布的下一写入记录编号，Host/CP 只读。 */
#define GPGPU_REG_IH_WPTR 0x080c
/* Host 写回的下一消费记录编号。 */
#define GPGPU_REG_IH_RPTR 0x0810
/* 只读的忙、首次故障及最近提交结果。 */
#define GPGPU_REG_IH_STATUS 0x0814
/* Host 控制 IH 使能与停用状态下的复位。 */
#define GPGPU_REG_IH_CTRL 0x0818
/* CP 专用提交入口，写固件私有 VRAM 中的记录偏移。 */
#define GPGPU_REG_IH_SUBMIT 0x081c
/* 派发代码字节数，空闲时配置。 */
#define GPGPU_REG_KERNEL_CODE_SIZE 0x032c
/* 派发参数块字节数，空闲时配置。 */
#define GPGPU_REG_KERNEL_ARGS_SIZE 0x0334
/* 只读的 IDLE/BUSY/DONE/FAILED 执行器状态。 */
#define GPGPU_REG_DISPATCH_STATUS 0x0338
/* 只读的本次派发细分错误码。 */
#define GPGPU_REG_DISPATCH_ERROR 0x033c
/* CP 在终态写 1 确认结果，使执行器返回空闲。 */
#define GPGPU_REG_DISPATCH_ACK 0x0340
/* RV32 线程读取 Host 参数的只读窗口起始地址。 */
#define GPGPU_KERNEL_ARGS_WINDOW 0x90000000u
/* 单次派发允许的 Host 参数字节数上限。 */
#define GPGPU_KERNEL_ARGS_MAX 65536u

/* 槽内 MQD Host 地址低 32 位。 */
#define GPGPU_HQD_MQD_LO 0x000
/* 槽内 MQD Host 地址高 32 位。 */
#define GPGPU_HQD_MQD_HI 0x004
/* 槽激活标志；清零只停止取包，不代表已释放占用。 */
#define GPGPU_HQD_ACTIVE 0x008
/* 槽地址空间编号，本版只能写零。 */
#define GPGPU_HQD_VMID 0x00c
/* Ring Host 物理基址低 32 位。 */
#define GPGPU_HQD_RING_LO 0x020
/* Ring Host 物理基址高 32 位。 */
#define GPGPU_HQD_RING_HI 0x024
/* CP 当前下一消费包的 32 位编号。 */
#define GPGPU_HQD_RPTR 0x028
/* 共享 R 写回地址低 32 位。 */
#define GPGPU_HQD_RPTR_ADDR_LO 0x02c
/* 共享 R 写回地址高 32 位。 */
#define GPGPU_HQD_RPTR_ADDR_HI 0x030
/* 共享 W 轮询地址低 32 位。 */
#define GPGPU_HQD_WPTR_ADDR_LO 0x034
/* 共享 W 轮询地址高 32 位。 */
#define GPGPU_HQD_WPTR_ADDR_HI 0x038
/* Doorbell 使能位与 BAR4 字节偏移。 */
#define GPGPU_HQD_DOORBELL 0x03c
/* Ring 字节容量，范围及 2 的幂约束由设备校验。 */
#define GPGPU_HQD_CONTROL 0x044
/* 首次队列错误；Host 只读，CP 停用槽后可清理。 */
#define GPGPU_HQD_ERROR 0x0cc
/* 包格式选择，0 为管理命令、1 为用户 AQL。 */
#define GPGPU_HQD_AQL 0x0d4
/* HQD Doorbell 控制中的使能位。 */
#define GPGPU_DOORBELL_ENABLE (1u << 30)
/* Doorbell 字节偏移字段掩码，另须满足 8 字节对齐。 */
#define GPGPU_DOORBELL_OFFSET_MASK 0x0ffffffcu
/* IH_CTRL 使能位，启用后禁止改写 Ring 配置。 */
#define GPGPU_IH_ENABLE (1u << 0)
/* IH_CTRL 复位命令位，只在停用且不忙时接受。 */
#define GPGPU_IH_RESET (1u << 1)
/* IH_STATUS 中当前正在写入一条记录的标记。 */
#define GPGPU_IH_BUSY (1u << 0)
/* IH_STATUS 中首次通道故障的锁存标记。 */
#define GPGPU_IH_FAULT (1u << 1)
/* IH_STATUS 首次故障码的起始位，占 8 位。 */
#define GPGPU_IH_ERROR_SHIFT 8
/* IH_STATUS 最近一次提交结果的起始位，占 8 位。 */
#define GPGPU_IH_RESULT_SHIFT 16
/* IRQ 状态与使能中的 IH 通知位。 */
#define GPGPU_QUEUE_IRQ_IH (1u << 3)
/* ERROR_STATUS 中的 IH 通道故障位。 */
#define GPGPU_QUEUE_ERR_IH (1u << 4)
/* ERROR_STATUS 中的 CP 固件异常位。 */
#define GPGPU_QUEUE_ERR_FIRMWARE (1u << 5)
/* AQL 包类型的无效编码，表示槽尚未发布或已回收。 */
#define GPGPU_AQL_INVALID 1u
/* AQL 包类型的 Kernel Dispatch 编码。 */
#define GPGPU_AQL_DISPATCH 2u
/* 支持的 Dispatch Header：系统级 acquire/release，无 barrier。 */
#define GPGPU_AQL_HEADER 0x1402u
/* AQL Header 的 barrier 位，当前串行执行允许设置。 */
#define GPGPU_AQL_BARRIER 0x0100u
/* 管理结果区 status 初值，不能当作完成结果。 */
#define GPGPU_MANAGEMENT_PENDING 0xffffffffu

/**
 * enum GpgpuQueueKind - 管理命令允许操作的目标队列种类
 * @GPGPU_QUEUE_HIQ: 由 KIQ 管理，固定使用 HQD 槽 1。
 * @GPGPU_QUEUE_USER_AQL: 由 HIQ 管理，使用 HQD 槽 2 或 3。
 */
enum GpgpuQueueKind {
	GPGPU_QUEUE_HIQ = 1,
	GPGPU_QUEUE_USER_AQL = 2
};

/**
 * enum GpgpuManagementOpcode - 32 字节管理包的操作码
 * @GPGPU_MAP_QUEUES: 装载 MQD 并激活队列。
 * @GPGPU_UNMAP_QUEUES: 停止队列并将消费进度与错误保存到 MQD。
 * @GPGPU_WRITE_COMPLETION: 将前一管理结果及请求序号写回 Host 结果区。
 */
enum GpgpuManagementOpcode {
	GPGPU_MAP_QUEUES = 1,
	GPGPU_UNMAP_QUEUES = 2,
	GPGPU_WRITE_COMPLETION = 3,
};

/**
 * enum GpgpuManagementStatus - 管理结果区的有效编码，值依次为 0 到 6
 * @GPGPU_MANAGEMENT_OK: 管理操作成功。
 * @GPGPU_MANAGEMENT_NO_SLOT: 允许范围内没有空闲 HQD 名额。
 * @GPGPU_MANAGEMENT_DUPLICATE: 相同完整 MQD 地址已被绑定。
 * @GPGPU_MANAGEMENT_NOT_MAPPED: 指定 MQD 当前没有绑定。
 * @GPGPU_MANAGEMENT_INVALID: 参数、MQD 配置或保存状态非法。
 * @GPGPU_MANAGEMENT_MQD_FAULT: 访问或保存 MQD 及相关状态失败，需核对资源占用。
 *
 * @GPGPU_MANAGEMENT_RECOVERY_REQUIRED: 当前状态不允许普通操作，
 * 需要后续恢复处理。
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

/**
 * enum GpgpuQueueError - HQD 首次错误编号，Host 只读，值依次为 0 到 10
 * @GPGPU_QUEUE_ERROR_NONE: 未记录错误。
 * @GPGPU_QUEUE_ERROR_RING: 读取 Ring、W 或相关地址失败。
 * @GPGPU_QUEUE_ERROR_PACKET: 已发布包的类型、格式或容量关系非法。
 * @GPGPU_QUEUE_ERROR_DESCRIPTOR: Kernel 描述对象或地址范围非法。
 * @GPGPU_QUEUE_ERROR_EXECUTOR: 执行器拒绝派发或执行失败。
 * @GPGPU_QUEUE_ERROR_SIGNAL: Signal 验证、读取或完成值写入失败。
 * @GPGPU_QUEUE_ERROR_MAILBOX: 通知槽地址非法或写入失败。
 * @GPGPU_QUEUE_ERROR_RETIRE: Header 回收或 R 写回失败。
 * @GPGPU_QUEUE_ERROR_PAIR: 管理操作与相邻完成命令不配对。
 * @GPGPU_QUEUE_ERROR_COMPLETION: 管理结果区配置或结果写回失败。
 * @GPGPU_QUEUE_ERROR_SAVE: 卸载时保存 MQD 状态失败。
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

/**
 * enum GpgpuIhType - IH 记录类型，决定 data 和队列身份字段的解释
 * @GPGPU_IH_SIGNAL_NOTIFY: data 为完成事件编号。
 * @GPGPU_IH_QUEUE_ERROR: data 为队列首次错误编号。
 * @GPGPU_IH_DEVICE_ERROR: data 为设备错误位，slot 全 1 且 MQD 为零。
 */
enum GpgpuIhType {
	GPGPU_IH_SIGNAL_NOTIFY = 1,
	GPGPU_IH_QUEUE_ERROR = 2,
	GPGPU_IH_DEVICE_ERROR = 3,
};

/**
 * enum GpgpuIhError - IH 首次故障及最近提交结果的编码
 * @GPGPU_IH_OK: 提交成功或尚无错误。
 * @GPGPU_IH_ERR_DISABLED: 通道尚未使能。
 * @GPGPU_IH_ERR_CONFIG: Ring 基址或容量配置非法。
 * @GPGPU_IH_ERR_FULL: Host 尚未归还可用记录槽。
 * @GPGPU_IH_ERR_DMA: 向 Host Ring 写记录失败。
 * @GPGPU_IH_ERR_PROTOCOL: 寄存器访问权限、宽度或状态转换非法。
 * @GPGPU_IH_ERR_RECORD: 记录地址不在私有 VRAM，或记录内容非法。
 * @GPGPU_IH_ERR_FAULTED: 通道已故障，拒绝新的提交。
 */
enum GpgpuIhError {
	GPGPU_IH_OK,
	GPGPU_IH_ERR_DISABLED,
	GPGPU_IH_ERR_CONFIG,
	GPGPU_IH_ERR_FULL,
	GPGPU_IH_ERR_DMA,
	GPGPU_IH_ERR_PROTOCOL,
	GPGPU_IH_ERR_RECORD,
	GPGPU_IH_ERR_FAULTED,
};

/**
 * enum GpgpuDispatchStatus - 执行器状态，终态须由 CP 确认后回到空闲
 * @GPGPU_DISPATCH_IDLE: 允许配置和提交新任务。
 * @GPGPU_DISPATCH_BUSY: 任务正在分段执行，拒绝覆盖配置。
 * @GPGPU_DISPATCH_DONE: 任务及相关访问已成功结束，等待 ACK。
 * @GPGPU_DISPATCH_FAILED: 任务已停止并保存错误，等待 ACK。
 */
enum GpgpuDispatchStatus {
	GPGPU_DISPATCH_IDLE,
	GPGPU_DISPATCH_BUSY,
	GPGPU_DISPATCH_DONE,
	GPGPU_DISPATCH_FAILED,
};

/**
 * enum GpgpuDispatchError - DISPATCH_ERROR 的细分执行错误编码
 * @GPGPU_EXEC_ERR_NONE: 未记录执行错误。
 * @GPGPU_EXEC_ERR_INVALID_CONFIG: 提交配置不满足当前执行条件。
 * @GPGPU_EXEC_ERR_CODE_RANGE: 取指超出本次代码范围。
 * @GPGPU_EXEC_ERR_ARG_RANGE: 参数地址、对齐或访问范围非法。
 * @GPGPU_EXEC_ERR_ARG_READ: 读取 Host 参数失败。
 * @GPGPU_EXEC_ERR_ARG_WRITE: 尝试写入只读参数窗口。
 * @GPGPU_EXEC_ERR_INSTRUCTION: 遇到非法或未支持指令。
 * @GPGPU_EXEC_ERR_VRAM: 计算线程的 VRAM 访问非法。
 * @GPGPU_EXEC_ERR_BUDGET: warp 或整次派发耗尽执行预算。
 */
enum GpgpuDispatchError {
	GPGPU_EXEC_ERR_NONE,
	GPGPU_EXEC_ERR_INVALID_CONFIG,
	GPGPU_EXEC_ERR_CODE_RANGE,
	GPGPU_EXEC_ERR_ARG_RANGE,
	GPGPU_EXEC_ERR_ARG_READ,
	GPGPU_EXEC_ERR_ARG_WRITE,
	GPGPU_EXEC_ERR_INSTRUCTION,
	GPGPU_EXEC_ERR_VRAM,
	GPGPU_EXEC_ERR_BUDGET,
};

/**
 * struct GpgpuMqd - 64 字节小端 MQD，存活队列各自独占并至少按 8 字节对齐
 * @ring_base: Ring 的完整 Host 物理基址，按 64 字节对齐。
 * @rptr_report_addr: 32 位消费进度 R 的 Host 地址，按 8 字节对齐。
 * @wptr_poll_addr: 32 位发布或预留进度 W 的 Host 地址，按 8 字节对齐。
 * @pq_control: Ring 字节容量，本版直接保存容量而非对数编码。
 * @doorbell_control: Doorbell 字节偏移与 ENABLE 位的组合。
 * @vmid: 队列地址空间编号，本版固定为零。
 * @aql_control: 管理 Ring 为零，用户 AQL Ring 为 1。
 * @saved_rptr: 卸载时保存的 R，将 32 位编号零扩展为 64 位。
 * @saved_error: 卸载时保存的首次队列错误，非零时禁止普通重装。
 * @reserved: 保留字段，初始化为零。
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
 * struct GpgpuManagementPacket - 32 字节小端管理命令，在 Ring 内按 32 字节对齐
 * @opcode: MAP_QUEUES、UNMAP_QUEUES 或 WRITE_COMPLETION。
 * @queue_kind: 映射或卸载的队列种类；完成命令填零。
 * @address: 映射或卸载使用 MQD 地址；完成命令使用结果区地址。
 * @value: 完成命令写入的非零序号；其余命令填零。
 * @reserved: 保留字段，必须为零。
 */
typedef struct GpgpuManagementPacket {
	gpgpu_q_u32 opcode;
	gpgpu_q_u32 queue_kind;
	gpgpu_q_u64 address;
	gpgpu_q_u64 value;
	gpgpu_q_u64 reserved;
} GpgpuManagementPacket;

/**
 * struct GpgpuManagementResult - 16 字节管理结果区，每个在途请求独占
 * @completion: 初值为零；CP 在 status 可见后写入本次预期序号。
 * @status: 初值为 GPGPU_MANAGEMENT_PENDING，完成后应为 0 到 6。
 * @reserved: 保留字段，初始化为零。
 */
typedef struct GpgpuManagementResult {
	gpgpu_q_u64 completion;
	gpgpu_q_u32 status;
	gpgpu_q_u32 reserved;
} GpgpuManagementResult;

/**
 * struct GpgpuAqlPacket - 64 字节小端 AQL 派发包，Header 最后以完整 32 位发布
 * @full_header: 低 16 位为 Header，高 16 位为维度 Setup。
 * @workgroup_size_x: 一个工作组在 X 维的线程数。
 * @workgroup_size_y: 一个工作组在 Y 维的线程数。
 * @workgroup_size_z: 一个工作组在 Z 维的线程数。
 * @reserved0: 保留字段，必须为零。
 * @grid_size_x: X 维总线程数，须为 workgroup_size_x 的整数倍。
 * @grid_size_y: Y 维总线程数，须为 workgroup_size_y 的整数倍。
 * @grid_size_z: Z 维总线程数，须为 workgroup_size_z 的整数倍。
 * @private_segment_size: 每线程私有段字节数，本版只接受零。
 * @group_segment_size: 每工作组共享段字节数，本版只接受零。
 * @kernel_object: 16 字节 Kernel 描述对象的 VRAM 偏移，按 8 字节对齐。
 * @kernarg_address: 参数块的完整 Host 物理地址，按 8 字节对齐。
 * @reserved2: 保留字段，必须为零。
 * @completion_signal: 24 字节 Signal 的完整 Host 物理地址，按 8 字节对齐。
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
 * struct GpgpuKernelDescriptor - 16 字节小端 Kernel 描述对象，存放在用户 VRAM
 * @code_entry: RV32 代码入口的 VRAM 字节偏移，按 4 字节对齐。
 * @code_size: 代码字节数，非零且为 4 的倍数。
 * @kernarg_size: 参数块字节数，范围为 4 到 64 KiB，按 4 字节对齐。
 * @reserved: 保留字段，必须为零。
 */
typedef struct GpgpuKernelDescriptor {
	gpgpu_q_u32 code_entry;
	gpgpu_q_u32 code_size;
	gpgpu_q_u32 kernarg_size;
	gpgpu_q_u32 reserved;
} __attribute__((aligned(8))) GpgpuKernelDescriptor;

/**
 * struct GpgpuSignal - 24 字节小端完成对象，必须至少按 8 字节对齐
 * @value: 完整 64 位有符号完成值，本版提交为 1、成功减为零。
 * @event_id: 所属进程内非零且唯一的存活事件编号。
 * @reserved: 保留字段，必须为零。
 * @event_mailbox_ptr: 独占的 8 字节通知槽 Host 物理地址，按 8 字节对齐。
 */
typedef struct GpgpuSignal {
	gpgpu_q_s64 value;
	gpgpu_q_u32 event_id;
	gpgpu_q_u32 reserved;
	gpgpu_q_u64 event_mailbox_ptr;
} GpgpuSignal;

/**
 * struct GpgpuIhRecord - 32 字节小端通知记录，记录写完后设备才发布 IH W
 * @type: Signal 通知、队列错误或设备错误的 GpgpuIhType 编码。
 * @hqd_slot: 来源 HQD 槽号；设备错误使用全 1。
 * @mqd_addr: 来源队列的完整 MQD 地址；设备错误为零。
 * @data: Signal 通知的事件编号，或队列/设备错误值。
 * @reserved: 保留字段，必须为零。
 */
typedef struct GpgpuIhRecord {
	gpgpu_q_u32 type;
	gpgpu_q_u32 hqd_slot;
	gpgpu_q_u64 mqd_addr;
	gpgpu_q_u32 data;
	gpgpu_q_u32 reserved[3];
} GpgpuIhRecord;

/* 编译期核对共享对象大小、8 字节对齐及关键字段偏移。 */
#define GPGPU_LAYOUT_ASSERT(type, size, member, offset)             \
	_Static_assert(sizeof(type) == (size), #type " size");      \
	_Static_assert(__alignof__(type) >= 8, #type " alignment"); \
	_Static_assert(__builtin_offsetof(type, member) == (offset), #type "." #member " offset")
GPGPU_LAYOUT_ASSERT(GpgpuMqd, 64, saved_rptr, 0x28);
GPGPU_LAYOUT_ASSERT(GpgpuMqd, 64, saved_error, 0x30);
GPGPU_LAYOUT_ASSERT(GpgpuManagementPacket, 32, address, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuManagementPacket, 32, value, 0x10);
GPGPU_LAYOUT_ASSERT(GpgpuManagementResult, 16, status, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuAqlPacket, 64, kernel_object, 0x20);
GPGPU_LAYOUT_ASSERT(GpgpuAqlPacket, 64, completion_signal, 0x38);
GPGPU_LAYOUT_ASSERT(GpgpuKernelDescriptor, 16, kernarg_size, 0x08);
GPGPU_LAYOUT_ASSERT(GpgpuSignal, 24, event_mailbox_ptr, 0x10);
GPGPU_LAYOUT_ASSERT(GpgpuIhRecord, 32, data, 0x10);
#undef GPGPU_LAYOUT_ASSERT
#endif
