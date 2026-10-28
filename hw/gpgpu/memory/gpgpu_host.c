// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_host.h"
#include "gpgpu_vram.h"
#include "hw/gpgpu/gpgpu_internal.h"
#include "hw/remote/vfio-user-obj.h"
#include "hw/pci/pci_device.h"
#include "qemu/bitops.h"
#include "software/gpgpu/include/gpgpu_queue.h"

bool gpgpu_phys_contains(GPGPUState *s, uint64_t address, uint64_t length)
{
	GPGPUPhysicalState *p = &s->physical;

	if (!length || address >= GPGPU_VM_ADDRESS_LIMIT ||
	    length > GPGPU_VM_ADDRESS_LIMIT - address)
		return false;
	if (address < s->vram_size)
		return length <= s->vram_size - address;
	return p->enabled && address >= p->gpu_base && address - p->gpu_base < p->size &&
		length <= p->size - (address - p->gpu_base);
}

MemTxResult gpgpu_phys_access(GPGPUState *s, uint64_t address, void *data, size_t length,
			      bool is_write)
{
	GPGPUPhysicalState *p = &s->physical;
	MemTxResult result;
	uint64_t iova;

	if (!gpgpu_phys_contains(s, address, length))
		return MEMTX_ERROR;
	p->accesses++;
	if (address < s->vram_size) {
		result = address_space_rw(&s->vram.as, address, MEMTXATTRS_UNSPECIFIED, data,
					  length, is_write);
	} else {
		uint64_t offset = address - p->gpu_base;

		/* 用已验证的窗口配置换算，绝不将 GPU 物理地址传给 PCI DMA。 */
		if (p->host_base > UINT64_MAX - offset ||
		    p->host_base + offset > UINT64_MAX - (length - 1)) {
			result = MEMTX_ERROR;
		} else {
			iova = p->host_base + offset;
			result = vfu_object_dma_rw(PCI_DEVICE(s), iova, data, length, is_write);
		}
	}
	p->accesses--;
	return result;
}

static bool outbound_busy(GPGPUState *s)
{
	if (s->physical.accesses || !s->cp.reset_hold ||
	    s->dispatch.status == GPGPU_DISPATCH_BUSY || s->dma.status == GPGPU_DMA_BUSY ||
	    (s->ih.status & GPGPU_IH_BUSY))
		return true;
	for (unsigned int slot = 0; slot < GPGPU_QUEUE_SLOTS; slot++)
		if (gpgpu_hqd_active(s, slot))
			return true;
	return false;
}

uint32_t gpgpu_outbound_read(GPGPUState *s, hwaddr addr)
{
	GPGPUPhysicalState *p = &s->physical;

	switch (addr) {
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

void gpgpu_outbound_write(GPGPUState *s, hwaddr addr, uint32_t value)
{
	GPGPUPhysicalState *p = &s->physical;

	if (outbound_busy(s))
		goto invalid;
	if (addr == GPGPU_REG_OUTBOUND_CONTROL) {
		if (value & ~GPGPU_OUTBOUND_ENABLE)
			goto invalid;
		if (value &&
		    (p->gpu_base != s->vram_size || p->host_base ||
		     p->size != GPGPU_VM_ADDRESS_LIMIT - s->vram_size))
			goto invalid;
		p->enabled = value;
		p->status = GPGPU_OUTBOUND_ACK;
		return;
	}
	if (p->enabled)
		goto invalid;
	switch (addr) {
	case GPGPU_REG_OUTBOUND_GPU_BASE:
		p->gpu_base = value;
		break;
	case GPGPU_REG_OUTBOUND_SIZE_LO:
		p->size = deposit64(p->size, 0, 32, value);
		break;
	case GPGPU_REG_OUTBOUND_SIZE_HI:
		p->size = deposit64(p->size, 32, 32, value);
		break;
	case GPGPU_REG_OUTBOUND_HOST_LO:
		p->host_base = deposit64(p->host_base, 0, 32, value);
		break;
	case GPGPU_REG_OUTBOUND_HOST_HI:
		p->host_base = deposit64(p->host_base, 32, 32, value);
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
	assert(!s->physical.accesses);
	s->physical = (GPGPUPhysicalState) {
		.gpu_base = s->vram_size,
		.size = GPGPU_VM_ADDRESS_LIMIT - s->vram_size,
	};
}
