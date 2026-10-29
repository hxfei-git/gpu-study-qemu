/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_FIRMWARE_CP_H
#define GPGPU_FIRMWARE_CP_H

#include "gpgpu/gpgpu_protocol.h"

typedef gpgpu_q_u32 u32;
typedef gpgpu_q_u64 u64;

/* mcause：load access fault。 */
#define RISCV_CAUSE_LOAD_ACCESS 5
/* mcause：store/AMO access fault。 */
#define RISCV_CAUSE_STORE_ACCESS 7

/**
 * struct CpSlot - 固件私有的 HQD 占用与管理结果记录
 * @mqd: 当前绑定 MQD 的完整 GPU 物理地址；占用解除前不得复用。
 * @domain: 管理对象直接访问物理地址，用户对象按设备模式解释地址。
 * @occupied: 占用标记，ACTIVE 清零不代表该槽可重新分配。
 * @pending: 管理操作已结束，正在等待相邻 WRITE_COMPLETION 回报。
 * @result: 暂存的 GpgpuManagementStatus，由完成命令写入 Host。
 * @vmid: 本对象所属的设备地址空间编号，管理访问为零。
 */
typedef struct CpSlot {
	u64 mqd;
	u32 occupied;
	u32 domain; /* 明确区分管理物理访问与用户访问。 */
	u32 vmid;   /* 普通队列访问的上下文；MQD 本体显式使用管理域。 */
	u32 pending;
	u32 result;
} CpSlot;

/**
 * struct CpAccess - 记录 Host 窗口访问归属及最近异常现场
 * @address: 本次访问的 GPU 地址，由 domain 决定是否翻译。
 * @fault_address: 陷阱返回的故障地址，即 mtval。
 * @cause: 最近一次陷阱的 mcause 值。
 * @slot: 当前访问归属的 HQD 槽号。
 * @phase: 访问所在阶段，使用 GpgpuQueueError 编码。
 * @source: 映射请求来自哪个管理槽，0 为 KIQ、1 为 HIQ。
 * @kind: 最近一次映射请求的 GpgpuQueueKind。
 * @mqd: 最近一次映射请求的目标 MQD 物理地址。
 */
typedef struct CpAccess {
	u64 address;
	u64 fault_address;
	u64 cause;
	u32 slot;
	u32 phase;
	u32 source;
	u32 kind;
	u64 mqd;
} CpAccess;

extern CpSlot cp_slots[GPGPU_HQD_SLOTS];
extern volatile CpAccess cp_access;
extern volatile u32 cp_stopped;
extern u32 cp_gpuvm;

/*
 * cp_fence() - 建立 CP 内存与 MMIO 的访问顺序
 *
 * 使用带编译器 memory 约束的 fence iorw，连接输入、提交与完成发布。
 */
static inline void cp_fence(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

/*
 * cp_reg_read() - 读取一个 CP 本地控制寄存器
 *
 * offset 是寄存器窗口内的字节偏移，按完整 32 位访问。
 */
static inline u32 cp_reg_read(u32 offset)
{
	return *(volatile u32 *)(unsigned long)(GPGPU_CP_REG_BASE + offset);
}

/*
 * cp_reg_write() - 写入一个 CP 本地控制寄存器
 *
 * 调用者在配置发布或结果通知边界显式执行 cp_fence。
 */
static inline void cp_reg_write(u32 offset, u32 value)
{
	*(volatile u32 *)(unsigned long)(GPGPU_CP_REG_BASE + offset) = value;
}

/*
 * cp_window_write() - 写一个访存窗口配置寄存器
 *
 * offset 为 GPGPU_CP_WINDOW_* 字段偏移；配置在下一次窗口访问前生效。
 */
static inline void cp_window_write(u32 offset, u32 value)
{
	*(volatile u32 *)(unsigned long)(GPGPU_CP_WINDOW_REGS + offset) = value;
}

/*
 * cp_hqd_read() - 读取指定槽的 HQD 寄存器
 *
 * 按槽号与字节步长计算 CP 本地直接地址，执行一次完整 32 位读取。
 */
static inline u32 cp_hqd_read(u32 slot, u32 offset)
{
	return *(volatile u32 *)(unsigned long)(GPGPU_CP_HQD_BASE + slot * GPGPU_HQD_STRIDE +
						offset);
}

/*
 * cp_hqd_write() - 写入指定槽的 HQD 寄存器
 *
 * 通过 CP 本地直接窗口访问，激活与地址发布顺序由调用者用屏障保证。
 */
static inline void cp_hqd_write(u32 slot, u32 offset, u32 value)
{
	*(volatile u32 *)(unsigned long)(GPGPU_CP_HQD_BASE + slot * GPGPU_HQD_STRIDE + offset) =
		value;
}

/*
 * cp_hqd_address() - 读取 HQD 中的完整 GPU 地址
 *
 * 合并低、高两个静态配置寄存器，不用于读取动态 64 位共享值。
 */
static inline u64 cp_hqd_address(u32 slot, u32 offset)
{
	return cp_hqd_read(slot, offset) | ((u64)cp_hqd_read(slot, offset + 4) << 32);
}

/*
 * cp_hqd_set_address() - 向 HQD 写入完整 GPU 地址
 *
 * 分别写低、高 32 位配置，调用者须在激活前完成全部字段。
 */
static inline void cp_hqd_set_address(u32 slot, u32 offset, u64 address)
{
	cp_hqd_write(slot, offset, (u32)address);
	cp_hqd_write(slot, offset + 4, address >> 32);
}

/*
 * cp_range() - 检查 GPU 对象地址是否可访问
 *
 * 要求非零长度与指定对齐，完整范围必须可由 RV32 地址表达。
 */
static inline int cp_range(u64 address, u64 size, u32 align)
{
	return !(address & (align - 1)) && size && address < GPGPU_VM_ADDRESS_LIMIT &&
		size <= GPGPU_VM_ADDRESS_LIMIT - address;
}

/* 从当前指定域 GPU 地址读写一个完整标量；成功返回 1。 */
/* 从指定域完整读取一个 32 位标量。 */
int cp_read32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u32 *value);
/* 从指定域完整读取一个 64 位标量。 */
int cp_read64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u64 *value);
/* 向指定域完整写入一个 32 位标量。 */
int cp_write32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u32 value);
/* 向指定域完整写入一个 64 位标量。 */
int cp_write64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u64 value);
/* 读取初始化后固定的 GPU 对象。 */
int cp_read_object(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, void *dest, u32 size);
/* 通过设备同步发布一条 IH 记录。 */
int cp_ih_submit(u32 slot, u32 type, u32 data);
/* 保存指定队列的首次错误并停止取包。 */
void cp_queue_error(u32 slot, u32 error);
/* 报告固件不可恢复异常并停机。 */
void cp_fatal(void) __attribute__((noreturn));
/* 处理 RV64 固件异常。 */
u64 cp_trap(u64 cause, u64 epc, u64 address);
/* 定位槽内下一待处理包。 */
int cp_next_packet(u32 slot, u64 *address, u32 packet_size);
/* 归还一个已经完成处理的 Ring 槽。 */
int cp_retire(u32 slot);
/* 消费一条 KIQ 或 HIQ 管理命令。 */
void cp_management(u32 slot);
/* 处理一个已发布的用户派发包。 */
int cp_aql(u32 slot);
/* 运行 CP 冷启动与队列调度循环。 */
void cp_main(void) __attribute__((noreturn));
#endif
