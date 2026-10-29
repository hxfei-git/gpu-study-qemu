/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cp.h"

volatile CpAccess cp_access;
/* 通过可恢复汇编读取 32 位标量。 */
extern int cp_access_load32(unsigned long address, u32 *value);
/* 通过可恢复汇编读取 64 位标量。 */
extern int cp_access_load64(unsigned long address, u64 *value);
/* 通过可恢复汇编写入 32 位标量。 */
extern int cp_access_store32(unsigned long address, u32 value);
/* 通过可恢复汇编写入 64 位标量。 */
extern int cp_access_store64(unsigned long address, u64 value);
extern char cp_fault_load32[], cp_fault_load64[];
extern char cp_fault_store32[], cp_fault_store64[], cp_access_failed[];

/*
 * cp_window() - 将一次 GPU 存储访问映射到 CP 本地窗口
 *
 * 先记录槽位和阶段，再配置 64 KiB 窗口基址并返回窗口内地址。
 */
static unsigned long cp_window(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address)
{
	cp_access.slot = slot;
	cp_access.phase = phase;
	cp_access.address = address;
	cp_fence();
	cp_window_write(GPGPU_CP_WINDOW_DOMAIN, domain);
	cp_window_write(GPGPU_CP_WINDOW_VMID, vmid);
	cp_window_write(GPGPU_CP_WINDOW_SLOT, slot);
	cp_window_write(GPGPU_CP_WINDOW_PHASE, phase);
	cp_window_write(GPGPU_CP_WINDOW_BASE_HI, address >> 32);
	cp_window_write(GPGPU_CP_WINDOW_BASE_LO, (u32)address & ~(GPGPU_CP_WINDOW_SIZE - 1));
	cp_fence();
	return GPGPU_CP_WINDOW_BASE + (address & (GPGPU_CP_WINDOW_SIZE - 1));
}

/*
 * cp_read32() - 从指定域完整读取一个 32 位标量
 *
 * 校验范围后调用可恢复访存汇编；成功返回 1，异常或非法地址返回 0。
 */
int cp_read32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u32 *value)
{
	if (!cp_range(address, 4, 4))
		return 0;
	return cp_access_load32(cp_window(slot, domain, vmid, phase, address), value);
}

/*
 * cp_read64() - 从指定域完整读取一个 64 位标量
 *
 * 使用单条 64 位 load，不能拆成两次 32 位共享值读取；成功返回 1。
 */
int cp_read64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u64 *value)
{
	if (!cp_range(address, 8, 8))
		return 0;
	return cp_access_load64(cp_window(slot, domain, vmid, phase, address), value);
}

/*
 * cp_write32() - 向指定域完整写入一个 32 位标量
 *
 * 记录访问归属并调用可恢复 store；失败返回 0，不重试已经执行的写入。
 */
int cp_write32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u32 value)
{
	if (!cp_range(address, 4, 4))
		return 0;
	return cp_access_store32(cp_window(slot, domain, vmid, phase, address), value);
}

/*
 * cp_write64() - 向指定域完整写入一个 64 位标量
 *
 * 使用单条 64 位 store 发布 Signal、通知槽或完成序号；成功返回 1。
 */
int cp_write64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, u64 value)
{
	if (!cp_range(address, 8, 8))
		return 0;
	return cp_access_store64(cp_window(slot, domain, vmid, phase, address), value);
}

/*
 * cp_read_object() - 读取初始化后固定的 GPU 对象
 *
 * 按 32 位分段复制，任一访问失败即停止；动态 Header 和进度须单独读取。
 */
int cp_read_object(u32 slot, u32 domain, u32 vmid, u32 phase, u64 address, void *dest, u32 size)
{
	u32 *words = dest;
	u32 i;

	if (!cp_range(address, size, 4) || size % 4)
		return 0;
	/* 对象初始化后固定；动态首部与进度单独使用完整标量接口。 */
	for (i = 0; i < size / 4; i++)
		if (!cp_read32(slot, domain, vmid, phase, address + 4 * i, &words[i]))
			return 0;
	return 1;
}

/*
 * cp_trap() - 处理 RV64 固件异常
 *
 * 已知 Host 访存指令故障跳到统一失败返回点；其他异常进入固件停机。
 */
u64 cp_trap(u64 cause, u64 epc, u64 address)
{
	cp_access.cause = cause;
	cp_access.fault_address = address;
	if ((cause == RISCV_CAUSE_LOAD_ACCESS || cause == RISCV_CAUSE_STORE_ACCESS) &&
	    address >= GPGPU_CP_WINDOW_BASE &&
	    address < GPGPU_CP_WINDOW_BASE + GPGPU_CP_WINDOW_SIZE &&
	    (epc == (u64)(unsigned long)cp_fault_load32 ||
	     epc == (u64)(unsigned long)cp_fault_load64 ||
	     epc == (u64)(unsigned long)cp_fault_store32 ||
	     epc == (u64)(unsigned long)cp_fault_store64)) {
		return (unsigned long)cp_access_failed;
	}
	cp_fatal();
}
