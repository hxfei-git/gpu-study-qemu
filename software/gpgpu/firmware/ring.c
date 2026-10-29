/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：Ring 取包与回收
 *
 * R（HQD_RPTR）与 W（共享内存中的 32 位编号）都是包编号，不是字节偏移，
 * 允许 32 位回绕。物理槽号为 编号 mod 槽数。
 *
 * 管理 Ring：W 只标出已发布的命令，W - R 不能超过槽数。
 * 用户 AQL Ring：W 是“已预留”编号，Host 可能先取号再等空槽，因此 W - R
 * 可以超过槽数；包是否可处理由 Header 是否为 INVALID 决定（见 aql.c）。
 */
#include "cp.h"

/*
 * cp_next_packet() - 定位 slot 中下一个待处理包
 *
 * 1. 调度已停止、IH 已故障或槽未激活时没有包；
 * 2. 检查 Ring 配置并读取共享 W，失败视为 RING 错误；
 * 3. 管理 Ring 的 W 越过容量视为 PACKET 错误；
 * 4. R == W 时没有包，否则返回 R 对应的槽地址。
 */
int cp_next_packet(u32 slot, u64 *addr, u32 packet_size)
{
	u32 capacity, rptr, wptr;
	u64 ring, wptr_addr;

	/* 1. 可调度。 */
	if (cp_stopped || cp_ih_faulted() || !cp_hqd_read(slot, GPGPU_HQD_ACTIVE))
		return 0;

	/* 2. 配置与 W。 */
	capacity = cp_hqd_read(slot, GPGPU_HQD_CONTROL);
	rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR);
	ring = cp_hqd_read64(slot, GPGPU_HQD_RING_LO);
	wptr_addr = cp_hqd_read64(slot, GPGPU_HQD_WPTR_ADDR_LO);
	if (!cp_ring_size_ok(capacity) || !cp_range_ok(ring, capacity, 64) ||
	    !cp_range_ok(wptr_addr, 4, 8) ||
	    !cp_queue_read32(slot, GPGPU_QUEUE_ERROR_RING, wptr_addr, &wptr)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}

	/* 3. 管理 Ring 的发布范围。 */
	if (packet_size == sizeof(GpgpuManagementPacket) &&
	    (u32)(wptr - rptr) > capacity / packet_size) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
		return 0;
	}

	/* 4. 下一个包。 */
	if (rptr == wptr)
		return 0;
	cp_fence();
	*addr = ring + (u64)(rptr & (capacity / packet_size - 1)) * packet_size;
	return 1;
}

/* 先推进 HQD 中的 R，再把它写回共享进度供 Host 复用槽。 */
int cp_retire(u32 slot)
{
	u32 rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR) + 1;
	u64 report = cp_hqd_read64(slot, GPGPU_HQD_RPTR_ADDR_LO);

	cp_fence();
	cp_hqd_write(slot, GPGPU_HQD_RPTR, rptr);
	if (!cp_queue_write32(slot, GPGPU_QUEUE_ERROR_RETIRE, report, rptr)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RETIRE);
		return 0;
	}
	return 1;
}
