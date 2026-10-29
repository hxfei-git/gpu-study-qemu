// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HQD：槽寄存器的检查与读写
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/mem/gpuvm.h"
#include "cp_ctrl.h"
#include "hqd.h"

/* 已实现的槽内字段；其余偏移读零，写入锁存 INVALID_CMD。 */
static bool hqd_field_defined(hwaddr field)
{
	switch (field) {
	case GPGPU_HQD_MQD_LO:
	case GPGPU_HQD_MQD_HI:
	case GPGPU_HQD_ACTIVE:
	case GPGPU_HQD_VMID:
	case GPGPU_HQD_RING_LO:
	case GPGPU_HQD_RING_HI:
	case GPGPU_HQD_RPTR:
	case GPGPU_HQD_RPTR_ADDR_LO:
	case GPGPU_HQD_RPTR_ADDR_HI:
	case GPGPU_HQD_WPTR_ADDR_LO:
	case GPGPU_HQD_WPTR_ADDR_HI:
	case GPGPU_HQD_DOORBELL:
	case GPGPU_HQD_CONTROL:
	case GPGPU_HQD_ERROR:
	case GPGPU_HQD_AQL:
		return true;
	default:
		return false;
	}
}

static uint32_t hqd_get(const uint32_t *hqd, hwaddr field)
{
	return hqd[field / 4];
}

/* 读取槽内 LO/HI 字段对组成的 64 位地址。 */
static uint64_t hqd_get64(const uint32_t *hqd, hwaddr lo)
{
	return hqd_get(hqd, lo) | (uint64_t)hqd_get(hqd, lo + 4) << 32;
}

/* 地址按 align 对齐，且 [addr, addr + len) 位于 32 位 GPU 地址空间内。 */
static bool hqd_range_ok(uint64_t addr, uint64_t len, uint64_t align)
{
	return !(addr & (align - 1)) && gpgpu_range_ok(addr, len, GPGPU_VM_ADDRESS_LIMIT);
}

static bool ring_size_valid(uint32_t size)
{
	return size >= GPGPU_QUEUE_RING_MIN && size <= GPGPU_QUEUE_RING_MAX && is_power_of_2(size);
}

/*
 * hqd_ready_to_activate() - 检查槽能否从停用转为激活
 *
 * 所有槽：Ring 容量合法；MQD、Ring、R/W 进度地址对齐且不越出 32 位空间。
 * 用户槽另需：VMID 为 1 或 2；gpuvm=on 时该 VM 的上下文已启用。
 */
static bool hqd_ready_to_activate(GPGPUState *s, unsigned int slot, const uint32_t *hqd)
{
	uint32_t size = hqd_get(hqd, GPGPU_HQD_CONTROL);
	uint32_t vmid = hqd_get(hqd, GPGPU_HQD_VMID);

	if (!ring_size_valid(size) || !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_MQD_LO), 64, 8) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_RING_LO), size, 64) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_RPTR_ADDR_LO), 4, 8) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_WPTR_ADDR_LO), 4, 8))
		return false;
	if (slot < GPGPU_HQD_USER_FIRST)
		return true;
	return vmid >= 1 && vmid <= GPGPU_VM_USERS &&
		(!gpgpu_gpuvm_enabled(s) || gpgpu_vm_context_enabled(s, vmid));
}

/* 单个字段的取值规则。 */
static bool hqd_value_valid(GPGPUState *s, unsigned int slot, const uint32_t *hqd, hwaddr field,
			    uint32_t value)
{
	uint32_t db_offset = value & GPGPU_DOORBELL_OFFSET_MASK;

	switch (field) {
	case GPGPU_HQD_MQD_LO:
	case GPGPU_HQD_RPTR_ADDR_LO:
	case GPGPU_HQD_WPTR_ADDR_LO:
		return !(value & 7);
	case GPGPU_HQD_RING_LO:
		return !(value & 63);
	case GPGPU_HQD_ACTIVE:
		return value <= 1 && (!value || hqd_ready_to_activate(s, slot, hqd));
	case GPGPU_HQD_VMID:
		return slot >= GPGPU_HQD_USER_FIRST ? value <= GPGPU_VM_USERS : value == 0;
	case GPGPU_HQD_AQL:
		return value <= 1;
	case GPGPU_HQD_CONTROL:
		return ring_size_valid(value);
	case GPGPU_HQD_DOORBELL:
		return !(value & ~(GPGPU_DOORBELL_ENABLE | GPGPU_DOORBELL_OFFSET_MASK)) &&
			!(db_offset & 7) && db_offset <= GPGPU_DOORBELL_BAR_SIZE - 8;
	case GPGPU_HQD_ERROR:
		/* 激活期间不能清除已记录的错误。 */
		return value <= GPGPU_QUEUE_ERROR_SAVE &&
			!(hqd_get(hqd, GPGPU_HQD_ERROR) && !value && hqd_get(hqd, GPGPU_HQD_ACTIVE));
	default:
		return true;
	}
}

/* 激活期间只允许改 ACTIVE、RPTR、ERROR，以及只翻转 Doorbell 使能位。 */
static bool hqd_writable_while_active(const uint32_t *hqd, hwaddr field, uint32_t value)
{
	switch (field) {
	case GPGPU_HQD_ACTIVE:
	case GPGPU_HQD_RPTR:
	case GPGPU_HQD_ERROR:
		return true;
	case GPGPU_HQD_DOORBELL:
		return !((hqd_get(hqd, field) ^ value) & ~GPGPU_DOORBELL_ENABLE);
	default:
		return false;
	}
}

static uint32_t hqd_read(GPGPUState *s, unsigned int slot, hwaddr field, unsigned int size)
{
	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (field & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return 0;
	}
	return hqd_field_defined(field) ? s->hqd.regs[slot][field / 4] : 0;
}

/*
 * hqd_write() - 按来源写入一个 HQD 字段
 *
 * 1. 访问形式：槽号、宽度、对齐与字段都合法；
 * 2. 来源权限：Host 只在 CP 冷启动前写槽 0，且不能写 ERROR；
 * 3. 激活期间的只读字段；
 * 4. ERROR 只保留首次错误：已有错误时写入不同的非零值被静默忽略；
 * 5. 字段取值，通过后写入。
 *
 * 任何拒绝都保留原值并锁存 INVALID_CMD。
 */
static void hqd_write(GPGPUState *s, unsigned int slot, hwaddr field, uint32_t value,
		      unsigned int size, bool from_cp)
{
	uint32_t *hqd;
	uint32_t old;

	/* 1. 访问形式。 */
	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (field & 3) || !hqd_field_defined(field))
		goto invalid;
	hqd = s->hqd.regs[slot];
	old = hqd_get(hqd, field);

	/* 2. 来源权限。 */
	if (!from_cp && (!gpgpu_cp_cold(s) || slot != GPGPU_HQD_SLOT_KIQ ||
			 field == GPGPU_HQD_ERROR))
		goto invalid;

	/* 3. 激活期间。 */
	if (hqd_get(hqd, GPGPU_HQD_ACTIVE) && !hqd_writable_while_active(hqd, field, value))
		goto invalid;

	/* 4. 首次错误。 */
	if (field == GPGPU_HQD_ERROR && old && value && value != old &&
	    value <= GPGPU_QUEUE_ERROR_SAVE)
		return;

	/* 5. 取值与写入。 */
	if (!hqd_value_valid(s, slot, hqd, field, value))
		goto invalid;
	hqd[field / 4] = value;
	return;
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

/* CP 直接窗口：地址按 GPGPU_HQD_STRIDE 拆成槽号和字段偏移。 */
static uint64_t cp_hqd_read(void *opaque, hwaddr addr, unsigned int size)
{
	return hqd_read(opaque, addr / GPGPU_HQD_STRIDE, addr % GPGPU_HQD_STRIDE, size);
}

static void cp_hqd_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
	hqd_write(opaque, addr / GPGPU_HQD_STRIDE, addr % GPGPU_HQD_STRIDE, value, size, true);
}

const MemoryRegionOps gpgpu_cp_hqd_ops = {
	.read = cp_hqd_read,
	.write = cp_hqd_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	/* 按原始宽度交给检查，非法的 64 位访问不被拆成两次 32 位。 */
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

uint32_t gpgpu_hqd_select_read(GPGPUState *s, hwaddr reg)
{
	return reg == GPGPU_REG_HQD_SELECT ? s->hqd.select : 0;
}

/* 非法槽号保留原选择并锁存 INVALID_CMD；组内其他偏移忽略。 */
void gpgpu_hqd_select_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	if (reg != GPGPU_REG_HQD_SELECT)
		return;
	if (value >= GPGPU_HQD_SLOTS) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	s->hqd.select = value;
}

uint32_t gpgpu_hqd_window_read(GPGPUState *s, hwaddr field)
{
	return hqd_read(s, s->hqd.select, field, 4);
}

void gpgpu_hqd_window_write(GPGPUState *s, hwaddr field, uint32_t value)
{
	hqd_write(s, s->hqd.select, field, value, 4, false);
}

bool gpgpu_hqd_active(GPGPUState *s, unsigned int slot)
{
	return slot < GPGPU_HQD_SLOTS && hqd_get(s->hqd.regs[slot], GPGPU_HQD_ACTIVE);
}

bool gpgpu_hqd_any_active(GPGPUState *s)
{
	for (unsigned int slot = 0; slot < GPGPU_HQD_SLOTS; slot++)
		if (gpgpu_hqd_active(s, slot))
			return true;
	return false;
}

bool gpgpu_hqd_binding_valid(GPGPUState *s, unsigned int slot, unsigned int vmid)
{
	return slot >= GPGPU_HQD_USER_FIRST && gpgpu_hqd_active(s, slot) &&
		hqd_get(s->hqd.regs[slot], GPGPU_HQD_VMID) == vmid;
}

bool gpgpu_hqd_vmid_active(GPGPUState *s, unsigned int vmid)
{
	for (unsigned int slot = GPGPU_HQD_USER_FIRST; slot < GPGPU_HQD_SLOTS; slot++)
		if (gpgpu_hqd_binding_valid(s, slot, vmid))
			return true;
	return false;
}

void gpgpu_hqd_reset(GPGPUState *s)
{
	s->hqd = (GPGPUHQDState) { 0 };
}
