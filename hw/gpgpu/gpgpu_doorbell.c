/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "gpgpu.h"

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
	for (unsigned int slot = 0; slot < GPGPU_QUEUE_SLOTS; slot++) {
		uint32_t control = s->hqd[slot][GPGPU_HQD_DOORBELL / 4];

		if (s->hqd[slot][GPGPU_HQD_ACTIVE / 4] && (control & GPGPU_DOORBELL_ENABLE) &&
		    (control & GPGPU_DOORBELL_OFFSET_MASK) == addr) {
			/* 固件持续轮询；通知不修改共享 W/R。 */
			s->doorbell_pending[slot] = true;
		}
	}
}

const MemoryRegionOps gpgpu_doorbell_ops = {
	.read = gpgpu_doorbell_read,
	.write = gpgpu_doorbell_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};
