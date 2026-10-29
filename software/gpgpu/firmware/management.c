/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cp.h"

/*
 * cp_mqd_valid() - 校验待装载的 MQD
 *
 * 检查 Ring、进度与 Doorbell 对齐，限制 VMID/格式并拒绝旧错误和保留位。
 */
static int cp_mqd_valid(const GpgpuMqd *mqd, u32 kind)
{
	u32 db = mqd->doorbell_control;
	u32 offset = db & GPGPU_DOORBELL_OFFSET_MASK;
	u32 capacity = mqd->pq_control;

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

	return capacity >= GPGPU_QUEUE_RING_MIN && capacity <= GPGPU_QUEUE_RING_MAX &&
		!(capacity & (capacity - 1)) && cp_range(mqd->ring_base, capacity, 64) &&
		cp_range(mqd->rptr_report_addr, 4, 8) && cp_range(mqd->wptr_poll_addr, 4, 8) &&
		!(db & ~(GPGPU_DOORBELL_ENABLE | GPGPU_DOORBELL_OFFSET_MASK)) && !(offset & 7) &&
		offset <= GPGPU_DOORBELL_BAR_SIZE - 8 &&
		mqd->aql_control == (kind == GPGPU_QUEUE_USER_AQL) && !(mqd->saved_rptr >> 32) &&
		!mqd->saved_error && !mqd->reserved[0] && !mqd->reserved[1] && !mqd->reserved[2];
}

/*
 * cp_clear_slot() - 清除已经停止访问的槽绑定配置
 *
 * 清 ACTIVE 和地址字段，保留旧容量及首次错误，私有占用另行解除。
 */
static void cp_clear_slot(u32 slot)
{
	static const u32 fields[] = {
		GPGPU_HQD_MQD_LO,	GPGPU_HQD_MQD_HI,	GPGPU_HQD_RING_LO,
		GPGPU_HQD_RING_HI,	GPGPU_HQD_RPTR_ADDR_LO, GPGPU_HQD_RPTR_ADDR_HI,
		GPGPU_HQD_WPTR_ADDR_LO, GPGPU_HQD_WPTR_ADDR_HI, GPGPU_HQD_RPTR,
		GPGPU_HQD_DOORBELL,	GPGPU_HQD_VMID,		GPGPU_HQD_AQL,
	};
	u32 i;

	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		cp_hqd_write(slot, fields[i], 0);
	/*
	 * 空槽保留旧容量与首次错误，下一次 MAP 才清错；占用另行记录。
	 */
}

/*
 * cp_map() - 把 MQD 装载到允许范围内的空闲 HQD
 *
 * 先按完整地址查重并登记占用，读取校验成功后写配置，最后激活。
 */
static u32 cp_map(u32 source, const GpgpuManagementPacket *packet)
{
	GpgpuMqd mqd;
	u32 slot, begin, end, wptr;

	for (slot = 0; slot < GPGPU_HQD_SLOTS; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			return GPGPU_MANAGEMENT_DUPLICATE;
	begin = packet->queue_kind == GPGPU_QUEUE_HIQ ? 1 : 2;
	end = packet->queue_kind == GPGPU_QUEUE_HIQ ? 2 : 4;
	for (slot = begin; slot < end && cp_slots[slot].occupied; slot++) {
	}
	if (slot == end)
		return GPGPU_MANAGEMENT_NO_SLOT;
	cp_slots[slot].occupied = 1;
	cp_slots[slot].mqd = packet->address;
	cp_access.source = source;
	cp_access.kind = packet->queue_kind;
	cp_access.mqd = packet->address;
	if (!cp_read_object(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_DESCRIPTOR,
			    packet->address, &mqd, sizeof(mqd))) {
		cp_slots[slot].occupied = 0;
		cp_slots[slot].mqd = 0;
		return GPGPU_MANAGEMENT_MQD_FAULT;
	}
	if (!cp_mqd_valid(&mqd, packet->queue_kind)) {
		cp_slots[slot].occupied = 0;
		cp_slots[slot].mqd = 0;
		return GPGPU_MANAGEMENT_INVALID;
	}
	if (!cp_read32(slot,
		       packet->queue_kind == GPGPU_QUEUE_USER_AQL ? GPGPU_CP_DOMAIN_USER :
								    GPGPU_CP_DOMAIN_MANAGEMENT,
		       mqd.vmid, GPGPU_QUEUE_ERROR_RING, mqd.wptr_poll_addr, &wptr)) {
		cp_slots[slot].occupied = 0;
		cp_slots[slot].mqd = 0;
		return GPGPU_MANAGEMENT_MQD_FAULT;
	}
	if (packet->queue_kind == GPGPU_QUEUE_HIQ &&
	    (u32)(wptr - (u32)mqd.saved_rptr) > mqd.pq_control / 32) {
		cp_slots[slot].occupied = 0;
		cp_slots[slot].mqd = 0;
		return GPGPU_MANAGEMENT_INVALID;
	}
	cp_hqd_write(slot, GPGPU_HQD_ERROR, 0);
	cp_hqd_set_address(slot, GPGPU_HQD_MQD_LO, packet->address);
	cp_hqd_set_address(slot, GPGPU_HQD_RING_LO, mqd.ring_base);
	cp_hqd_set_address(slot, GPGPU_HQD_RPTR_ADDR_LO, mqd.rptr_report_addr);
	cp_hqd_set_address(slot, GPGPU_HQD_WPTR_ADDR_LO, mqd.wptr_poll_addr);
	cp_hqd_write(slot, GPGPU_HQD_CONTROL, mqd.pq_control);
	cp_hqd_write(slot, GPGPU_HQD_DOORBELL, mqd.doorbell_control);
	cp_slots[slot].vmid = mqd.vmid;
	cp_slots[slot].domain = packet->queue_kind == GPGPU_QUEUE_USER_AQL ?
		GPGPU_CP_DOMAIN_USER :
		GPGPU_CP_DOMAIN_MANAGEMENT;
	cp_hqd_write(slot, GPGPU_HQD_VMID, mqd.vmid);
	cp_hqd_write(slot, GPGPU_HQD_AQL, mqd.aql_control);
	cp_hqd_write(slot, GPGPU_HQD_RPTR, (u32)mqd.saved_rptr);
	cp_slots[slot].pending = 0;
	cp_fence();
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 1);
	return GPGPU_MANAGEMENT_OK;
}

/*
 * cp_unmap() - 停止目标槽并保存 MQD 状态
 *
 * 正常队列须排空；关闭 ACTIVE/Doorbell 后保存 R 和错误，成功才解除占用。
 */
static u32 cp_unmap(const GpgpuManagementPacket *packet)
{
	u32 slot, begin, end, rptr, wptr, error;
	u64 poll;

	begin = packet->queue_kind == GPGPU_QUEUE_HIQ ? 1 : 2;
	end = packet->queue_kind == GPGPU_QUEUE_HIQ ? 2 : 4;
	for (slot = begin; slot < end; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			break;
	if (slot == end)
		return GPGPU_MANAGEMENT_NOT_MAPPED;
	rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR);
	error = cp_hqd_read(slot, GPGPU_HQD_ERROR);
	/*
	 * 故障 HIQ 的重建属于后续恢复；不能绕过存活用户槽和未回报请求。
	 */
	if (slot == 1 &&
	    (error || cp_slots[slot].pending || cp_slots[2].occupied || cp_slots[3].occupied)) {
		return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;
	}
	if (!error) {
		poll = cp_hqd_address(slot, GPGPU_HQD_WPTR_ADDR_LO);
		if (!cp_read32(slot, cp_slots[slot].domain, cp_slots[slot].vmid,
			       GPGPU_QUEUE_ERROR_RING, poll, &wptr)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
			return GPGPU_MANAGEMENT_MQD_FAULT;
		}
		if (rptr != wptr)
			return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;
	}
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	cp_hqd_write(slot, GPGPU_HQD_DOORBELL,
		     cp_hqd_read(slot, GPGPU_HQD_DOORBELL) & ~GPGPU_DOORBELL_ENABLE);
	/*
	 * 本固件等待整个 Dispatch 终态后才调度管理，故没有遗留计算访问。
	 */
	cp_fence();
	if (!cp_write64(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_rptr), rptr) ||
	    !cp_write32(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_error), error)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SAVE);
		return GPGPU_MANAGEMENT_MQD_FAULT;
	}
	cp_clear_slot(slot);
	cp_slots[slot].occupied = 0;
	cp_slots[slot].mqd = 0;
	cp_slots[slot].pending = 0;
	return GPGPU_MANAGEMENT_OK;
}

/*
 * cp_management() - 消费一条 KIQ 或 HIQ 管理命令
 *
 * 将操作结果暂存到槽状态，要求相邻完成命令先写结果码、再发布序号。
 */
void cp_management(u32 slot)
{
	GpgpuManagementPacket packet;
	u64 address;
	u32 result;

	if (!cp_next_packet(slot, &address, sizeof(packet)))
		return;
	if (!cp_read_object(slot, cp_slots[slot].domain, cp_slots[slot].vmid,
			    GPGPU_QUEUE_ERROR_RING, address, &packet, sizeof(packet))) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return;
	}
	if (cp_slots[slot].pending) {
		if (packet.opcode != GPGPU_WRITE_COMPLETION) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PAIR);
			return;
		}
		if (packet.queue_kind || packet.reserved || !packet.value ||
		    !cp_range(packet.address, sizeof(GpgpuManagementResult), 8)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_COMPLETION);
			return;
		}
		if (!cp_write32(slot, cp_slots[slot].domain, cp_slots[slot].vmid,
				GPGPU_QUEUE_ERROR_COMPLETION, packet.address + 8,
				cp_slots[slot].result)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_COMPLETION);
			return;
		}
		cp_fence();
		if (!cp_write64(slot, cp_slots[slot].domain, cp_slots[slot].vmid,
				GPGPU_QUEUE_ERROR_COMPLETION, packet.address, packet.value)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_COMPLETION);
			return;
		}
		cp_slots[slot].pending = 0;
	} else {
		if (packet.opcode == GPGPU_WRITE_COMPLETION) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PAIR);
			return;
		}
		if (packet.opcode != GPGPU_MAP_QUEUES && packet.opcode != GPGPU_UNMAP_QUEUES) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
			return;
		}
		if (packet.queue_kind != slot + 1 || packet.value || packet.reserved ||
		    !cp_range(packet.address, sizeof(GpgpuMqd), 8)) {
			result = GPGPU_MANAGEMENT_INVALID;
		} else if (packet.opcode == GPGPU_MAP_QUEUES) {
			result = cp_map(slot, &packet);
		} else {
			result = cp_unmap(&packet);
		}
		cp_slots[slot].result = result;
		cp_slots[slot].pending = 1;
	}
	if (!cp_stopped)
		cp_retire(slot);
}
