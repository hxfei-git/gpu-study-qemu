// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CP 访存窗口：配置寄存器与窗口访问转发
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/mem/gpuvm.h"
#include "hw/gpgpu/mem/phys.h"
#include "cp_window.h"

/*
 * window_rw() - 把一次窗口内的标量访问转发到目标地址
 *
 * 1. 窗口内偏移与目标地址相加不能溢出；
 * 2. 按访问域转发；未定义的域直接失败。
 */
static MemTxResult window_rw(GPGPUState *s, hwaddr off, uint8_t *buf, unsigned int size,
			     bool is_write)
{
	GPGPUCPWindowState *w = &s->window;
	uint64_t addr;

	/* 1. 目标地址。 */
	if (!gpgpu_range_ok(off, size, GPGPU_CP_WINDOW_SIZE) || w->base > UINT64_MAX - off)
		return MEMTX_ERROR;
	addr = w->base + off;
	if (addr > UINT64_MAX - (size - 1))
		return MEMTX_ERROR;

	/* 2. 访问域。 */
	switch (w->domain) {
	case GPGPU_CP_DOMAIN_MANAGEMENT:
		return gpgpu_phys_rw(s, addr, buf, size, is_write);
	case GPGPU_CP_DOMAIN_USER:
		return gpgpu_user_rw(s, w->vmid, w->slot, addr, buf, size,
				     is_write ? GPGPU_VM_WRITE : GPGPU_VM_READ, w->phase) ?
			MEMTX_OK :
			MEMTX_ERROR;
	default:
		return MEMTX_ERROR;
	}
}

static MemTxResult window_read(void *opaque, hwaddr off, uint64_t *value, unsigned int size,
			       MemTxAttrs attrs)
{
	uint8_t buf[8];
	MemTxResult result = window_rw(opaque, off, buf, size, false);

	*value = result == MEMTX_OK ? ldn_le_p(buf, size) : 0;
	return result;
}

static MemTxResult window_write(void *opaque, hwaddr off, uint64_t value, unsigned int size,
				MemTxAttrs attrs)
{
	uint8_t buf[8];

	stn_le_p(buf, size, value);
	return window_rw(opaque, off, buf, size, true);
}

const MemoryRegionOps gpgpu_cp_window_ops = {
	.read_with_attrs = window_read,
	.write_with_attrs = window_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8 },
	.impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t window_regs_read(void *opaque, hwaddr reg, unsigned int size)
{
	GPGPUCPWindowState *w = &((GPGPUState *)opaque)->window;

	switch (reg) {
	case GPGPU_CP_WINDOW_BASE_LO:
	case GPGPU_CP_WINDOW_BASE_HI:
		return gpgpu_reg64_read(w->base, reg - GPGPU_CP_WINDOW_BASE_LO);
	case GPGPU_CP_WINDOW_DOMAIN:
		return w->domain;
	case GPGPU_CP_WINDOW_VMID:
		return w->vmid;
	case GPGPU_CP_WINDOW_SLOT:
		return w->slot;
	case GPGPU_CP_WINDOW_PHASE:
		return w->phase;
	default:
		return 0;
	}
}

/* 配置只保存不检查；非法组合在下一次窗口访问时失败。BASE_LO 的低 16 位读零。 */
static void window_regs_write(void *opaque, hwaddr reg, uint64_t value, unsigned int size)
{
	GPGPUCPWindowState *w = &((GPGPUState *)opaque)->window;

	switch (reg) {
	case GPGPU_CP_WINDOW_BASE_LO:
		gpgpu_reg64_write(&w->base, 0, value & ~(GPGPU_CP_WINDOW_SIZE - 1));
		break;
	case GPGPU_CP_WINDOW_BASE_HI:
		gpgpu_reg64_write(&w->base, 4, value);
		break;
	case GPGPU_CP_WINDOW_DOMAIN:
		w->domain = value;
		break;
	case GPGPU_CP_WINDOW_VMID:
		w->vmid = value;
		break;
	case GPGPU_CP_WINDOW_SLOT:
		w->slot = value;
		break;
	case GPGPU_CP_WINDOW_PHASE:
		w->phase = value;
		break;
	default:
		break;
	}
}

const MemoryRegionOps gpgpu_cp_window_regs_ops = {
	.read = window_regs_read,
	.write = window_regs_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
	.impl = { .min_access_size = 4, .max_access_size = 4 },
};

void gpgpu_cp_window_reset(GPGPUState *s)
{
	s->window = (GPGPUCPWindowState) { 0 };
}
