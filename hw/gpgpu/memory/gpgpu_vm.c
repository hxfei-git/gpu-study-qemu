// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_vm.h"
#include "gpgpu_vram.h"
#include "gpgpu_host.h"
#include "hw/gpgpu/cp/gpgpu_hqd.h"

#include "hw/gpgpu/gpgpu_internal.h"

/* 首次故障按源槽锁存，普通用户错误不升级为全设备错误。 */
bool gpgpu_vm_fail(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, uint32_t access,
		   uint32_t phase, uint32_t reason)
{
	if (slot < GPGPU_HQD_SLOTS && !s->vm.fault[slot].reason) {
		s->vm.fault[slot] = (GPGPUVMFault) {
			.address = address,
			.reason = reason,
			.vmid = vmid,
			.access = access,
			.phase = phase,
		};
	}
	return false;
}

/* 用户不能触及 VRAM 首页或固件；Outbound 只检查路由，DMA 仍可能失败。 */
static bool user_physical_valid(GPGPUState *s, uint64_t address, uint64_t length)
{
	if (!gpgpu_phys_contains(s, address, length))
		return false;
	if (address >= s->vram_size)
		return true;
	return address >= GPGPU_VM_PAGE_SIZE &&
		(address >= (uint64_t)GPGPU_FIRMWARE_BASE + GPGPU_FIRMWARE_SIZE ||
		 (address < GPGPU_FIRMWARE_BASE && length <= GPGPU_FIRMWARE_BASE - address));
}

static bool vm_page_valid(GPGPUState *s, uint64_t address)
{
	return !(address & (GPGPU_VM_PAGE_SIZE - 1)) &&
		user_physical_valid(s, address, GPGPU_VM_PAGE_SIZE);
}

/* 同步遍历；表项不经过用户地址翻译，避免递归及管理域混淆。 */
static uint32_t vm_walk(GPGPUState *s, uint32_t vmid, uint32_t address, uint64_t *pte)
{
	unsigned int shifts[] = { 30, 21, 12 };
	uint64_t table = s->vm.contexts[vmid].root;

	for (unsigned int level = 0; level < 3; level++) {
		uint32_t index = (address >> shifts[level]) & (level ? 511 : 3);
		uint64_t entry;
		uint8_t raw[8];

		if (!vm_page_valid(s, table))
			return GPGPU_VM_FAULT_TABLE;
		if (gpgpu_phys_access(s, table + index * 8, raw, sizeof(raw), false) != MEMTX_OK)
			return GPGPU_VM_FAULT_TABLE;
		entry = ldq_le_p(raw);
		if (!(entry & GPGPU_VM_VALID))
			return GPGPU_VM_FAULT_UNMAPPED;
		if (entry & ~(GPGPU_VM_ADDR_MASK | (level == 2 ? GPGPU_VM_FLAGS : GPGPU_VM_VALID)))
			return GPGPU_VM_FAULT_TABLE;
		table = entry & GPGPU_VM_ADDR_MASK;
		if (level == 2) {
			if (!vm_page_valid(s, table))
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

	if (!s->gpuvm || vmid < 1 || vmid > GPGPU_VM_USERS || !s->vm.contexts[vmid].enabled)
		goto fail;
	reason = GPGPU_VM_FAULT_RANGE;
	if (!length || address >= GPGPU_VM_ADDRESS_LIMIT ||
	    length > GPGPU_VM_ADDRESS_LIMIT - address ||
	    (length <= 8 && (!is_power_of_2(length) || (address & (length - 1)))))
		goto fail;
	if (access != GPGPU_VM_READ && access != GPGPU_VM_WRITE && access != GPGPU_VM_EXEC)
		goto fail;
	s->vm.contexts[vmid].accesses++;
	counted = true;
	while (length) {
		uint32_t page = address >> 12;
		uint32_t offset = address & 4095;
		size_t count = MIN(length, GPGPU_VM_PAGE_SIZE - offset);
		GPGPUVMTlb *cached = &s->vm.tlb[(page ^ vmid) % ARRAY_SIZE(s->vm.tlb)];
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
		if (gpgpu_phys_access(s, physical, bytes, count, access == GPGPU_VM_WRITE) !=
		    MEMTX_OK)
			goto fail;
		address += count;
		bytes += count;
		length -= count;
	}
	s->vm.contexts[vmid].accesses--;
	return true;
fail:
	if (counted)
		s->vm.contexts[vmid].accesses--;
	return gpgpu_vm_fail(s, vmid, slot, address, access, phase, reason);
}

/* 访问域由调用者决定；GPUVM 开关只决定用户地址是否经过翻译。 */
bool gpgpu_user_access(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, void *data,
		       size_t length, uint32_t access, uint32_t phase)
{
	if (s->gpuvm)
		return gpgpu_vm_access(s, vmid, slot, address, data, length, access, phase);
	if (!length || (length <= 8 && (!is_power_of_2(length) || (address & (length - 1)))) ||
	    (access != GPGPU_VM_READ && access != GPGPU_VM_WRITE && access != GPGPU_VM_EXEC) ||
	    !user_physical_valid(s, address, length))
		return gpgpu_vm_fail(s, vmid, slot, address, access, phase, GPGPU_VM_FAULT_RANGE);
	if (gpgpu_phys_access(s, address, data, length, access == GPGPU_VM_WRITE) != MEMTX_OK)
		return gpgpu_vm_fail(s, vmid, slot, address, access, phase, GPGPU_VM_FAULT_STORAGE);
	return true;
}

/* 上下文有驻留队列或在途访问时，Host 不能变更页表根或失效。 */
static bool vm_busy(GPGPUState *s, unsigned int vmid)
{
	if (s->vm.contexts[vmid].accesses)
		return true;
	return gpgpu_hqd_vmid_active(s, vmid);
}

/* 读取只读能力、上下文或源槽的首次故障，不产生清除副作用。 */
uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr addr)
{
	if (addr == GPGPU_REG_VM_VERSION)
		return GPGPU_VM_VERSION;
	if (addr == GPGPU_REG_VM_MODE)
		return s->gpuvm ? 1 : 0;
	if (addr == GPGPU_REG_VM_FIRMWARE)
		return s->vm.firmware;
	if (addr == GPGPU_REG_VM_STATUS)
		return s->vm.status;
	for (unsigned int id = 1; id <= GPGPU_VM_USERS; id++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(id);

		if (addr == base + GPGPU_VM_ROOT_LO)
			return s->vm.contexts[id].root;
		if (addr == base + GPGPU_VM_ROOT_HI)
			return s->vm.contexts[id].root >> 32;
		if (addr == base + GPGPU_VM_ENABLE)
			return s->vm.contexts[id].enabled;
	}
	if (addr >= GPGPU_REG_VM_FAULT(0) && addr < GPGPU_REG_VM_FAULT(4)) {
		GPGPUVMFault *f = &s->vm.fault[(addr - GPGPU_REG_VM_FAULT(0)) / 0x40];

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
	if (addr == GPGPU_REG_VM_INVALIDATE) {
		if (!s->gpuvm)
			goto invalid;
		if (s->vm.status & GPGPU_VM_BUSY)
			goto invalid;
		s->vm.status = GPGPU_VM_BUSY;
		if (value < 1 || value > GPGPU_VM_USERS || vm_busy(s, value)) {
			s->vm.status = GPGPU_VM_ERROR;
			return;
		}
		for (unsigned int i = 0; i < ARRAY_SIZE(s->vm.tlb); i++)
			if (s->vm.tlb[i].vmid == value)
				memset(&s->vm.tlb[i], 0, sizeof(s->vm.tlb[i]));
		s->vm.status = GPGPU_VM_ACK;
		return;
	}
	for (unsigned int id = 1; id <= GPGPU_VM_USERS; id++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(id);
		GPGPUVMContext *vm = &s->vm.contexts[id];

		if (addr < base || addr >= base + 0x20)
			continue;
		if (!s->gpuvm || vm_busy(s, id))
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
	for (unsigned int slot = 0; slot < GPGPU_HQD_SLOTS; slot++) {
		if (addr == GPGPU_REG_VM_FAULT(slot) + GPGPU_VM_FAULT_CLEAR && value == 1 &&
		    !gpgpu_hqd_active(s, slot)) {
			memset(&s->vm.fault[slot], 0, sizeof(s->vm.fault[slot]));
			return;
		}
	}
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

bool gpgpu_vm_enabled(GPGPUState *s, unsigned int vmid)
{
	return vmid >= 1 && vmid <= GPGPU_VM_USERS && s->vm.contexts[vmid].enabled;
}

void gpgpu_vm_reset(GPGPUState *s)
{
	memset(&s->vm, 0, sizeof(s->vm));
}

void gpgpu_vm_reset_firmware(GPGPUState *s)
{
	s->vm.firmware = 0;
}

bool gpgpu_vm_set_firmware(GPGPUState *s, uint32_t version)
{
	if (version != GPGPU_VM_VERSION)
		return false;
	s->vm.firmware = version;
	return true;
}
