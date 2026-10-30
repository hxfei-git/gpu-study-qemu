/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RV64 CP 固件：模块间共享的状态与接口
 *
 * 固件运行在设备 VRAM 末尾的 4 MiB 保留区，由 Host 驱动装载后释放 CP 复位。
 * 主循环依次检查三类队列：
 *
 *   KIQ（槽 0）  管理命令：映射 / 卸载 HIQ
 *   HIQ（槽 1）  管理命令：映射 / 卸载用户 AQL 队列
 *   用户槽 2、3  AQL Kernel Dispatch，两个槽轮流优先
 *
 * 文件划分：
 *   main.c  启动检查与调度循环
 *   ring.c  按 R/W 取包与回收
 *   mgmt.c  MAP_QUEUES / UNMAP_QUEUES / WRITE_COMPLETION
 *   aql.c   派发包检查、提交执行器、完成发布
 *   ih.c    IH 通知、队列错误与固件致命错误
 *   mem.c   经 CP 访存窗口读写 GPU 地址，以及访存异常的恢复
 */
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
 * struct CpSlot - 固件私有的槽占用记录
 * @mqd: 绑定的 MQD GPU 物理地址；占用解除前同一地址不能再次映射。
 * @occupied: 槽已被占用。ACTIVE 清零（如出错停止）不等于释放，只有正常卸载才清除。
 * @domain: 本槽队列对象的访问域：管理队列为 GPGPU_CP_DOMAIN_MANAGEMENT，
 *          用户队列为 GPGPU_CP_DOMAIN_USER。MQD 本身总按管理域访问。
 * @vmid: 用户队列的 VMID；管理队列为 0。
 * @pending: 管理槽专用：上一条命令已执行，等待相邻的 WRITE_COMPLETION。
 * @result: 管理槽专用：暂存的 GpgpuManagementStatus。
 */
typedef struct CpSlot {
	u64 mqd;
	u32 occupied;
	u32 domain;
	u32 vmid;
	u32 pending;
	u32 result;
} CpSlot;

/* 各槽的占用记录。 */
extern CpSlot cp_slots[GPGPU_HQD_SLOTS];
/* 全局停止标记：KIQ 故障、IH 故障或固件致命错误后置位，主循环只等待。 */
extern volatile u32 cp_stopped;
/* 设备以 gpuvm=on 启动；启动时从 VM_MODE 读取。 */
extern u32 cp_gpuvm;

/* ---- 寄存器访问：CP 本地寄存器、访存窗口配置与 HQD 窗口 ---- */

/* 建立 CP 内存访问与 MMIO 之间的顺序。 */
static inline void cp_fence(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

/* 读 CP 本地寄存器窗口；offset 与 BAR0 偏移相同。 */
static inline u32 cp_reg_read(u32 offset)
{
	return *(volatile u32 *)(unsigned long)(GPGPU_CP_REG_BASE + offset);
}

/* 写 CP 本地寄存器窗口；发布顺序由调用者用 cp_fence() 保证。 */
static inline void cp_reg_write(u32 offset, u32 value)
{
	*(volatile u32 *)(unsigned long)(GPGPU_CP_REG_BASE + offset) = value;
}

/* 写访存窗口配置寄存器；offset 为 GPGPU_CP_WINDOW_* 字段。 */
static inline void cp_window_write(u32 offset, u32 value)
{
	*(volatile u32 *)(unsigned long)(GPGPU_CP_WINDOW_REGS + offset) = value;
}

/* HQD 槽 slot 中字段 field（GPGPU_HQD_* 偏移）在 CP 本地地址空间的位置。 */
static inline volatile u32 *cp_hqd_reg(u32 slot, u32 field)
{
	return (volatile u32 *)(unsigned long)(GPGPU_CP_HQD_BASE + slot * GPGPU_HQD_STRIDE + field);
}

static inline u32 cp_hqd_read(u32 slot, u32 field)
{
	return *cp_hqd_reg(slot, field);
}

static inline void cp_hqd_write(u32 slot, u32 field, u32 value)
{
	*cp_hqd_reg(slot, field) = value;
}

/* 读 HQD 中 LO/HI 字段对组成的静态地址；不用于读取动态共享值。 */
static inline u64 cp_hqd_read64(u32 slot, u32 lo)
{
	return cp_hqd_read(slot, lo) | (u64)cp_hqd_read(slot, lo + 4) << 32;
}

/* 先低后高写入 LO/HI 字段对；只在激活前配置。 */
static inline void cp_hqd_write64(u32 slot, u32 lo, u64 value)
{
	cp_hqd_write(slot, lo, (u32)value);
	cp_hqd_write(slot, lo + 4, value >> 32);
}

/* 地址按 align 对齐、长度非零，且 [addr, addr + size) 在 32 位 GPU 地址空间内。 */
static inline int cp_range_ok(u64 addr, u64 size, u32 align)
{
	return !(addr & (align - 1)) && size && addr < GPGPU_VM_ADDRESS_LIMIT &&
		size <= GPGPU_VM_ADDRESS_LIMIT - addr;
}

/* Ring 容量合法：2 的幂，GPGPU_QUEUE_RING_MIN 到 GPGPU_QUEUE_RING_MAX。 */
static inline int cp_ring_size_ok(u32 size)
{
	return size >= GPGPU_QUEUE_RING_MIN && size <= GPGPU_QUEUE_RING_MAX &&
		!(size & (size - 1));
}

/* ---- start.S：恢复型访存 ---- */

/* 成功返回 1；访存故障时 cp_trap() 把 PC 改到 cp_access_failed，返回 0。 */
int cp_access_load32(unsigned long addr, u32 *value);
int cp_access_load64(unsigned long addr, u64 *value);
int cp_access_store32(unsigned long addr, u32 value);
int cp_access_store64(unsigned long addr, u64 value);
/* 四条访存指令的地址，以及故障后的恢复点。 */
extern char cp_fault_load32[], cp_fault_load64[];
extern char cp_fault_store32[], cp_fault_store64[], cp_access_failed[];

/* ---- mem.c：GPU 地址访问 ---- */

/*
 * 以下函数经 CP 访存窗口访问一个 GPU 地址，成功返回 1。
 * domain 为访问域；vmid 与 slot 用于用户域翻译和故障归属；phase 记入故障。
 * 访问失败（含窗口触发的 access fault）返回 0，已执行的写入不重试。
 */
int cp_read32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u32 *value);
int cp_read64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u64 *value);
int cp_write32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u32 value);
int cp_write64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u64 value);
/* 按 32 位分段读取初始化后不再变化的对象；动态字段须用标量接口单独读取。 */
int cp_read_object(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, void *dest, u32 size);

/* 以 slot 自己的访问域与 VMID 读写（用于 Ring、进度、Signal 等队列对象）。 */
int cp_queue_read32(u32 slot, u32 phase, u64 addr, u32 *value);
int cp_queue_read64(u32 slot, u32 phase, u64 addr, u64 *value);
int cp_queue_write32(u32 slot, u32 phase, u64 addr, u32 value);
int cp_queue_write64(u32 slot, u32 phase, u64 addr, u64 value);
int cp_queue_read_object(u32 slot, u32 phase, u64 addr, void *dest, u32 size);

/* 陷阱入口调用：窗口访存故障返回恢复点，其余异常不返回。 */
u64 cp_trap(u64 cause, u64 epc, u64 tval);

/* ---- ih.c：通知与错误 ---- */

/* 经设备同步发布一条 IH 记录；失败时置 cp_stopped 并返回 0。 */
int cp_ih_submit(u32 slot, u32 type, u32 data);
/* 当 IH 通道已故障时置 cp_stopped，返回 1。 */
int cp_ih_faulted(void);
/* 停止 slot 取包并记录首次错误；首次出错时发 QUEUE_ERROR。KIQ 出错停止全部调度。 */
void cp_queue_error(u32 slot, u32 error);
/* 报告固件致命错误并停机。 */
void cp_fatal(void) __attribute__((noreturn));

/* ---- ring.c：取包与回收 ---- */

/* 定位 slot 中下一个待处理包；有包时写入 *addr 并返回 1。 */
int cp_next_packet(u32 slot, u64 *addr, u32 packet_size);
/* 推进 R 并写回共享进度；写回失败停止该队列并返回 0。 */
int cp_retire(u32 slot);

/* ---- mgmt.c / aql.c / main.c ---- */

/* 处理 KIQ 或 HIQ 中的一条管理命令。 */
void cp_management(u32 slot);
/*
 * 处理用户槽中的一个派发包。提交过执行器或调度已停止时返回 1；
 * 没有可执行的包，或提交执行器前就已出错时返回 0。
 */
int cp_aql(u32 slot);
/* 启动检查与调度循环。 */
void cp_main(void) __attribute__((noreturn));

#endif /* GPGPU_FIRMWARE_CP_H */
