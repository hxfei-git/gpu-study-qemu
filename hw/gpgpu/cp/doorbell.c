// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Doorbell：BAR4 写入检查
 */
#include "qemu/osdep.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "doorbell.h"

/* Doorbell 不保存可读回的值。 */
static uint64_t doorbell_read(void *opaque, hwaddr addr, unsigned int size)
{
	return 0;
}

/* 只接受 8 字节对齐、高 32 位为 0 的 64 位写入；其余锁存 INVALID_CMD。 */
static void doorbell_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 8 || (addr & 7) || value >> 32) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	/* 仅记录提醒的到达时刻；CP 可能已通过轮询开始处理这个包。 */
	if (gpgpu_trace_enabled(s))
		fprintf(stderr, "GPGPU_DOORBELL {\"host_ns\":%" PRIu64
			",\"offset\":%" PRIu64 ",\"packet\":%u}\n",
			(uint64_t)g_get_monotonic_time() * 1000, (uint64_t)addr, (uint32_t)value);
}

static const MemoryRegionOps doorbell_ops = {
	.read = doorbell_read,
	.write = doorbell_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void gpgpu_doorbell_init(GPGPUState *s)
{
	memory_region_init_io(&s->doorbell.bar, OBJECT(s), &doorbell_ops, s, "gpgpu-doorbell",
			      GPGPU_DOORBELL_BAR_SIZE);
	pci_register_bar(PCI_DEVICE(s), 4, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->doorbell.bar);
}
