// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPU 物理地址路由与 Outbound 窗口
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/remote/vfio-user-obj.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "phys.h"
#include "vram.h"

bool gpgpu_phys_contains(GPGPUState *s, uint64_t addr, uint64_t len)
{
	GPGPUPhysState *p = &s->phys;

	if (!len || !gpgpu_range_ok(addr, len, GPGPU_VM_ADDRESS_LIMIT))
		return false;
	if (addr < s->vram_size)
		return gpgpu_vram_contains(s, addr, len);
	return p->enabled && addr >= p->gpu_base &&
		gpgpu_range_ok(addr - p->gpu_base, len, p->size);
}

/*
 * 1. 检查路由；2. 计入在途访问；3. VRAM 走内部地址空间，Outbound 换算成
 * Host IOVA 后经 vfio-user DMA（可能暂时释放主锁）；4. 撤销在途计数。
 *
 * 使能时 host_base 为 0 且窗口不超过 4 GiB，换算后的 IOVA 不会溢出。
 */
MemTxResult gpgpu_phys_rw(GPGPUState *s, uint64_t addr, void *buf, size_t len, bool is_write)
{
	GPGPUPhysState *p = &s->phys;
	MemTxResult result;

	/* 1. 路由。 */
	if (!gpgpu_phys_contains(s, addr, len))
		return MEMTX_ERROR;

	/* 2. 在途计数。 */
	p->accesses++;

	/* 3. 访问。 */
	if (addr < s->vram_size) {
		result = gpgpu_vram_rw(s, addr, buf, len, is_write);
	} else {
		uint64_t iova = p->host_base + (addr - p->gpu_base);

		result = vfu_object_dma_rw(PCI_DEVICE(s), iova, buf, len, is_write);
	}

	/* 4. 撤销计数。 */
	p->accesses--;
	return result;
}

bool gpgpu_phys_busy(GPGPUState *s)
{
	return s->phys.accesses;
}

uint32_t gpgpu_outbound_read(GPGPUState *s, hwaddr reg)
{
	GPGPUPhysState *p = &s->phys;

	switch (reg) {
	case GPGPU_REG_OUTBOUND_GPU_BASE:
		return p->gpu_base;
	case GPGPU_REG_OUTBOUND_SIZE_LO:
		return p->size;
	case GPGPU_REG_OUTBOUND_SIZE_HI:
		return p->size >> 32;
	case GPGPU_REG_OUTBOUND_HOST_LO:
		return p->host_base;
	case GPGPU_REG_OUTBOUND_HOST_HI:
		return p->host_base >> 32;
	case GPGPU_REG_OUTBOUND_CONTROL:
		return p->enabled ? GPGPU_OUTBOUND_ENABLE : 0;
	case GPGPU_REG_OUTBOUND_STATUS:
		return p->status | (p->accesses ? GPGPU_OUTBOUND_BUSY : 0);
	default:
		return 0;
	}
}

/* 使能只接受唯一的固定布局：窗口紧接 VRAM，覆盖到 4 GiB，映射到 IOVA 0。 */
static bool outbound_layout_valid(GPGPUState *s)
{
	GPGPUPhysState *p = &s->phys;

	return p->gpu_base == s->vram_size && !p->host_base &&
		p->size == GPGPU_VM_ADDRESS_LIMIT - s->vram_size;
}

/*
 * 1. 所有设备引擎停止、且无在途访问时才接受配置；2. CONTROL 使能前核对布局；
 * 3. 窗口使能期间其余字段只读。任何拒绝都置 ERROR 并锁存 INVALID_CMD。
 */
void gpgpu_outbound_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUPhysState *p = &s->phys;

	/* 1. 停机检查。 */
	if (p->accesses || !gpgpu_engines_idle(s))
		goto invalid;

	/* 2. 使能控制。 */
	if (reg == GPGPU_REG_OUTBOUND_CONTROL) {
		if (value & ~GPGPU_OUTBOUND_ENABLE)
			goto invalid;
		if (value && !outbound_layout_valid(s))
			goto invalid;
		p->enabled = value;
		p->status = GPGPU_OUTBOUND_ACK;
		return;
	}

	/* 3. 布局字段。 */
	if (p->enabled)
		goto invalid;
	switch (reg) {
	case GPGPU_REG_OUTBOUND_GPU_BASE:
		p->gpu_base = value;
		break;
	case GPGPU_REG_OUTBOUND_SIZE_LO:
	case GPGPU_REG_OUTBOUND_SIZE_HI:
		gpgpu_reg64_write(&p->size, reg - GPGPU_REG_OUTBOUND_SIZE_LO, value);
		break;
	case GPGPU_REG_OUTBOUND_HOST_LO:
	case GPGPU_REG_OUTBOUND_HOST_HI:
		gpgpu_reg64_write(&p->host_base, reg - GPGPU_REG_OUTBOUND_HOST_LO, value);
		break;
	default:
		goto invalid;
	}
	p->status = 0;
	return;
invalid:
	p->status = GPGPU_OUTBOUND_ERROR;
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

void gpgpu_outbound_reset(GPGPUState *s)
{
	assert(!s->phys.accesses);
	s->phys = (GPGPUPhysState) {
		.gpu_base = s->vram_size,
		.size = GPGPU_VM_ADDRESS_LIMIT - s->vram_size,
	};
}
