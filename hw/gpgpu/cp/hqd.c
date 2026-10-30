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

/* 槽内已定义的 15 个字段；其余偏移读零，写入锁存 INVALID_CMD。 */
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

/* 按字段字节偏移读取槽内一个 32 位寄存器。 */
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

/* Ring 容量：GPGPU_QUEUE_RING_MIN～MAX 之间的 2 的幂字节数。 */
static bool ring_size_valid(uint32_t size)
{
	return size >= GPGPU_QUEUE_RING_MIN && size <= GPGPU_QUEUE_RING_MAX && is_power_of_2(size);
}

/*
 * hqd_ready_to_activate() - 写 ACTIVE=1 时检查整套配置
 *
 * 各字段写入时只检查自身取值，彼此之间的关系（如 Ring 地址加容量
 * 是否越界）要等全部写完才能判断，所以在激活这一刻统一检查。
 * 激活后这些字段只读，CP 取包期间配置不会再变：
 *
 * 所有槽：Ring 容量合法；MQD 按 8 字节、Ring 按 64 字节、R/W 进度地址
 * 按 8 字节对齐，且整段范围不越出 32 位 GPU 地址空间。
 * 用户槽另需：VMID 为 1 或 2；gpuvm=on 时该 VM 的上下文已启用，
 * 否则 CP 无法翻译用户地址。KIQ、HIQ 使用 VMID 0，不经翻译。
 */
static bool hqd_ready_to_activate(GPGPUState *s, unsigned int slot, const uint32_t *hqd)
{
	uint32_t size = hqd_get(hqd, GPGPU_HQD_CONTROL);
	uint32_t vmid = hqd_get(hqd, GPGPU_HQD_VMID);

	/* MQD 64 字节，Ring 为 size 字节，R、W 进度各 4 字节。 */
	if (!ring_size_valid(size) || !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_MQD_LO), 64, 8) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_RING_LO), size, 64) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_RPTR_ADDR_LO), 4, 8) ||
	    !hqd_range_ok(hqd_get64(hqd, GPGPU_HQD_WPTR_ADDR_LO), 4, 8))
		return false;

	/* KIQ、HIQ 到此为止；用户槽再检查 VMID 与上下文。 */
	if (slot < GPGPU_HQD_USER_FIRST)
		return true;
	return vmid >= 1 && vmid <= GPGPU_VM_USERS &&
		(!gpgpu_gpuvm_enabled(s) || gpgpu_vm_context_enabled(s, vmid));
}

/*
 * 写入单个字段时检查它自己的取值：地址低位按对齐要求为 0，VMID 范围
 * 按槽类型区分，Doorbell 偏移按 8 字节对齐且落在 BAR4 内。
 * 写 ACTIVE=1 时再检查整套配置，见 hqd_ready_to_activate()。
 */
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
		/* 激活期间不能把已记录的错误清零。 */
		if (!value && hqd_get(hqd, GPGPU_HQD_ERROR) && hqd_get(hqd, GPGPU_HQD_ACTIVE))
			return false;
		return value <= GPGPU_QUEUE_ERROR_SAVE;
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

/* 槽号、宽度与对齐的检查同 hqd_write()；未定义字段读零。 */
static uint32_t hqd_read(GPGPUState *s, unsigned int slot, hwaddr field, unsigned int size)
{
	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (field & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return 0;
	}
	return hqd_field_defined(field) ? s->hqd.regs[slot][field / 4] : 0;
}

/*
 * hqd_write() - Host 选槽窗口或 CP 直接窗口写入一个 HQD 字段
 *
 * 两条路径共用这一个函数，from_cp 区分来源。检查从访问形式到字段
 * 取值逐层收紧，任一层不通过就停止：
 *
 * 1. 访问形式：槽号存在，宽度为 4 字节且对齐，字段已定义；
 * 2. 来源权限：Host 只负责在 CP 冷启动前配置 KIQ（槽 0），之后的
 *    HIQ 和用户队列都由 CP 装载；ERROR 只由 CP 写入，Host 只读；
 * 3. 槽已激活时，只允许改 ACTIVE、RPTR、ERROR 和 Doorbell 使能位，
 *    防止 CP 取包期间 Ring 地址等配置被改掉；
 * 4. ERROR 只保留首次错误：已有错误时写入另一个合法的非零值被
 *    静默忽略，不锁存 INVALID_CMD，固件可以照常上报后续错误；
 * 5. 检查字段自身的取值，通过后写入。
 *
 * 第 1、2、3、5 步拒绝时保留原值并锁存 INVALID_CMD。
 */
static void hqd_write(GPGPUState *s, unsigned int slot, hwaddr field, uint32_t value,
		      unsigned int size, bool from_cp)
{
	uint32_t *hqd;
	uint32_t old;

	/* 1. 槽号、宽度、对齐与字段名。 */
	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (field & 3) || !hqd_field_defined(field))
		goto invalid;
	hqd = s->hqd.regs[slot];
	old = hqd_get(hqd, field);

	/* 2. Host 只能在 CP 冷启动前写 KIQ 槽，且不能写 ERROR。 */
	if (!from_cp && (!gpgpu_cp_cold(s) || slot != GPGPU_HQD_SLOT_KIQ ||
			 field == GPGPU_HQD_ERROR))
		goto invalid;

	/* 3. 激活期间配置字段只读。 */
	if (hqd_get(hqd, GPGPU_HQD_ACTIVE) && !hqd_writable_while_active(hqd, field, value))
		goto invalid;

	/* 4. 已有首次错误，新错误不覆盖，直接返回。 */
	if (field == GPGPU_HQD_ERROR && old && value && value != old &&
	    value <= GPGPU_QUEUE_ERROR_SAVE)
		return;

	/* 5. 取值合法才写入；写 ACTIVE=1 时在这里做整套检查。 */
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
