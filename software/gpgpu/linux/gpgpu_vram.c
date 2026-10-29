// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：VRAM 分配器
 *
 * gpu_buddy 只管理固件区之前的 60 MiB（GPGPU_VRAM_USABLE_SIZE），VRAM 首页
 * 预先分配出去，保证 GPU 物理地址 0 永远不会分给用户或页表。
 */
#include "gpgpu_drv.h"

int gpgpu_vram_init(struct gpgpu_device *dev)
{
	int ret = gpu_buddy_init(&dev->buddy, GPGPU_VRAM_USABLE_SIZE, PAGE_SIZE);

	if (ret)
		return ret;
	dev->buddy_ready = true;
	return gpu_buddy_alloc_blocks(&dev->buddy, 0, PAGE_SIZE, PAGE_SIZE, PAGE_SIZE,
				      &dev->reserved_blocks, GPU_BUDDY_RANGE_ALLOCATION);
}

/* 所有用户块与页表页都已归还后调用。 */
void gpgpu_vram_fini(struct gpgpu_device *dev)
{
	if (!dev->buddy_ready)
		return;
	gpu_buddy_free_list(&dev->buddy, &dev->reserved_blocks, 0);
	gpu_buddy_fini(&dev->buddy);
	dev->buddy_ready = false;
}

/**
 * gpgpu_vram_alloc() - 分配一段连续 VRAM 并清零
 * @dev: 设备；调用者持 dev->lock。
 * @size: 字节数，2 的幂且不小于 PAGE_SIZE；对齐等于 size。
 * @blocks: 输出：持有的 buddy 块，用 gpgpu_vram_free() 归还。
 * @offset: 输出：VRAM 字节偏移，也是 GPU 物理地址。
 *
 * Return: 0 或 gpu_buddy 的错误码。
 */
int gpgpu_vram_alloc(struct gpgpu_device *dev, u64 size, struct list_head *blocks, u64 *offset)
{
	struct gpu_buddy_block *block;
	int ret;

	ret = gpu_buddy_alloc_blocks(&dev->buddy, PAGE_SIZE, GPGPU_VRAM_USABLE_SIZE, size, size,
				     blocks, GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	if (ret)
		return ret;
	block = list_first_entry(blocks, struct gpu_buddy_block, link);
	*offset = gpu_buddy_block_offset(block);
	memset_io(dev->vram + *offset, 0, size);
	return 0;
}

void gpgpu_vram_free(struct gpgpu_device *dev, struct list_head *blocks)
{
	gpu_buddy_free_list(&dev->buddy, blocks, 0);
}
