// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VRAM：后备存储、BAR2 与设备内部访问
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "vram.h"

/* BAR2 读：越界锁存 VRAM_FAULT 并返回零。 */
static uint64_t vram_bar_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return 0;
	}
	return ldn_le_p(s->vram.ptr + addr, size);
}

/*
 * BAR2 写：越界锁存 VRAM_FAULT。合法写入经内部地址空间，
 * 使原生 CP 已翻译的代码失效。
 */
static void vram_bar_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	GPGPUState *s = opaque;
	uint8_t buf[8];

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return;
	}
	stn_le_p(buf, size, val);
	gpgpu_vram_rw(s, addr, buf, size, true);
}

static const MemoryRegionOps vram_bar_ops = {
	.read = vram_bar_read,
	.write = vram_bar_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl = {
		.min_access_size = 1,
		.max_access_size = 8,
	},
};

bool gpgpu_vram_init(GPGPUState *s, Error **errp)
{
	GPGPUVRAMState *v = &s->vram;

	if (!memory_region_init_ram_nomigrate(&v->ram, OBJECT(s), "gpgpu-vram-ram", s->vram_size,
					      errp))
		return false;
	v->ptr = memory_region_get_ram_ptr(&v->ram);
	address_space_init(&v->as, &v->ram, "gpgpu-vram");

	memory_region_init_io(&v->bar, OBJECT(s), &vram_bar_ops, s, "gpgpu-vram", s->vram_size);
	pci_register_bar(PCI_DEVICE(s), 2,
			 PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
				 PCI_BASE_ADDRESS_MEM_PREFETCH,
			 &v->bar);
	return true;
}

void gpgpu_vram_cleanup(GPGPUState *s)
{
	address_space_destroy(&s->vram.as);
}

void gpgpu_vram_clear(GPGPUState *s)
{
	address_space_set(&s->vram.as, 0, 0, s->vram_size, MEMTXATTRS_UNSPECIFIED);
}

MemoryRegion *gpgpu_vram_region(GPGPUState *s)
{
	return &s->vram.ram;
}

uint64_t gpgpu_vram_size(GPGPUState *s)
{
	return s->vram_size;
}

bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr, uint64_t size)
{
	return addr < s->vram_size && size <= s->vram_size - addr;
}

void *gpgpu_vram_map(GPGPUState *s, uint64_t addr, uint64_t len, bool is_write)
{
	hwaddr mapped = len;
	void *ptr = address_space_map(&s->vram.as, addr, &mapped, is_write, MEMTXATTRS_UNSPECIFIED);

	assert(ptr && mapped == len);
	return ptr;
}

void gpgpu_vram_unmap(GPGPUState *s, void *ptr, uint64_t len, bool is_write)
{
	address_space_unmap(&s->vram.as, ptr, len, is_write, is_write ? len : 0);
}

MemTxResult gpgpu_vram_rw(GPGPUState *s, uint64_t addr, void *buf, uint64_t len, bool is_write)
{
	return address_space_rw(&s->vram.as, addr, MEMTXATTRS_UNSPECIFIED, buf, len, is_write);
}
