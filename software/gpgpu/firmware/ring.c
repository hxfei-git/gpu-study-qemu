// SPDX-License-Identifier: GPL-2.0-or-later
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
 * R 取自 HQD，W 每次都从 Host 共享进度重新读取，所以即使 Doorbell 没有
 * 送达，CP 下一轮轮询也能发现新包。packet_size 为 32 时按管理 Ring
 * 处理，为 64 时按 AQL Ring 处理。本函数只给出包地址，不推进 R：
 *
 * 1. 调度已停止、IH 已故障或槽未激活时直接返回 0，不访问任何对象；
 * 2. 从 HQD 读容量、R、Ring 基址和 W 地址，检查后经访存窗口读共享 W，
 *    配置非法或读取失败都记 RING 错误并停止该队列；
 * 3. 管理 Ring 的 W 只标出已发布的命令，W - R 超过槽数说明 Host 写坏了
 *    W，记 PACKET 错误。AQL Ring 不做这项检查，原因见文件头；
 * 4. R == W 时没有包；否则 fence 保证先读到 W 再读包内容，
 *    并按“R mod 槽数”返回物理槽地址。
 *
 * Return: 1 表示 *addr 有效；0 表示没有包或已停止该队列。
 */
int cp_next_packet(u32 slot, u64 *addr, u32 packet_size)
{
	u32 capacity, rptr, wptr;
	u64 ring, wptr_addr;

	/* 1. 停止状态下不再访问 Host 内存。 */
	if (cp_stopped || cp_ih_faulted() || !cp_hqd_read(slot, GPGPU_HQD_ACTIVE))
		return 0;

	/* 2. 读取 HQD 配置，再读共享 W。 */
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

	/* 3. 管理 Ring 已发布的命令数不能超过槽数。 */
	if (packet_size == sizeof(GpgpuManagementPacket) &&
	    (u32)(wptr - rptr) > capacity / packet_size) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
		return 0;
	}

	/* 4. 先确认读到 W 再读包；槽数是 2 的幂，用掩码代替取模。 */
	if (rptr == wptr)
		return 0;
	cp_fence();
	*addr = ring + (u64)(rptr & (capacity / packet_size - 1)) * packet_size;
	return 1;
}

/*
 * cp_retire() - 回收当前包占用的 Ring 槽
 *
 * 调用者已经处理完 R 指向的包。fence 保证此前对包和 Host 对象的读写都已
 * 完成，再把 HQD 中的 R 加 1，然后把新 R 写回 Host 共享进度；
 * Host 读到新 R 才会复用这个槽。写回失败记 RETIRE 错误并停止
 * 该队列，此时 HQD 中的 R 已经推进。
 *
 * Return: 1 表示写回成功。
 */
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
