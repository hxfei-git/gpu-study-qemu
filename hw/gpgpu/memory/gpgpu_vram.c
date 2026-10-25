// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_vram.h"
#include "hw/gpgpu/gpgpu_internal.h"
#include "qemu/bswap.h"

/*
 * gpgpu_vram_read() - 读取 BAR2 的小端数据
 *
 * 先检查 VRAM 范围，越界时锁存故障并返回零。
 */
static uint64_t gpgpu_vram_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return 0;
	}

	return ldn_le_p(s->vram.ptr + addr, size);
}

/*
 * gpgpu_vram_store() - 向 VRAM 写入一个小端标量
 *
 * 经 AddressSpace 写入，使原生 CP 已翻译的代码同步失效。
 */
void gpgpu_vram_store(GPGPUState *s, hwaddr addr, uint64_t val, unsigned int size)
{
	uint8_t buf[8];

	if (!gpgpu_vram_contains(s, addr, size)) {
		gpgpu_set_error(s, GPGPU_ERR_VRAM_FAULT);
		return;
	}

	stn_le_p(buf, size, val);
	address_space_write(&s->vram.as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
}

/*
 * gpgpu_vram_write() - 处理 BAR2 写回调
 *
 * 转交共用写入口，沿用范围检查和 TCG 失效处理。
 */
static void gpgpu_vram_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	gpgpu_vram_store(opaque, addr, val, size);
}

static const MemoryRegionOps gpgpu_vram_ops = {
	.read = gpgpu_vram_read,
	.write = gpgpu_vram_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl = {
		.min_access_size = 1,
		.max_access_size = 8,
	},
};

bool gpgpu_vram_init(GPGPUState *s, Error **errp)
{
	/* 后备 RAM 供原生 CP 取指，BAR2 仍保留独立的访问检查。 */
	if (!memory_region_init_ram_nomigrate(&s->vram.ram, OBJECT(s), "gpgpu-vram-ram",
					      s->vram_size, errp))
		return false;
	s->vram.ptr = memory_region_get_ram_ptr(&s->vram.ram);
	address_space_init(&s->vram.as, &s->vram.ram, "gpgpu-vram-writes");
	return true;
}

void gpgpu_vram_register_bar(GPGPUState *s)
{
	memory_region_init_io(&s->vram.bar, OBJECT(s), &gpgpu_vram_ops, s, "gpgpu-vram",
			      s->vram_size);
	pci_register_bar(PCI_DEVICE(s), 2,
			 PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
				 PCI_BASE_ADDRESS_MEM_PREFETCH,
			 &s->vram.bar);
}

void gpgpu_vram_cleanup(GPGPUState *s)
{
	address_space_destroy(&s->vram.as);
}

void gpgpu_vram_reset(GPGPUState *s)
{
	/* 经地址空间清零，保留原生 CP 对同一物理地址的代码失效处理。 */
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

bool gpgpu_vram_word_valid(GPGPUState *s, uint64_t addr)
{
	return !(addr & 3) && gpgpu_vram_contains(s, addr, sizeof(uint32_t));
}

bool gpgpu_vram_read_word(GPGPUState *s, uint64_t addr, uint32_t *value)
{
	if (!gpgpu_vram_word_valid(s, addr))
		return false;
	*value = ldl_le_p(s->vram.ptr + addr);
	return true;
}

void gpgpu_vram_read_bytes(GPGPUState *s, uint64_t addr, void *data, size_t length)
{
	memcpy(data, s->vram.ptr + addr, length);
}
