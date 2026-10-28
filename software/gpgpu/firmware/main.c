/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cp.h"

u32 cp_gpuvm;

CpSlot cp_slots[GPGPU_QUEUE_SLOTS];
volatile u32 cp_stopped;
static GpgpuIhRecord cp_ih_record __attribute__((aligned(8)));

/*
 * cp_ih_submit() - 通过设备同步发布一条 IH 记录
 *
 * 先准备私有 VRAM 记录，再写 SUBMIT 并检查结果；IH 故障停止全部调度。
 */
int cp_ih_submit(u32 slot, u32 type, u32 data)
{
	u32 status;

	cp_ih_record.type = type;
	cp_ih_record.hqd_slot = slot;
	cp_ih_record.mqd_addr = slot < GPGPU_QUEUE_SLOTS ? cp_slots[slot].mqd : 0;
	cp_ih_record.data = data;
	cp_ih_record.reserved[0] = 0;
	cp_ih_record.reserved[1] = 0;
	cp_ih_record.reserved[2] = 0;
	cp_fence();
	cp_reg_write(GPGPU_REG_IH_SUBMIT, (unsigned long)&cp_ih_record);
	cp_fence();
	status = cp_reg_read(GPGPU_REG_IH_STATUS);
	if ((status & GPGPU_IH_FAULT) || ((status >> GPGPU_IH_RESULT_SHIFT) & 0xff)) {
		cp_stopped = 1;
		return 0;
	}
	return 1;
}

/*
 * cp_queue_error() - 保存指定队列的首次错误并停止取包
 *
 * 只首次上报 QUEUE_ERROR，保留 MQD 占用；KIQ 故障还会停止全局调度。
 */
void cp_queue_error(u32 slot, u32 error)
{
	u32 old = cp_hqd_read(slot, GPGPU_HQD_ERROR);

	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	if (!old) {
		cp_hqd_write(slot, GPGPU_HQD_ERROR, error);
		cp_ih_submit(slot, GPGPU_IH_QUEUE_ERROR, error);
	}
	/* KIQ 是全部管理的根；停止调度不归还任何仍绑定的槽位。 */
	if (slot == 0)
		cp_stopped = 1;
}

/*
 * cp_fatal() - 报告固件不可恢复异常并停机
 *
 * 提交设备错误通知后进入 WFI 循环，不归还仍可能被访问的对象。
 */
void cp_fatal(void)
{
	cp_stopped = 1;
	cp_ih_submit(~0u, GPGPU_IH_DEVICE_ERROR, GPGPU_QUEUE_ERR_FIRMWARE);
	for (;;)
		__asm__ volatile("wfi");
}

/*
 * cp_next_packet() - 定位槽内下一待处理包
 *
 * 校验 Ring 与共享 W，按 R 计算物理槽；AQL 的预留跨度允许超过容量。
 */
int cp_next_packet(u32 slot, u64 *address, u32 packet_size)
{
	u32 capacity, rptr, wptr;
	u64 ring, poll;

	if (cp_stopped)
		return 0;
	/* Host 可在同一调度轮次使 IH 故障，每次选队列都重新确认。 */
	if (cp_reg_read(GPGPU_REG_IH_STATUS) & GPGPU_IH_FAULT) {
		cp_stopped = 1;
		return 0;
	}
	if (!cp_hqd_read(slot, GPGPU_HQD_ACTIVE))
		return 0;
	capacity = cp_hqd_read(slot, GPGPU_HQD_CONTROL);
	rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR);
	ring = cp_hqd_address(slot, GPGPU_HQD_RING_LO);
	poll = cp_hqd_address(slot, GPGPU_HQD_WPTR_ADDR_LO);
	if (capacity < GPGPU_QUEUE_RING_MIN || capacity > GPGPU_QUEUE_RING_MAX ||
	    (capacity & (capacity - 1)) || !cp_range(ring, capacity, 64) || !cp_range(poll, 4, 8) ||
	    !cp_read32(slot, cp_slots[slot].domain, cp_slots[slot].vmid, GPGPU_QUEUE_ERROR_RING,
		       poll, &wptr)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	/* AQL 的 W 包含等空槽的预留，不能用 W-R > 容量判错。 */
	if (packet_size == 32 && (u32)(wptr - rptr) > capacity / 32) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
		return 0;
	}
	if (rptr == wptr)
		return 0;
	cp_fence();
	*address = ring + (u64)(rptr & (capacity / packet_size - 1)) * packet_size;
	return 1;
}

/*
 * cp_retire() - 归还一个已经完成处理的 Ring 槽
 *
 * 先推进 HQD 的 R，再完整写回共享消费编号；写回失败停止该队列。
 */
int cp_retire(u32 slot)
{
	u32 rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR) + 1;
	u64 report = cp_hqd_address(slot, GPGPU_HQD_RPTR_ADDR_LO);

	cp_fence();
	cp_hqd_write(slot, GPGPU_HQD_RPTR, rptr);
	if (!cp_write32(slot, cp_slots[slot].domain, cp_slots[slot].vmid, GPGPU_QUEUE_ERROR_RETIRE,
			report, rptr)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RETIRE);
		return 0;
	}
	return 1;
}

/*
 * cp_main() - 运行 CP 冷启动与队列调度循环
 *
 * 核对 KIQ 初态，依次处理 KIQ/HIQ，并在两个用户槽之间轮转。
 */
void cp_main(void)
{
	u32 first = 2;
	u32 slot;

	cp_gpuvm = cp_reg_read(GPGPU_REG_VM_MODE) != 0;
	if (cp_reg_read(GPGPU_REG_VM_VERSION) != GPGPU_VM_VERSION)
		cp_fatal();
	cp_reg_write(GPGPU_REG_VM_FIRMWARE, GPGPU_VM_VERSION);
	/* 固件只接受 Host 在冷启动时准备的 KIQ；保留旧 HQD 不等于恢复。 */
	if (!cp_hqd_read(0, GPGPU_HQD_ACTIVE) || cp_hqd_read(0, GPGPU_HQD_AQL) ||
	    cp_hqd_read(0, GPGPU_HQD_ERROR)) {
		cp_fatal();
	}
	for (slot = 1; slot < GPGPU_QUEUE_SLOTS; slot++) {
		if (cp_hqd_read(slot, GPGPU_HQD_ACTIVE) || cp_hqd_read(slot, GPGPU_HQD_MQD_LO) ||
		    cp_hqd_read(slot, GPGPU_HQD_MQD_HI)) {
			cp_fatal();
		}
	}
	cp_slots[0].mqd = cp_hqd_address(0, GPGPU_HQD_MQD_LO);
	cp_slots[0].occupied = 1;
	for (;;) {
		if (cp_stopped || (cp_reg_read(GPGPU_REG_IH_STATUS) & GPGPU_IH_FAULT)) {
			cp_stopped = 1;
			__asm__ volatile("wfi");
			continue;
		}
		cp_management(0);
		cp_management(1);
		if (cp_aql(first)) {
			first ^= 1;
		} else if (cp_aql(first ^ 1)) {
			/* 当前另一槽成功处理，下次仍从原 first 开始。 */
		}
	}
}
