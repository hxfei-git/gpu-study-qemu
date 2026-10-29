// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPUVM：用户地址翻译、TLB、上下文寄存器与故障记录
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/cp/hqd.h"
#include "gpuvm.h"
#include "phys.h"

/* GPUVA 中页内偏移的位数。 */
#define VM_PAGE_SHIFT 12
/* 页表级数：L0 由 VA[31:30] 索引 4 项，L1、L2 由 9 位索引 512 项。 */
#define VM_LEVELS 3
/* 每个页表项的字节数。 */
#define VM_PTE_SIZE 8

static const unsigned int vm_level_shift[VM_LEVELS] = { 30, 21, 12 };
static const unsigned int vm_level_mask[VM_LEVELS] = { 3, 511, 511 };

void gpgpu_vm_record_fault(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr,
			   uint32_t access, uint32_t phase, uint32_t reason)
{
	if (slot >= GPGPU_HQD_SLOTS || s->vm.fault[slot].reason)
		return;
	s->vm.fault[slot] = (GPGPUVMFault) {
		.address = addr,
		.reason = reason,
		.vmid = vmid,
		.access = access,
		.phase = phase,
	};
}

/* 标量（不超过 8 字节）要求 2 的幂长度和自然对齐；访问类型必须是单一权限。 */
static bool user_request_valid(uint64_t addr, size_t len, uint32_t access)
{
	if (!len || (len <= 8 && (!is_power_of_2(len) || (addr & (len - 1)))))
		return false;
	return access == GPGPU_VM_READ || access == GPGPU_VM_WRITE || access == GPGPU_VM_EXEC;
}

/* 用户可见的物理范围：可路由，且不触及 VRAM 首页与固件保留区。 */
static bool user_phys_valid(GPGPUState *s, uint64_t addr, uint64_t len)
{
	const uint64_t fw_end = (uint64_t)GPGPU_FIRMWARE_BASE + GPGPU_FIRMWARE_SIZE;

	if (!gpgpu_phys_contains(s, addr, len))
		return false;
	if (addr >= gpgpu_vram_size(s))
		return true;
	if (addr < GPGPU_VM_PAGE_SIZE)
		return false;
	return addr >= fw_end || gpgpu_range_ok(addr, len, GPGPU_FIRMWARE_BASE);
}

/* 页表页与叶子目标页都必须是页对齐、用户可见的物理页。 */
static bool vm_page_valid(GPGPUState *s, uint64_t addr)
{
	return !(addr & (GPGPU_VM_PAGE_SIZE - 1)) && user_phys_valid(s, addr, GPGPU_VM_PAGE_SIZE);
}

/*
 * vm_walk() - 同步遍历三级页表，取得 va 的叶子项
 *
 * 表页按 GPU 物理地址直接读取，不再经过用户翻译。中间项只允许 VALID 位，
 * 叶子项只允许 VALID 与权限位。
 *
 * Return: 0 表示成功并写入 *pte，否则为 GPGPU_VM_FAULT_*。
 */
static uint32_t vm_walk(GPGPUState *s, uint32_t vmid, uint32_t va, uint64_t *pte)
{
	uint64_t table = s->vm.contexts[vmid].root;

	for (unsigned int level = 0; level < VM_LEVELS; level++) {
		bool leaf = level == VM_LEVELS - 1;
		uint32_t index = (va >> vm_level_shift[level]) & vm_level_mask[level];
		uint64_t allowed = GPGPU_VM_ADDR_MASK | (leaf ? GPGPU_VM_FLAGS : GPGPU_VM_VALID);
		uint8_t raw[VM_PTE_SIZE];
		uint64_t entry;

		if (!vm_page_valid(s, table) ||
		    gpgpu_phys_rw(s, table + index * VM_PTE_SIZE, raw, sizeof(raw), false) != MEMTX_OK)
			return GPGPU_VM_FAULT_TABLE;
		entry = ldq_le_p(raw);
		if (!(entry & GPGPU_VM_VALID))
			return GPGPU_VM_FAULT_UNMAPPED;
		if (entry & ~allowed)
			return GPGPU_VM_FAULT_TABLE;
		table = entry & GPGPU_VM_ADDR_MASK;
		if (leaf) {
			if (!vm_page_valid(s, table))
				return GPGPU_VM_FAULT_TABLE;
			*pte = entry;
		}
	}
	return 0;
}

/* 先查 TLB，未命中则遍历页表并填回；返回 0 或故障原因。 */
static uint32_t vm_lookup(GPGPUState *s, uint32_t vmid, uint32_t va, uint64_t *pte)
{
	uint32_t page = va >> VM_PAGE_SHIFT;
	GPGPUVMTlbEntry *entry = &s->vm.tlb[(page ^ vmid) % GPGPU_VM_TLB_ENTRIES];
	uint32_t reason;

	if (entry->pte && entry->vmid == vmid && entry->page == page) {
		*pte = entry->pte;
		return 0;
	}
	reason = vm_walk(s, vmid, va, pte);
	if (!reason)
		*entry = (GPGPUVMTlbEntry) { .pte = *pte, .page = page, .vmid = vmid };
	return reason;
}

/*
 * vm_translated_rw() - gpuvm=on 时的用户访问
 *
 * 1. 检查上下文与请求；2. 计入上下文在途访问，阻止并发失效；
 * 3. 逐页翻译、检查权限并访问物理地址；4. 撤销计数，失败时记录故障。
 * 参数按值保存，跨进程 DMA 期间的寄存器写入不影响本次访问。
 */
static bool vm_translated_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr,
			     uint8_t *buf, size_t len, uint32_t access, uint32_t phase)
{
	GPGPUVMContext *ctx;
	uint32_t reason;

	/* 1. 上下文与请求。 */
	if (!gpgpu_vm_context_enabled(s, vmid)) {
		reason = GPGPU_VM_FAULT_CONTEXT;
		goto fail;
	}
	if (!gpgpu_range_ok(addr, len, GPGPU_VM_ADDRESS_LIMIT) ||
	    !user_request_valid(addr, len, access)) {
		reason = GPGPU_VM_FAULT_RANGE;
		goto fail;
	}

	/* 2. 在途计数。 */
	ctx = &s->vm.contexts[vmid];
	ctx->accesses++;

	/* 3. 逐页访问。 */
	reason = 0;
	while (len && !reason) {
		uint32_t offset = addr & (GPGPU_VM_PAGE_SIZE - 1);
		size_t chunk = MIN(len, GPGPU_VM_PAGE_SIZE - offset);
		uint64_t pte;

		reason = vm_lookup(s, vmid, addr, &pte);
		if (!reason && !(pte & access))
			reason = GPGPU_VM_FAULT_PERMISSION;
		if (!reason && gpgpu_phys_rw(s, (pte & GPGPU_VM_ADDR_MASK) + offset, buf, chunk,
					     access == GPGPU_VM_WRITE) != MEMTX_OK)
			reason = GPGPU_VM_FAULT_STORAGE;
		if (!reason) {
			addr += chunk;
			buf += chunk;
			len -= chunk;
		}
	}

	/* 4. 撤销计数。 */
	ctx->accesses--;
	if (!reason)
		return true;
fail:
	gpgpu_vm_record_fault(s, vmid, slot, addr, access, phase, reason);
	return false;
}

/* gpuvm=off 时的用户访问：地址即 GPU 物理地址，只检查保护区。 */
static bool vm_direct_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr, void *buf,
			 size_t len, uint32_t access, uint32_t phase)
{
	uint32_t reason = 0;

	if (!user_request_valid(addr, len, access) || !user_phys_valid(s, addr, len))
		reason = GPGPU_VM_FAULT_RANGE;
	else if (gpgpu_phys_rw(s, addr, buf, len, access == GPGPU_VM_WRITE) != MEMTX_OK)
		reason = GPGPU_VM_FAULT_STORAGE;
	if (!reason)
		return true;
	gpgpu_vm_record_fault(s, vmid, slot, addr, access, phase, reason);
	return false;
}

bool gpgpu_user_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr, void *buf,
		   size_t len, uint32_t access, uint32_t phase)
{
	if (s->gpuvm)
		return vm_translated_rw(s, vmid, slot, addr, buf, len, access, phase);
	return vm_direct_rw(s, vmid, slot, addr, buf, len, access, phase);
}

bool gpgpu_vm_context_enabled(GPGPUState *s, unsigned int vmid)
{
	return vmid >= 1 && vmid <= GPGPU_VM_USERS && s->vm.contexts[vmid].enabled;
}

/* 上下文有激活的用户队列或在途访问时，Host 不能修改它或使其失效。 */
static bool vm_context_busy(GPGPUState *s, unsigned int vmid)
{
	return s->vm.contexts[vmid].accesses || gpgpu_hqd_vmid_active(s, vmid);
}

/* 把寄存器偏移解析为用户上下文：返回 VMID，*field 为组内偏移；不在任何上下文内返回 0。 */
static unsigned int vm_context_of(hwaddr reg, hwaddr *field)
{
	for (unsigned int vmid = 1; vmid <= GPGPU_VM_USERS; vmid++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(vmid);

		if (reg >= base && reg < base + 0x20) {
			*field = reg - base;
			return vmid;
		}
	}
	return 0;
}

/* 把寄存器偏移解析为故障组：返回槽号，*field 为组内偏移；不在故障区返回 -1。 */
static int vm_fault_slot_of(hwaddr reg, hwaddr *field)
{
	if (reg < GPGPU_REG_VM_FAULT(0) || reg >= GPGPU_REG_VM_FAULT(GPGPU_HQD_SLOTS))
		return -1;
	*field = (reg - GPGPU_REG_VM_FAULT(0)) % 0x40;
	return (reg - GPGPU_REG_VM_FAULT(0)) / 0x40;
}

static uint32_t vm_fault_read(const GPGPUVMFault *f, hwaddr field)
{
	switch (field) {
	case GPGPU_VM_FAULT_REASON:
		return f->reason;
	case GPGPU_VM_FAULT_VMID:
		return f->vmid;
	case GPGPU_VM_FAULT_ADDR_LO:
	case GPGPU_VM_FAULT_ADDR_HI:
		return gpgpu_reg64_read(f->address, field - GPGPU_VM_FAULT_ADDR_LO);
	case GPGPU_VM_FAULT_ACCESS:
		return f->access;
	case GPGPU_VM_FAULT_PHASE:
		return f->phase;
	default:
		return 0;
	}
}

uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr reg)
{
	unsigned int vmid;
	hwaddr field;
	int slot;

	switch (reg) {
	case GPGPU_REG_VM_VERSION:
		return GPGPU_PROTOCOL_VERSION;
	case GPGPU_REG_VM_MODE:
		return s->gpuvm;
	case GPGPU_REG_VM_FIRMWARE:
		return s->vm.firmware;
	case GPGPU_REG_VM_STATUS:
		return s->vm.status;
	}
	vmid = vm_context_of(reg, &field);
	if (vmid) {
		const GPGPUVMContext *ctx = &s->vm.contexts[vmid];

		if (field == GPGPU_VM_ROOT_LO || field == GPGPU_VM_ROOT_HI)
			return gpgpu_reg64_read(ctx->root, field - GPGPU_VM_ROOT_LO);
		return field == GPGPU_VM_ENABLE ? ctx->enabled : 0;
	}
	slot = vm_fault_slot_of(reg, &field);
	return slot >= 0 ? vm_fault_read(&s->vm.fault[slot], field) : 0;
}

/* 同步失效 vmid 的全部 TLB 项；VM 仍在使用时只报告 ERROR。 */
static void vm_invalidate(GPGPUState *s, uint32_t vmid)
{
	if (vmid < 1 || vmid > GPGPU_VM_USERS || vm_context_busy(s, vmid)) {
		s->vm.status = GPGPU_VM_ERROR;
		return;
	}
	for (unsigned int i = 0; i < GPGPU_VM_TLB_ENTRIES; i++)
		if (s->vm.tlb[i].vmid == vmid)
			memset(&s->vm.tlb[i], 0, sizeof(s->vm.tlb[i]));
	s->vm.status = GPGPU_VM_ACK;
}

/*
 * 上下文寄存器写入规则：VM 忙时全部拒绝；ENABLE 需要合法根页；启用后根只读；
 * ROOT_LO 须页对齐，ROOT_HI 只接受 0。
 */
static bool vm_context_write(GPGPUState *s, unsigned int vmid, hwaddr field, uint32_t value)
{
	GPGPUVMContext *ctx = &s->vm.contexts[vmid];

	if (vm_context_busy(s, vmid))
		return false;
	if (field == GPGPU_VM_ENABLE) {
		if (value > 1 || (value && !vm_page_valid(s, ctx->root)))
			return false;
		ctx->enabled = value;
		return true;
	}
	if (ctx->enabled)
		return false;
	if (field == GPGPU_VM_ROOT_LO && !(value & (GPGPU_VM_PAGE_SIZE - 1))) {
		gpgpu_reg64_write(&ctx->root, 0, value);
		return true;
	}
	if (field == GPGPU_VM_ROOT_HI && !value) {
		gpgpu_reg64_write(&ctx->root, 4, 0);
		return true;
	}
	return false;
}

/*
 * Host 写入分三类：INVALIDATE、上下文寄存器、故障清除。其余偏移以及
 * gpuvm=off 时的前两类都锁存 INVALID_CMD。
 */
void gpgpu_vm_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	unsigned int vmid;
	hwaddr field;
	int slot;

	if (reg == GPGPU_REG_VM_INVALIDATE) {
		if (!s->gpuvm)
			goto invalid;
		vm_invalidate(s, value);
		return;
	}
	vmid = vm_context_of(reg, &field);
	if (vmid) {
		if (!s->gpuvm || !vm_context_write(s, vmid, field, value))
			goto invalid;
		return;
	}
	slot = vm_fault_slot_of(reg, &field);
	if (slot >= 0 && field == GPGPU_VM_FAULT_CLEAR && value == 1 &&
	    !gpgpu_hqd_active(s, slot)) {
		memset(&s->vm.fault[slot], 0, sizeof(s->vm.fault[slot]));
		return;
	}
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

bool gpgpu_vm_firmware_handshake(GPGPUState *s, uint32_t version)
{
	if (version != GPGPU_PROTOCOL_VERSION)
		return false;
	s->vm.firmware = version;
	return true;
}

void gpgpu_vm_firmware_reset(GPGPUState *s)
{
	s->vm.firmware = 0;
}

void gpgpu_vm_reset(GPGPUState *s)
{
	memset(&s->vm, 0, sizeof(s->vm));
}
