// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_hqd.h"
#include "gpgpu_cp.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "hw/gpgpu/gpgpu_internal.h"

/*
 * hqd_field() - 判断槽内偏移是否为已实现的 HQD 字段
 *
 * 用显式字段集合排除保留寄存器。
 */
static bool hqd_field(hwaddr addr)
{
	switch (addr) {
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

/*
 * hqd_address() - 拼接 HQD 内相邻的地址寄存器
 *
 * low 指向低 32 位偏移，返回完整的 64 位 GPU 地址。
 */
static uint64_t hqd_address(uint32_t *hqd, unsigned int low)
{
	return hqd[low / 4] | (uint64_t)hqd[low / 4 + 1] << 32;
}

/*
 * hqd_range() - 检查 HQD 地址对齐与范围溢出
 *
 * 调用者提供非零长度及 2 的幂对齐值，用减法检查完整范围是否落在 32 位地址空间。
 */
static bool hqd_range(uint64_t addr, uint64_t size, unsigned int alignment)
{
	return !(addr & (alignment - 1)) && addr < GPGPU_VM_ADDRESS_LIMIT &&
		size <= GPGPU_VM_ADDRESS_LIMIT - addr;
}

/*
 * hqd_complete() - 检查一个 HQD 是否具备激活条件
 *
 * 校验 Ring 容量以及 MQD、Ring 和进度地址的对齐与边界。
 */
static bool hqd_complete(uint32_t *hqd)
{
	uint32_t size = hqd[GPGPU_HQD_CONTROL / 4];

	return size >= GPGPU_QUEUE_RING_MIN && size <= GPGPU_QUEUE_RING_MAX &&
		is_power_of_2(size) && hqd_range(hqd_address(hqd, GPGPU_HQD_MQD_LO), 64, 8) &&
		hqd_range(hqd_address(hqd, GPGPU_HQD_RING_LO), size, 64) &&
		hqd_range(hqd_address(hqd, GPGPU_HQD_RPTR_ADDR_LO), 4, 8) &&
		hqd_range(hqd_address(hqd, GPGPU_HQD_WPTR_ADDR_LO), 4, 8);
}

/*
 * gpgpu_hqd_read() - 读取指定 HQD 槽的寄存器
 *
 * 先校验槽号和 32 位访问宽度，保留偏移返回零。
 */
uint32_t gpgpu_hqd_read(GPGPUState *s, unsigned int slot, hwaddr addr, unsigned int size)
{
	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (addr & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return 0;
	}
	return hqd_field(addr) ? s->hqd.slots[slot][addr / 4] : 0;
}

/*
 * gpgpu_hqd_write() - 按访问来源更新 HQD 字段
 *
 * Host 只在冷启动配置 KIQ；激活前校验完整配置，非法写入保持原值。
 */
void gpgpu_hqd_write(GPGPUState *s, unsigned int slot, hwaddr addr, uint32_t val, unsigned int size,
		     bool from_cp)
{
	uint32_t *hqd, old;
	bool valid = true;

	if (slot >= GPGPU_HQD_SLOTS || size != 4 || (addr & 3) || !hqd_field(addr))
		goto invalid;
	hqd = s->hqd.slots[slot];
	old = hqd[addr / 4];
	if (!from_cp && (!gpgpu_cp_host_configurable(s) || slot != 0 || addr == GPGPU_HQD_ERROR))
		goto invalid;
	if (hqd[GPGPU_HQD_ACTIVE / 4] && addr != GPGPU_HQD_ACTIVE && addr != GPGPU_HQD_RPTR &&
	    addr != GPGPU_HQD_ERROR &&
	    !(addr == GPGPU_HQD_DOORBELL && !((old ^ val) & ~GPGPU_DOORBELL_ENABLE))) {
		goto invalid;
	}
	switch (addr) {
	case GPGPU_HQD_MQD_LO:
	case GPGPU_HQD_RPTR_ADDR_LO:
	case GPGPU_HQD_WPTR_ADDR_LO:
		valid = !(val & 7);
		break;
	case GPGPU_HQD_RING_LO:
		valid = !(val & 63);
		break;
	case GPGPU_HQD_ACTIVE:
		valid = val <= 1 && (!val || hqd_complete(hqd));
		if (valid && val && slot >= 2) {
			unsigned int vmid = hqd[GPGPU_HQD_VMID / 4];

			valid = vmid >= 1 && vmid <= GPGPU_VM_USERS &&
				(!s->gpuvm || gpgpu_vm_enabled(s, vmid)) &&
				hqd_address(hqd, GPGPU_HQD_RING_LO) <=
					GPGPU_VM_ADDRESS_LIMIT - hqd[GPGPU_HQD_CONTROL / 4] &&
				hqd_address(hqd, GPGPU_HQD_RPTR_ADDR_LO) < GPGPU_VM_ADDRESS_LIMIT &&
				hqd_address(hqd, GPGPU_HQD_WPTR_ADDR_LO) < GPGPU_VM_ADDRESS_LIMIT;
		}
		break;
	case GPGPU_HQD_VMID:
		valid = slot >= 2 ? val <= GPGPU_VM_USERS : val == 0;
		break;
	case GPGPU_HQD_AQL:
		valid = val <= 1;
		break;
	case GPGPU_HQD_CONTROL:
		valid = val >= GPGPU_QUEUE_RING_MIN && val <= GPGPU_QUEUE_RING_MAX &&
			is_power_of_2(val);
		break;
	case GPGPU_HQD_DOORBELL:
		valid = !(val & ~(GPGPU_DOORBELL_ENABLE | GPGPU_DOORBELL_OFFSET_MASK)) &&
			!(val & 7) &&
			(val & GPGPU_DOORBELL_OFFSET_MASK) <= GPGPU_DOORBELL_BAR_SIZE - 8;
		break;
	case GPGPU_HQD_ERROR:
		valid = val <= GPGPU_QUEUE_ERROR_SAVE;
		/* 保留首次错误，停用后才可清理。 */
		if (valid && old && val && val != old)
			return;
		valid &= !(old && !val && hqd[GPGPU_HQD_ACTIVE / 4]);
		break;
	default:
		break;
	}
	if (!valid)
		goto invalid;
	hqd[addr / 4] = val;
	if (addr == GPGPU_HQD_ACTIVE && val)
		s->hqd.doorbell_pending[slot] = true;
	return;
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

/*
 * cp_hqd_read() - 从 CP 本地窗口读取 HQD
 *
 * 用窗口步长分解槽号和槽内偏移，再调用共用访问检查。
 */
static uint64_t cp_hqd_read(void *opaque, hwaddr addr, unsigned int size)
{
	return gpgpu_hqd_read(opaque, addr / GPGPU_HQD_STRIDE, addr % GPGPU_HQD_STRIDE, size);
}

/*
 * cp_hqd_write() - 从 CP 本地窗口写入 HQD
 *
 * 分解直接窗口地址并标记 CP 来源，沿用共用字段校验。
 */
static void cp_hqd_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	gpgpu_hqd_write(opaque, addr / GPGPU_HQD_STRIDE, addr % GPGPU_HQD_STRIDE, val, size, true);
}

const MemoryRegionOps gpgpu_cp_hqd_ops = {
	.read = cp_hqd_read,
	.write = cp_hqd_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	/* 保留原始宽度，禁止拆开非法的 64 位访问。 */
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

unsigned int gpgpu_hqd_selected(GPGPUState *s)
{
	return s->hqd.select;
}

void gpgpu_hqd_select(GPGPUState *s, uint32_t slot)
{
	if (slot < GPGPU_HQD_SLOTS)
		s->hqd.select = slot;
	else
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

bool gpgpu_hqd_active(GPGPUState *s, unsigned int slot)
{
	return slot < GPGPU_HQD_SLOTS && s->hqd.slots[slot][GPGPU_HQD_ACTIVE / 4];
}

bool gpgpu_hqd_binding_valid(GPGPUState *s, unsigned int slot, unsigned int vmid)
{
	return slot >= 2 && gpgpu_hqd_active(s, slot) &&
		s->hqd.slots[slot][GPGPU_HQD_VMID / 4] == vmid;
}

bool gpgpu_hqd_vmid_active(GPGPUState *s, unsigned int vmid)
{
	for (unsigned int slot = 2; slot < GPGPU_HQD_SLOTS; slot++)
		if (gpgpu_hqd_binding_valid(s, slot, vmid))
			return true;
	return false;
}

void gpgpu_hqd_notify(GPGPUState *s, hwaddr offset)
{
	for (unsigned int slot = 0; slot < GPGPU_HQD_SLOTS; slot++) {
		uint32_t control = s->hqd.slots[slot][GPGPU_HQD_DOORBELL / 4];

		if (gpgpu_hqd_active(s, slot) && (control & GPGPU_DOORBELL_ENABLE) &&
		    (control & GPGPU_DOORBELL_OFFSET_MASK) == offset) {
			/* 固件持续轮询；通知不修改共享 W/R，也不启动计算。 */
			s->hqd.doorbell_pending[slot] = true;
		}
	}
}

void gpgpu_hqd_reset(GPGPUState *s)
{
	memset(&s->hqd, 0, sizeof(s->hqd));
}
