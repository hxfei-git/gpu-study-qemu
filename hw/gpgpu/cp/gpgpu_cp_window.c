// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_cp_window.h"
#include "hw/gpgpu/memory/gpgpu_host.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "hw/gpgpu/gpgpu_internal.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"

/* 先检查窗口和加法边界，再按当前管理域或用户域完成一次标量访问。 */
static MemTxResult gpgpu_cp_host_access(GPGPUState *s, hwaddr addr, uint8_t *buf, unsigned int size,
					bool is_write)
{
	uint64_t iova;

	if (addr >= GPGPU_CP_HOST_WINDOW_SIZE || size > GPGPU_CP_HOST_WINDOW_SIZE - addr ||
	    s->cp.window.host_iova_base > UINT64_MAX - addr) {
		return MEMTX_ERROR;
	}
	iova = s->cp.window.host_iova_base + addr;
	if (iova > UINT64_MAX - (size - 1))
		return MEMTX_ERROR;

	if (s->cp.window.vm_domain == GPGPU_VM_DOMAIN_USER) {
		return gpgpu_user_access(s, s->cp.window.vm_id, s->cp.window.vm_slot, iova, buf, size,
				       is_write ? GPGPU_VM_WRITE : GPGPU_VM_READ,
				       s->cp.window.vm_phase) ?
			MEMTX_OK :
			MEMTX_ERROR;
	}
	if (s->cp.window.vm_domain != GPGPU_VM_DOMAIN_MANAGEMENT)
		return MEMTX_ERROR;
	/* 直接访存，不使用 Host DMA 描述符、状态或完成中断。 */
	return gpgpu_phys_access(s, iova, buf, size, is_write);
}

static MemTxResult gpgpu_cp_host_read(void *opaque, hwaddr addr, uint64_t *val, unsigned int size,
				      MemTxAttrs attrs)
{
	uint8_t buf[8];
	MemTxResult result;

	*val = 0;
	result = gpgpu_cp_host_access(opaque, addr, buf, size, false);
	if (result == MEMTX_OK)
		*val = ldn_le_p(buf, size);
	return result;
}

static MemTxResult gpgpu_cp_host_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size,
				       MemTxAttrs attrs)
{
	uint8_t buf[8];

	stn_le_p(buf, size, val);
	return gpgpu_cp_host_access(opaque, addr, buf, size, true);
}

const MemoryRegionOps gpgpu_cp_host_ops = {
	.read_with_attrs = gpgpu_cp_host_read,
	.write_with_attrs = gpgpu_cp_host_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 8,
	},
	.impl = {
		.min_access_size = 1,
		.max_access_size = 8,
	},
};

static uint64_t gpgpu_cp_host_regs_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	switch (addr) {
	case 0:
		return (uint32_t)s->cp.window.host_iova_base;
	case 4:
		return s->cp.window.host_iova_base >> 32;
	case 8:
		return s->cp.window.vm_domain;
	case 12:
		return s->cp.window.vm_id;
	case 16:
		return s->cp.window.vm_slot;
	case 20:
		return s->cp.window.vm_phase;
	default:
		return 0;
	}
}

static void gpgpu_cp_host_regs_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	GPGPUState *s = opaque;

	if (addr >= 8) {
		switch (addr) {
		case 8:
			s->cp.window.vm_domain = val;
			break;
		case 12:
			s->cp.window.vm_id = val;
			break;
		case 16:
			s->cp.window.vm_slot = val;
			break;
		case 20:
			s->cp.window.vm_phase = val;
			break;
		}
		return;
	}
	if (addr == 0) {
		/* 低 16 位读零；写完高低位后再访存。 */
		val &= ~(GPGPU_CP_HOST_WINDOW_SIZE - 1);
		s->cp.window.host_iova_base = deposit64(s->cp.window.host_iova_base, 0, 32, val);
	} else {
		s->cp.window.host_iova_base = deposit64(s->cp.window.host_iova_base, 32, 32, val);
	}
}

const MemoryRegionOps gpgpu_cp_host_regs_ops = {
	.read = gpgpu_cp_host_regs_read,
	.write = gpgpu_cp_host_regs_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
	.impl = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

void gpgpu_cp_window_reset(GPGPUState *s)
{
	memset(&s->cp.window, 0, sizeof(s->cp.window));
}
