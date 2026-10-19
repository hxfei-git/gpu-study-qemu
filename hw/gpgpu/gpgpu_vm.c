/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/remote/vfio-user-obj.h"
#include "gpgpu.h"

/* 首次故障按源槽锁存，普通用户错误不升级为全设备错误。 */
bool gpgpu_vm_fail(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, uint32_t access,
		   uint32_t phase, uint32_t reason)
{
	if (slot < GPGPU_QUEUE_SLOTS && !s->vm_fault[slot].reason) {
		s->vm_fault[slot] = (GPGPUVMFault) {
			.address = address,
			.reason = reason,
			.vmid = vmid,
			.access = access,
			.phase = phase,
		};
	}
	return false;
}

/* 表页和用户 VRAM 页均必须完整位于普通存储池，不能触及固件。 */
static bool vm_page_valid(GPGPUState *s, uint64_t address)
{
	return !(address & (GPGPU_VM_PAGE_SIZE - 1)) && address >= GPGPU_VM_PAGE_SIZE &&
		address <= GPGPU_VRAM_USABLE_SIZE - GPGPU_VM_PAGE_SIZE &&
		gpgpu_vram_contains(s, address, GPGPU_VM_PAGE_SIZE);
}

/* 同步遍历；表项不经过用户地址翻译，避免递归及管理域混淆。 */
static uint32_t vm_walk(GPGPUState *s, uint32_t vmid, uint32_t address, uint64_t *pte)
{
	unsigned int shifts[] = { 30, 21, 12 };
	uint64_t table = s->vm[vmid].root;

	for (unsigned int level = 0; level < 3; level++) {
		uint32_t index = (address >> shifts[level]) & (level ? 511 : 3);
		uint64_t entry;

		if (!vm_page_valid(s, table))
			return GPGPU_VM_FAULT_TABLE;
		entry = ldq_le_p(s->vram_ptr + table + index * 8);
		if (!(entry & GPGPU_VM_VALID))
			return GPGPU_VM_FAULT_UNMAPPED;
		if (entry & ~(GPGPU_VM_ADDR_MASK | (level == 2 ? GPGPU_VM_FLAGS : GPGPU_VM_VALID)))
			return GPGPU_VM_FAULT_TABLE;
		table = entry & GPGPU_VM_ADDR_MASK;
		if (level == 2) {
			if (!(entry & GPGPU_VM_SYSTEM) && !vm_page_valid(s, table))
				return GPGPU_VM_FAULT_TABLE;
			*pte = entry;
		}
	}
	return 0;
}

/*
 * gpgpu_vm_access() - 唯一用户存储访问入口
 *
 * 参数按值保存，DMA 期间的回调不能改变本次上下文。标量保持完整读写；
 * 块传输逐页检查，后续页失败保留前面已经完成的写入。
 */
bool gpgpu_vm_access(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, void *data,
		     size_t length, uint32_t access, uint32_t phase)
{
	uint8_t *bytes = data;
	uint32_t reason = GPGPU_VM_FAULT_CONTEXT;
	bool counted = false;

	if (!s->gpuvm || vmid < 1 || vmid > GPGPU_VM_USERS || !s->vm[vmid].enabled)
		goto fail;
	reason = GPGPU_VM_FAULT_RANGE;
	if (!length || address >= GPGPU_VM_ADDRESS_LIMIT ||
	    length > GPGPU_VM_ADDRESS_LIMIT - address ||
	    (length <= 8 && (!is_power_of_2(length) || (address & (length - 1)))))
		goto fail;
	if (access != GPGPU_VM_READ && access != GPGPU_VM_WRITE && access != GPGPU_VM_EXEC)
		goto fail;
	s->vm[vmid].accesses++;
	counted = true;
	while (length) {
		uint32_t page = address >> 12;
		uint32_t offset = address & 4095;
		size_t count = MIN(length, GPGPU_VM_PAGE_SIZE - offset);
		GPGPUVMTlb *cached = &s->tlb[(page ^ vmid) % ARRAY_SIZE(s->tlb)];
		uint64_t pte, physical;

		if (cached->vmid == vmid && cached->page == page && cached->pte) {
			pte = cached->pte;
		} else {
			reason = vm_walk(s, vmid, address, &pte);
			if (reason)
				goto fail;
			*cached = (GPGPUVMTlb) { .vmid = vmid, .page = page, .pte = pte };
		}
		reason = GPGPU_VM_FAULT_PERMISSION;
		if (!(pte & access))
			goto fail;
		physical = (pte & GPGPU_VM_ADDR_MASK) + offset;
		reason = GPGPU_VM_FAULT_STORAGE;
		if (pte & GPGPU_VM_SYSTEM) {
			if (vfu_object_dma_rw(PCI_DEVICE(s), physical, bytes, count,
					      access == GPGPU_VM_WRITE) != MEMTX_OK)
				goto fail;
		} else if (access == GPGPU_VM_WRITE) {
			memcpy(s->vram_ptr + physical, bytes, count);
		} else {
			memcpy(bytes, s->vram_ptr + physical, count);
		}
		address += count;
		bytes += count;
		length -= count;
	}
	s->vm[vmid].accesses--;
	return true;
fail:
	if (counted)
		s->vm[vmid].accesses--;
	return gpgpu_vm_fail(s, vmid, slot, address, access, phase, reason);
}

/* 上下文有驻留队列或在途访问时，Host 不能变更页表根或失效。 */
static bool vm_busy(GPGPUState *s, unsigned int vmid)
{
	if (s->vm[vmid].accesses)
		return true;
	for (unsigned int slot = 2; slot < GPGPU_QUEUE_SLOTS; slot++)
		if (s->hqd[slot][GPGPU_HQD_ACTIVE / 4] && s->hqd[slot][GPGPU_HQD_VMID / 4] == vmid)
			return true;
	return false;
}

/* 读取只读能力、上下文或源槽的首次故障，不产生清除副作用。 */
uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr addr)
{
	if (addr == GPGPU_REG_VM_VERSION)
		return GPGPU_VM_VERSION;
	if (addr == GPGPU_REG_VM_MODE)
		return s->gpuvm ? 2 : 0;
	if (addr == GPGPU_REG_VM_FIRMWARE)
		return s->vm_firmware;
	if (addr == GPGPU_REG_VM_STATUS)
		return s->vm_status;
	for (unsigned int id = 1; id <= GPGPU_VM_USERS; id++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(id);

		if (addr == base + GPGPU_VM_ROOT_LO)
			return s->vm[id].root;
		if (addr == base + GPGPU_VM_ROOT_HI)
			return s->vm[id].root >> 32;
		if (addr == base + GPGPU_VM_ENABLE)
			return s->vm[id].enabled;
	}
	if (addr >= GPGPU_REG_VM_FAULT(0) && addr < GPGPU_REG_VM_FAULT(4)) {
		GPGPUVMFault *f = &s->vm_fault[(addr - GPGPU_REG_VM_FAULT(0)) / 0x40];

		switch (addr & 0x3f) {
		case GPGPU_VM_FAULT_REASON:
			return f->reason;
		case GPGPU_VM_FAULT_VMID:
			return f->vmid;
		case GPGPU_VM_FAULT_ADDR_LO:
			return f->address;
		case GPGPU_VM_FAULT_ADDR_HI:
			return f->address >> 32;
		case GPGPU_VM_FAULT_ACCESS:
			return f->access;
		case GPGPU_VM_FAULT_PHASE:
			return f->phase;
		}
	}
	return 0;
}

/* 同步失效先清旧 ACK；设备主锁及访问计数阻止旧遍历重新填充。 */
void gpgpu_vm_write(GPGPUState *s, hwaddr addr, uint32_t value)
{
	if (!s->gpuvm)
		goto invalid;
	if (addr == GPGPU_REG_VM_INVALIDATE) {
		if (s->vm_status & GPGPU_VM_BUSY)
			goto invalid;
		s->vm_status = GPGPU_VM_BUSY;
		if (value < 1 || value > GPGPU_VM_USERS || vm_busy(s, value)) {
			s->vm_status = GPGPU_VM_ERROR;
			return;
		}
		for (unsigned int i = 0; i < ARRAY_SIZE(s->tlb); i++)
			if (s->tlb[i].vmid == value)
				memset(&s->tlb[i], 0, sizeof(s->tlb[i]));
		s->vm_status = GPGPU_VM_ACK;
		return;
	}
	for (unsigned int id = 1; id <= GPGPU_VM_USERS; id++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(id);
		GPGPUVMContext *vm = &s->vm[id];

		if (addr < base || addr >= base + 0x20)
			continue;
		if (vm_busy(s, id))
			goto invalid;
		if (addr == base + GPGPU_VM_ENABLE) {
			if (value > 1 || (value && !vm_page_valid(s, vm->root)))
				goto invalid;
			vm->enabled = value;
			return;
		}
		if (vm->enabled)
			goto invalid;
		if (addr == base + GPGPU_VM_ROOT_LO && !(value & 4095)) {
			vm->root = (vm->root & 0xffffffff00000000ULL) | value;
			return;
		}
		if (addr == base + GPGPU_VM_ROOT_HI && !value) {
			vm->root &= 0xffffffffULL;
			return;
		}
		goto invalid;
	}
	for (unsigned int slot = 0; slot < GPGPU_QUEUE_SLOTS; slot++) {
		if (addr == GPGPU_REG_VM_FAULT(slot) + GPGPU_VM_FAULT_CLEAR && value == 1 &&
		    !s->hqd[slot][GPGPU_HQD_ACTIVE / 4]) {
			memset(&s->vm_fault[slot], 0, sizeof(s->vm_fault[slot]));
			return;
		}
	}
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}
