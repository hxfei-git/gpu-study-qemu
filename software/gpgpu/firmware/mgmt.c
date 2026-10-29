/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：管理命令
 *
 * KIQ 与 HIQ 的 Ring 中，每条 MAP_QUEUES / UNMAP_QUEUES 后面紧跟一条
 * WRITE_COMPLETION。固件执行前一条时把结果暂存在 cp_slots[].result，
 * 处理后一条时先写结果码、再写完成序号，Host 看到序号后读取结果码。
 *
 *   KIQ：queue_kind = HIQ，目标槽固定为 1
 *   HIQ：queue_kind = USER_AQL，目标槽为 2 或 3 中的空闲槽
 */
#include "cp.h"

/* 管理槽 source 能操作的目标种类与槽范围 [*begin, *end)。 */
static u32 cp_target_slots(u32 source, u32 *begin, u32 *end)
{
	if (source == GPGPU_HQD_SLOT_KIQ) {
		*begin = GPGPU_HQD_SLOT_HIQ;
		*end = GPGPU_HQD_SLOT_HIQ + 1;
		return GPGPU_QUEUE_HIQ;
	}
	*begin = GPGPU_HQD_USER_FIRST;
	*end = GPGPU_HQD_SLOTS;
	return GPGPU_QUEUE_USER_AQL;
}

/*
 * cp_mqd_valid() - 检查待装载的 MQD
 *
 * 1. 用户队列：VMID 为 1 或 2，gpuvm=on 时该 VM 已启用，Ring 与进度地址在
 *    32 位空间内；管理队列：VMID 为 0；
 * 2. Ring 容量合法，Ring 与 R/W 进度地址对齐；
 * 3. Doorbell 只含使能位和 8 字节对齐、位于 BAR4 内的偏移；
 * 4. 包格式与队列种类一致，保存的 R 不超过 32 位，没有旧错误，保留字段为 0。
 */
static int cp_mqd_valid(const GpgpuMqd *mqd, u32 kind)
{
	u32 db = mqd->doorbell_control;
	u32 db_offset = db & GPGPU_DOORBELL_OFFSET_MASK;
	u32 capacity = mqd->pq_control;

	/* 1. VMID。 */
	if (kind == GPGPU_QUEUE_USER_AQL) {
		if (mqd->vmid < 1 || mqd->vmid > GPGPU_VM_USERS ||
		    (cp_gpuvm && !cp_reg_read(GPGPU_REG_VM_CONTEXT(mqd->vmid) + GPGPU_VM_ENABLE)) ||
		    mqd->ring_base >= GPGPU_VM_ADDRESS_LIMIT ||
		    capacity > GPGPU_VM_ADDRESS_LIMIT - mqd->ring_base ||
		    mqd->rptr_report_addr >= GPGPU_VM_ADDRESS_LIMIT ||
		    mqd->wptr_poll_addr >= GPGPU_VM_ADDRESS_LIMIT)
			return 0;
	} else if (mqd->vmid) {
		return 0;
	}

	/* 2. Ring 与进度。 */
	if (!cp_ring_size_ok(capacity) || !cp_range_ok(mqd->ring_base, capacity, 64) ||
	    !cp_range_ok(mqd->rptr_report_addr, 4, 8) || !cp_range_ok(mqd->wptr_poll_addr, 4, 8))
		return 0;

	/* 3. Doorbell。 */
	if ((db & ~(GPGPU_DOORBELL_ENABLE | GPGPU_DOORBELL_OFFSET_MASK)) || (db_offset & 7) ||
	    db_offset > GPGPU_DOORBELL_BAR_SIZE - 8)
		return 0;

	/* 4. 格式与保存状态。 */
	return mqd->aql_control == (kind == GPGPU_QUEUE_USER_AQL) && !(mqd->saved_rptr >> 32) &&
		!mqd->saved_error && !mqd->reserved[0] && !mqd->reserved[1] && !mqd->reserved[2];
}

/* 清除已停止访问的槽：先停用，再清地址类字段。保留容量与首次错误，下次 MAP 才清错。 */
static void cp_clear_slot(u32 slot)
{
	static const u32 fields[] = {
		GPGPU_HQD_MQD_LO,	GPGPU_HQD_MQD_HI,	GPGPU_HQD_RING_LO,
		GPGPU_HQD_RING_HI,	GPGPU_HQD_RPTR_ADDR_LO, GPGPU_HQD_RPTR_ADDR_HI,
		GPGPU_HQD_WPTR_ADDR_LO, GPGPU_HQD_WPTR_ADDR_HI, GPGPU_HQD_RPTR,
		GPGPU_HQD_DOORBELL,	GPGPU_HQD_VMID,		GPGPU_HQD_AQL,
	};

	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	for (u32 i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		cp_hqd_write(slot, fields[i], 0);
}

static void cp_release_slot(u32 slot)
{
	cp_slots[slot] = (CpSlot) { 0 };
}

/* 把 MQD 写入 HQD 各字段，最后激活。调用者已校验 MQD。 */
static void cp_load_hqd(u32 slot, u64 mqd_addr, const GpgpuMqd *mqd, u32 domain)
{
	cp_hqd_write(slot, GPGPU_HQD_ERROR, 0);
	cp_hqd_write64(slot, GPGPU_HQD_MQD_LO, mqd_addr);
	cp_hqd_write64(slot, GPGPU_HQD_RING_LO, mqd->ring_base);
	cp_hqd_write64(slot, GPGPU_HQD_RPTR_ADDR_LO, mqd->rptr_report_addr);
	cp_hqd_write64(slot, GPGPU_HQD_WPTR_ADDR_LO, mqd->wptr_poll_addr);
	cp_hqd_write(slot, GPGPU_HQD_CONTROL, mqd->pq_control);
	cp_hqd_write(slot, GPGPU_HQD_DOORBELL, mqd->doorbell_control);
	cp_slots[slot].vmid = mqd->vmid;
	cp_slots[slot].domain = domain;
	cp_hqd_write(slot, GPGPU_HQD_VMID, mqd->vmid);
	cp_hqd_write(slot, GPGPU_HQD_AQL, mqd->aql_control);
	cp_hqd_write(slot, GPGPU_HQD_RPTR, (u32)mqd->saved_rptr);
	cp_fence();
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 1);
}

/*
 * cp_map() - MAP_QUEUES：把 MQD 装载到空闲槽
 *
 * 1. 同一 MQD 地址已绑定则返回 DUPLICATE；
 * 2. 在允许范围内找空闲槽并登记占用，没有则返回 NO_SLOT；
 * 3. 读取并检查 MQD；
 * 4. 读一次共享 W 确认进度地址可访问；HIQ 的 W 不能越过保存的 R 一个 Ring；
 * 5. 写入 HQD 并激活。
 * 第 3、4 步失败时撤销占用，HQD 未被修改。
 */
static u32 cp_map(u32 source, const GpgpuManagementPacket *packet)
{
	u32 begin, end, slot, wptr, status;
	u32 kind = cp_target_slots(source, &begin, &end);
	u32 domain = kind == GPGPU_QUEUE_USER_AQL ? GPGPU_CP_DOMAIN_USER :
						    GPGPU_CP_DOMAIN_MANAGEMENT;
	GpgpuMqd mqd;

	/* 1. 查重。 */
	for (slot = 0; slot < GPGPU_HQD_SLOTS; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			return GPGPU_MANAGEMENT_DUPLICATE;

	/* 2. 空闲槽。 */
	for (slot = begin; slot < end && cp_slots[slot].occupied; slot++)
		;
	if (slot == end)
		return GPGPU_MANAGEMENT_NO_SLOT;
	cp_slots[slot].occupied = 1;
	cp_slots[slot].mqd = packet->address;

	/* 3. MQD。 */
	status = GPGPU_MANAGEMENT_MQD_FAULT;
	if (!cp_read_object(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_DESCRIPTOR,
			    packet->address, &mqd, sizeof(mqd)))
		goto undo;
	status = GPGPU_MANAGEMENT_INVALID;
	if (!cp_mqd_valid(&mqd, kind))
		goto undo;

	/* 4. 进度。 */
	status = GPGPU_MANAGEMENT_MQD_FAULT;
	if (!cp_read32(slot, domain, mqd.vmid, GPGPU_QUEUE_ERROR_RING, mqd.wptr_poll_addr, &wptr))
		goto undo;
	status = GPGPU_MANAGEMENT_INVALID;
	if (kind == GPGPU_QUEUE_HIQ &&
	    (u32)(wptr - (u32)mqd.saved_rptr) > mqd.pq_control / sizeof(GpgpuManagementPacket))
		goto undo;

	/* 5. 装载。 */
	cp_load_hqd(slot, packet->address, &mqd, domain);
	cp_slots[slot].pending = 0;
	return GPGPU_MANAGEMENT_OK;
undo:
	cp_release_slot(slot);
	return status;
}

/*
 * cp_unmap() - UNMAP_QUEUES：停止队列并保存状态到 MQD
 *
 * 1. 在允许范围内按 MQD 地址找到绑定的槽，没有则返回 NOT_MAPPED；
 * 2. HIQ 还有用户队列、未回报的管理请求或已出错时返回 RECOVERY_REQUIRED；
 * 3. 没有错误的队列必须已排空（R == W），否则返回 RECOVERY_REQUIRED；
 * 4. 停用槽并关闭 Doorbell；
 * 5. 把 R 和首次错误写回 MQD，失败返回 MQD_FAULT 并保留占用；
 * 6. 清除 HQD 并释放槽。
 * 本固件等整个 Dispatch 结束后才处理管理命令，此时没有遗留的计算访问。
 */
static u32 cp_unmap(u32 source, const GpgpuManagementPacket *packet)
{
	u32 begin, end, slot, rptr, wptr, error;

	/* 1. 定位。 */
	cp_target_slots(source, &begin, &end);
	for (slot = begin; slot < end; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			break;
	if (slot == end)
		return GPGPU_MANAGEMENT_NOT_MAPPED;
	rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR);
	error = cp_hqd_read(slot, GPGPU_HQD_ERROR);

	/* 2. HIQ 的前提。 */
	if (slot == GPGPU_HQD_SLOT_HIQ &&
	    (error || cp_slots[slot].pending || cp_slots[GPGPU_HQD_USER_FIRST].occupied ||
	     cp_slots[GPGPU_HQD_USER_FIRST + 1].occupied))
		return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;

	/* 3. 排空。 */
	if (!error) {
		if (!cp_queue_read32(slot, GPGPU_QUEUE_ERROR_RING,
				     cp_hqd_read64(slot, GPGPU_HQD_WPTR_ADDR_LO), &wptr)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
			return GPGPU_MANAGEMENT_MQD_FAULT;
		}
		if (rptr != wptr)
			return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;
	}

	/* 4. 停用。 */
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	cp_hqd_write(slot, GPGPU_HQD_DOORBELL,
		     cp_hqd_read(slot, GPGPU_HQD_DOORBELL) & ~GPGPU_DOORBELL_ENABLE);
	cp_fence();

	/* 5. 保存。 */
	if (!cp_write64(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_rptr), rptr) ||
	    !cp_write32(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_error), error)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SAVE);
		return GPGPU_MANAGEMENT_MQD_FAULT;
	}

	/* 6. 释放。 */
	cp_clear_slot(slot);
	cp_release_slot(slot);
	return GPGPU_MANAGEMENT_OK;
}

/* 执行一条 MAP/UNMAP，返回 GpgpuManagementStatus；参数非法直接返回 INVALID。 */
static u32 cp_execute(u32 slot, const GpgpuManagementPacket *packet)
{
	u32 begin, end;

	if (packet->queue_kind != cp_target_slots(slot, &begin, &end) || packet->value ||
	    packet->reserved || !cp_range_ok(packet->address, sizeof(GpgpuMqd), 8))
		return GPGPU_MANAGEMENT_INVALID;
	if (packet->opcode == GPGPU_MAP_QUEUES)
		return cp_map(slot, packet);
	return cp_unmap(slot, packet);
}

/*
 * cp_write_completion() - 把暂存结果写回 Host
 *
 * 先写 status，可见后再写 completion：Host 看到本次序号时 status 一定已更新。
 * 结果区非法或写回失败都是 COMPLETION 错误。
 */
static int cp_write_completion(u32 slot, const GpgpuManagementPacket *packet)
{
	if (packet->queue_kind || packet->reserved || !packet->value ||
	    !cp_range_ok(packet->address, sizeof(GpgpuManagementResult), 8) ||
	    !cp_queue_write32(slot, GPGPU_QUEUE_ERROR_COMPLETION,
			      packet->address + __builtin_offsetof(GpgpuManagementResult, status),
			      cp_slots[slot].result))
		return 0;
	cp_fence();
	return cp_queue_write64(slot, GPGPU_QUEUE_ERROR_COMPLETION, packet->address, packet->value);
}

/*
 * cp_management() - 处理 KIQ 或 HIQ 中的一条管理命令
 *
 * 1. 取一条命令；
 * 2. 已有暂存结果时，这一条必须是 WRITE_COMPLETION，写回后清除暂存；
 *    否则这一条必须是 MAP/UNMAP，执行后暂存结果；
 *    顺序不对是 PAIR 错误，未知操作码是 PACKET 错误；
 * 3. 回收这一条命令的 Ring 槽。
 */
void cp_management(u32 slot)
{
	CpSlot *state = &cp_slots[slot];
	GpgpuManagementPacket packet;
	u64 addr;

	/* 1. 取包。 */
	if (!cp_next_packet(slot, &addr, sizeof(packet)))
		return;
	if (!cp_queue_read_object(slot, GPGPU_QUEUE_ERROR_RING, addr, &packet, sizeof(packet))) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return;
	}

	/* 2. 配对执行。 */
	if (state->pending) {
		if (packet.opcode != GPGPU_WRITE_COMPLETION) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PAIR);
			return;
		}
		if (!cp_write_completion(slot, &packet)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_COMPLETION);
			return;
		}
		state->pending = 0;
	} else {
		if (packet.opcode == GPGPU_WRITE_COMPLETION) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PAIR);
			return;
		}
		if (packet.opcode != GPGPU_MAP_QUEUES && packet.opcode != GPGPU_UNMAP_QUEUES) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
			return;
		}
		state->result = cp_execute(slot, &packet);
		state->pending = 1;
	}

	/* 3. 回收。 */
	if (!cp_stopped)
		cp_retire(slot);
}
