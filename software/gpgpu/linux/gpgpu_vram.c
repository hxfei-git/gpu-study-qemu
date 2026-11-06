// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：VRAM 分配器
 *
 * gpu_buddy 只管理固件区之前的 60 MiB（GPGPU_VRAM_USABLE_SIZE），VRAM 首页
 * 预先分配出去，保证 GPU 物理地址 0 永远不会分给用户或页表。
 */
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include "gpgpu_drv.h"

/* 每段清零的字节数；段间让出 CPU。 */
#define VRAM_CLEAR_CHUNK SZ_64K

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
 * @dev: 设备；调用者持 dev->lock，处在可睡眠的上下文。
 * @size: 非零页对齐字节数；起点按不小于 size 的最小 2 的幂对齐。
 * @blocks: 输出：持有的 buddy 块，用 gpgpu_vram_free() 归还。
 * @offset: 输出：VRAM 字节偏移，也是 GPU 物理地址。
 *
 * 清零经 BAR2 逐次写入。设备在另一进程、经 vfio-user 连接时每次写都要
 * 往返一次 socket，4 MiB 要几十秒，一次写完会触发 soft lockup，所以按
 * VRAM_CLEAR_CHUNK 分段，段间 cond_resched()。清零期间仍持 dev->lock，
 * 其他 ioctl 与 IH 处理要等它结束。
 *
 * Return: 0 或 gpu_buddy 的错误码。
 */
int gpgpu_vram_alloc(struct gpgpu_device *dev, u64 size, struct list_head *blocks, u64 *offset)
{
	struct gpu_buddy_block *block;
	u64 alignment = roundup_pow_of_two(size);
	int ret;

	ret = gpu_buddy_alloc_blocks(&dev->buddy, PAGE_SIZE, GPGPU_VRAM_USABLE_SIZE, size,
				     alignment, blocks, GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	if (ret)
		return ret;
	block = list_first_entry(blocks, struct gpu_buddy_block, link);
	*offset = gpu_buddy_block_offset(block);
	for (u64 done = 0; done < size; done += VRAM_CLEAR_CHUNK) {
		memset_io(dev->vram + *offset + done, 0, min_t(u64, size - done, VRAM_CLEAR_CHUNK));
		cond_resched();
	}
	return 0;
}

void gpgpu_vram_free(struct gpgpu_device *dev, struct list_head *blocks)
{
	gpu_buddy_free_list(&dev->buddy, blocks, 0);
}
