// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CP 固件：经访存窗口读写 GPU 地址，以及访存异常的恢复
 *
 * 一次访问：
 *   1. 写窗口配置：访问域、VMID、故障归属槽、阶段、目标基址（64 KiB 对齐）；
 *   2. 用 start.S 中的恢复型访存指令读写“窗口基址 + 地址低 16 位”；
 *   3. 设备访问失败时 CP 触发 access fault，cp_trap() 把 PC 改到恢复点，
 *      访存函数返回 0。
 */
#include "cp.h"

/*
 * 让访存窗口指向 addr 所在的 64 KiB 区域，返回 addr 在 CP 本地的地址。
 * 前一个 fence 保证上一次窗口访问已经完成，才改写窗口配置；后一个 fence
 * 保证全部配置生效后，调用者才通过返回的地址访问。BASE_LO 最后写入，
 * 只保留 64 KiB 对齐的部分，页内偏移由返回地址的低 16 位给出。
 */
static unsigned long cp_window(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr)
{
	cp_fence();
	cp_window_write(GPGPU_CP_WINDOW_DOMAIN, domain);
	cp_window_write(GPGPU_CP_WINDOW_VMID, vmid);
	cp_window_write(GPGPU_CP_WINDOW_SLOT, slot);
	cp_window_write(GPGPU_CP_WINDOW_PHASE, phase);
	cp_window_write(GPGPU_CP_WINDOW_BASE_HI, addr >> 32);
	cp_window_write(GPGPU_CP_WINDOW_BASE_LO, (u32)addr & ~(GPGPU_CP_WINDOW_SIZE - 1));
	cp_fence();
	return GPGPU_CP_WINDOW_BASE + (addr & (GPGPU_CP_WINDOW_SIZE - 1));
}

int cp_read32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u32 *value)
{
	if (!cp_range_ok(addr, 4, 4))
		return 0;
	return cp_access_load32(cp_window(slot, domain, vmid, phase, addr), value);
}

/* 单条 64 位 load，保证读到完整的旧值或新值。 */
int cp_read64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u64 *value)
{
	if (!cp_range_ok(addr, 8, 8))
		return 0;
	return cp_access_load64(cp_window(slot, domain, vmid, phase, addr), value);
}

int cp_write32(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u32 value)
{
	if (!cp_range_ok(addr, 4, 4))
		return 0;
	return cp_access_store32(cp_window(slot, domain, vmid, phase, addr), value);
}

/* 单条 64 位 store，用于发布 Signal、通知槽和管理完成序号。 */
int cp_write64(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, u64 value)
{
	if (!cp_range_ok(addr, 8, 8))
		return 0;
	return cp_access_store64(cp_window(slot, domain, vmid, phase, addr), value);
}

/*
 * 按 4 字节逐个读取对象，每次都重新配置窗口，
 * 所以对象可以跨 64 KiB 边界。
 * 只用于 MQD、AQL 包体、描述对象这类读取期间不会被另一方修改的内容。
 */
int cp_read_object(u32 slot, u32 domain, u32 vmid, u32 phase, u64 addr, void *dest, u32 size)
{
	u32 *words = dest;

	if (!cp_range_ok(addr, size, 4) || size % 4)
		return 0;
	for (u32 i = 0; i < size / 4; i++)
		if (!cp_read32(slot, domain, vmid, phase, addr + 4 * i, &words[i]))
			return 0;
	return 1;
}

int cp_queue_read32(u32 slot, u32 phase, u64 addr, u32 *value)
{
	return cp_read32(slot, cp_slots[slot].domain, cp_slots[slot].vmid, phase, addr, value);
}

int cp_queue_read64(u32 slot, u32 phase, u64 addr, u64 *value)
{
	return cp_read64(slot, cp_slots[slot].domain, cp_slots[slot].vmid, phase, addr, value);
}

int cp_queue_write32(u32 slot, u32 phase, u64 addr, u32 value)
{
	return cp_write32(slot, cp_slots[slot].domain, cp_slots[slot].vmid, phase, addr, value);
}

int cp_queue_write64(u32 slot, u32 phase, u64 addr, u64 value)
{
	return cp_write64(slot, cp_slots[slot].domain, cp_slots[slot].vmid, phase, addr, value);
}

int cp_queue_read_object(u32 slot, u32 phase, u64 addr, void *dest, u32 size)
{
	return cp_read_object(slot, cp_slots[slot].domain, cp_slots[slot].vmid, phase, addr, dest,
			      size);
}

/* 是否为四条恢复型访存指令之一。 */
static int cp_is_window_access(u64 epc)
{
	return epc == (u64)(unsigned long)cp_fault_load32 ||
		epc == (u64)(unsigned long)cp_fault_load64 ||
		epc == (u64)(unsigned long)cp_fault_store32 ||
		epc == (u64)(unsigned long)cp_fault_store64;
}

/*
 * cp_trap() - 异常处理
 *
 * 只有恢复型访存指令在窗口内触发的 access fault 可以恢复：返回恢复点，
 * 访存函数以 0 返回。其余异常都是固件缺陷，报告致命错误并停机。
 */
u64 cp_trap(u64 cause, u64 epc, u64 tval)
{
	if ((cause == RISCV_CAUSE_LOAD_ACCESS || cause == RISCV_CAUSE_STORE_ACCESS) &&
	    tval >= GPGPU_CP_WINDOW_BASE && tval < GPGPU_CP_WINDOW_BASE + GPGPU_CP_WINDOW_SIZE &&
	    cp_is_window_access(epc))
		return (unsigned long)cp_access_failed;
	cp_fatal();
}
