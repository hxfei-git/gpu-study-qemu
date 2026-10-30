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
 * window_rw() - 把 CP 对窗口的一次 1/2/4/8 字节访问转发到目标地址
 *
 * off 是 CP 地址相对窗口起点的偏移。目标地址 = 配置的窗口基址 + off，
 * 再按配置的访问域决定是否经过 GPUVM：
 *
 * 1. 检查访问不越出 64 KiB 窗口，基址加偏移、再加访问长度都不溢出
 *    64 位，否则返回 MEMTX_ERROR；
 * 2. 管理域把目标当 GPU 物理地址访问（MQD、Ring、IH 等驱动对象）；
 *    用户域按配置的 VMID 访问用户地址，gpuvm=on 时经页表翻译，失败时
 *    故障记到配置的 HQD 槽和阶段。未定义的域直接失败。
 *
 * 返回 MEMTX_ERROR 时 CP 收到 access fault，由固件陷阱处理恢复。
 */
static MemTxResult window_rw(GPGPUState *s, hwaddr off, uint8_t *buf, unsigned int size,
			     bool is_write)
{
	GPGPUCPWindowState *w = &s->window;
	uint64_t addr;

	/* 1. 窗口内范围与目标地址的 64 位溢出检查。 */
	if (!gpgpu_range_ok(off, size, GPGPU_CP_WINDOW_SIZE) || w->base > UINT64_MAX - off)
		return MEMTX_ERROR;
	addr = w->base + off;
	if (addr > UINT64_MAX - (size - 1))
		return MEMTX_ERROR;

	/* 2. 管理域走物理地址，用户域走 gpgpu_user_rw()。 */
	switch (w->domain) {
	case GPGPU_CP_DOMAIN_MANAGEMENT:
		return gpgpu_phys_rw(s, addr, buf, size, is_write);
	case GPGPU_CP_DOMAIN_USER:
		if (!gpgpu_user_rw(s, w->vmid, w->slot, addr, buf, size,
				   is_write ? GPGPU_VM_WRITE : GPGPU_VM_READ, w->phase))
			return MEMTX_ERROR;
		return MEMTX_OK;
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

/*
 * 配置只保存不检查，非法组合在下一次窗口访问时失败。
 * 窗口按 64 KiB 对齐，BASE_LO 写入时清掉低 16 位，读回为零。
 */
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
