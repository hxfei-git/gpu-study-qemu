// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：VRAM 分配器
 *
 * gpu_buddy 只管理固件区之前的 VRAM（GPGPU_VRAM_USABLE_SIZE）。首页预先
 * 分配出去，保证 GPU 物理地址 0 永远不会分给用户或页表。
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

/* 所有用户块与页表页都已归还后调用；保留块随首页一起归还。 */
void gpgpu_vram_fini(struct gpgpu_device *dev)
{
	if (!dev->buddy_ready)
		return;
	gpu_buddy_free_list(&dev->buddy, &dev->reserved_blocks, 0);
	gpu_buddy_fini(&dev->buddy);
	dev->buddy_ready = false;
}

/*
 * vram_zero_dma() - 用设备只读零源清零尚未发布的 BO
 *
 * buddy 块已从空闲池移除，但尚未挂入会话、生成句柄或写进页表。
 * 调用者持 vm_serial 和设备引用，零源在设备寿命内保持不变：
 *
 *   固定 zero.addr --chunk 字节--> offset + done
 *
 * 例：零源 64 KiB、目的 132 KiB，三次长度为 64、64、4 KiB，
 * 源地址不变，目的偏移依次增加 0、64、128 KiB。
 *
 * 1. 释放 dev->lock，避免持主锁取 dma_lock 或等待设备；
 * 2. 持 dma_lock 分块调用共享提交/等待函数，成功后才推进目的偏移；
 * 3. 先放 dma_lock 再取回 dev->lock，所有返回路径恢复调用者主锁。
 *
 * 本函数不发布对象，不释放源或目的，失败后的块保留由分配函数处理。
 *
 * Return: 0，或共享 DMA 函数的 -EIO / -ETIMEDOUT。
 */
static int vram_zero_dma(struct gpgpu_device *dev, u64 offset, u64 size)
{
	const struct gpgpu_dma_buf *zero = &dev->vram_zero;
	int ret = 0;

	/* 1. 表页不走此路径；BO 未公开，私有块不会被并发释放。 */
	mutex_unlock(&dev->lock);

	/* 2. 整段清零与用户 COPY 共用 DMA 通道，长度取零源的唯一定义。 */
	mutex_lock(&dev->dma_lock);
	for (u64 done = 0; done < size;) {
		u32 chunk = min_t(u64, size - done, zero->size);

		ret = gpgpu_dma_copy(dev, false, zero->addr, offset + done, chunk);
		if (ret)
			break;
		done += chunk;
	}
	mutex_unlock(&dev->dma_lock);

	/* 3. stop 可在失锁期间改变设备状态，调用者发布前仍须复核。 */
	mutex_lock(&dev->lock);
	return ret;
}

int gpgpu_vram_alloc(struct gpgpu_device *dev, u64 size, struct list_head *blocks, u64 *offset,
		      bool for_bo)
{
	struct gpu_buddy_block *block;
	u64 alignment;
	int ret;

	/* 1. roundup_pow_of_two() 使用 unsigned long，取整结果不能越过其最高位。 */
	if (!size || !IS_ALIGNED(size, PAGE_SIZE) || size > GPGPU_VRAM_USABLE_SIZE ||
	    size > (1UL << (BITS_PER_LONG - 1)))
		return -EINVAL;

	alignment = roundup_pow_of_two(size);

	/* 2. 保持连续 buddy 分配，容量与碎片不足仍由分配器返回。 */
	ret = gpu_buddy_alloc_blocks(&dev->buddy, PAGE_SIZE, GPGPU_VRAM_USABLE_SIZE, size,
				     alignment, blocks, GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	if (ret)
		return ret;
	block = list_first_entry(blocks, struct gpu_buddy_block, link);
	*offset = gpu_buddy_block_offset(block);

	/* 3. 只有未发布 BO 可失锁；表页和默认路径保持锁内 BAR2。 */
	if (for_bo && dev->vram_zero.cpu) {
		ret = vram_zero_dma(dev, *offset, size);
		/* 超时整组隔离，不用 dcache 维护成功冒充 DMA 已经停止。 */
		if (ret == -ETIMEDOUT)
			list_splice_tail_init(blocks, &dev->reserved_blocks);
		else if (ret)
			gpgpu_vram_free(dev, blocks);
		return ret;
	}
	for (u64 done = 0; done < size; done += VRAM_CLEAR_CHUNK) {
		memset_io(dev->vram + *offset + done, 0, min_t(u64, size - done, VRAM_CLEAR_CHUNK));
		cond_resched();
	}
	return 0;
}

/*
 * 归还 blocks 中的全部块，之后 blocks 为空。任一块不能确认 dcache 已处置
 * 时整组转入 dev->reserved_blocks，不再分给新对象，设备释放时归还。
 * 调用者持 dev->lock，或设备已不再被并发使用。
 */
void gpgpu_vram_free(struct gpgpu_device *dev, struct list_head *blocks)
{
	struct gpu_buddy_block *block;

	list_for_each_entry(block, blocks, link) {
		if (!gpgpu_storage_release_ok(dev, gpu_buddy_block_offset(block),
					      gpu_buddy_block_size(&dev->buddy, block))) {
			list_splice_tail_init(blocks, &dev->reserved_blocks);
			return;
		}
	}
	gpu_buddy_free_list(&dev->buddy, blocks, 0);
}
