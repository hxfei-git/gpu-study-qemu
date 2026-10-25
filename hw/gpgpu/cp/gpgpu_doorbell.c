// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_doorbell.h"
#include "gpgpu_hqd.h"
#include "hw/gpgpu/gpgpu_internal.h"

/*
 * gpgpu_doorbell_read() - 处理 Doorbell 读访问
 *
 * Doorbell 不保存可读的发布值，因此固定返回零。
 */
static uint64_t gpgpu_doorbell_read(void *opaque, hwaddr addr, unsigned int size)
{
	return 0;
}

/*
 * gpgpu_doorbell_write() - 记录用户或管理队列的 Doorbell 提示
 *
 * 只接受对齐的 8 字节写入和零扩展的编号，按已使能槽的偏移匹配通知。
 */
static void gpgpu_doorbell_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 8 || (addr & 7) || val >> 32) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	gpgpu_hqd_notify(s, addr);
}

static const MemoryRegionOps gpgpu_doorbell_ops = {
	.read = gpgpu_doorbell_read,
	.write = gpgpu_doorbell_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void gpgpu_doorbell_init(GPGPUState *s)
{
	memory_region_init_io(&s->doorbell.mmio, OBJECT(s), &gpgpu_doorbell_ops, s,
			      "gpgpu-doorbell", GPGPU_DOORBELL_BAR_SIZE);
	pci_register_bar(PCI_DEVICE(s), 4, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->doorbell.mmio);
}
