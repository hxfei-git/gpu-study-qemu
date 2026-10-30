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

/*
 * 返回管理槽 source 能操作的队列种类，并给出目标槽范围 [*begin, *end)。
 * KIQ 只能映射或卸载 HIQ（槽 1）；HIQ 只能管理用户 AQL 队列（槽 2、3）。
 */
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
 * cp_mqd_valid() - 检查从 Host 读来的 MQD 能否装进 HQD
 *
 * mqd 是 cp_map() 刚经访存窗口复制到固件栈上的副本，kind 为本次 MAP
 * 命令的目标种类。HQD 一旦激活，CP 就会按这些字段反复访问 Ring 和进度，
 * 所以装载前必须一次查完，避免运行中才发现地址或格式非法：
 *
 * 1. VMID：用户队列只能用 1 或 2，gpuvm=on 时该 VM 的上下文须已启用，
 *    Ring 与 R/W 进度地址都在 32 位 GPUVA 内；管理队列不翻译，VMID 须为 0；
 * 2. Ring 容量是允许范围内的 2 的幂，Ring 按 64 字节对齐，
 *    R/W 进度按 8 字节对齐；
 * 3. Doorbell 控制字只含使能位和偏移，偏移按 8 字节对齐，
 *    整个入口落在 BAR4 内；
 * 4. AQL 标志与队列种类一致，保存的 R 不超过 32 位，saved_error 为 0
 *    （带旧错误的 MQD 须先由 Host 清理），保留字段为 0。
 *
 * Return: 1 表示可以装载；0 时调用者返回 INVALID。
 */
static int cp_mqd_valid(const GpgpuMqd *mqd, u32 kind)
{
	u32 db = mqd->doorbell_control;
	u32 db_offset = db & GPGPU_DOORBELL_OFFSET_MASK;
	u32 capacity = mqd->pq_control;

	/* 1. 用户队列检查 VMID 与 32 位地址范围；管理队列只允许 VMID 0。 */
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

	/* 2. Ring 容量与 Ring、R/W 进度地址的对齐。 */
	if (!cp_ring_size_ok(capacity) || !cp_range_ok(mqd->ring_base, capacity, 64) ||
	    !cp_range_ok(mqd->rptr_report_addr, 4, 8) || !cp_range_ok(mqd->wptr_poll_addr, 4, 8))
		return 0;

	/* 3. Doorbell 控制字不能含未定义位，8 字节入口不能越过 BAR4 末尾。 */
	if ((db & ~(GPGPU_DOORBELL_ENABLE | GPGPU_DOORBELL_OFFSET_MASK)) || (db_offset & 7) ||
	    db_offset > GPGPU_DOORBELL_BAR_SIZE - 8)
		return 0;

	/* 4. 包格式与队列种类一致，没有遗留错误，保留字段为 0。 */
	return mqd->aql_control == (kind == GPGPU_QUEUE_USER_AQL) && !(mqd->saved_rptr >> 32) &&
		!mqd->saved_error && !mqd->reserved[0] && !mqd->reserved[1] && !mqd->reserved[2];
}

/*
 * 正常卸载的最后一步：清除已停止访问的 HQD 槽。
 * 先清 ACTIVE，保证清字段期间不会再按半旧半新的配置取包，再把地址、
 * R、Doorbell、VMID 等字段清零。容量和 HQD_ERROR 不清，首次错误
 * 留给 Host 查询，直到下一次 MAP 在 cp_load_hqd() 中清零。
 */
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

/* 解除固件私有的占用记录，槽此后才能被 MAP 选中；不修改 HQD 寄存器。 */
static void cp_release_slot(u32 slot)
{
	cp_slots[slot] = (CpSlot) { 0 };
}

/*
 * 把已校验的 MQD 写入 HQD 各字段，最后激活。
 * 先清旧错误，再写地址、容量、Doorbell、VMID 和保存的 R；fence 保证
 * 这些写入先于 ACTIVE=1 生效，设备和下一轮调度看到 ACTIVE 时配置已完整。
 * domain 同时记入 cp_slots，后续访问 Ring、Signal 时按它选择访问域。
 */
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
 * cp_map() - 执行 MAP_QUEUES：把 Host 准备好的 MQD 装载到空闲 HQD 槽
 *
 * source 是命令来源（KIQ 或 HIQ），决定目标种类和可选槽；
 * packet->address 是 MQD 的 GPU 物理地址。返回值先暂存，
 * 由紧随其后的 WRITE_COMPLETION 写回 Host 结果区：
 *
 * 1. 同一 MQD 地址已绑定在任何槽上则返回 DUPLICATE，原绑定不受影响；
 * 2. 在允许范围内找第一个未占用的槽，先登记占用和 MQD 地址再访问
 *    MQD，没有空槽返回 NO_SLOT；
 * 3. 把 MQD 复制到固件栈上并检查，读取失败返回 MQD_FAULT，
 *    内容非法返回 INVALID；
 * 4. 按队列自己的访问域读一次共享 W，确认进度地址可以访问；
 *    HIQ 还要求 W 与保存的 R 之差不超过 Ring 槽数，否则返回 INVALID；
 * 5. 写入 HQD 并激活，清除管理暂存标记后返回 OK。
 *
 * 第 3、4 步失败时只撤销第 2 步的占用登记，HQD 寄存器还没有被修改。
 */
static u32 cp_map(u32 source, const GpgpuManagementPacket *packet)
{
	u32 begin, end, slot, wptr, status;
	u32 kind = cp_target_slots(source, &begin, &end);
	u32 domain = kind == GPGPU_QUEUE_USER_AQL ? GPGPU_CP_DOMAIN_USER :
						    GPGPU_CP_DOMAIN_MANAGEMENT;
	GpgpuMqd mqd;

	/* 1. 查重：已占用的槽中有同一 MQD 地址，说明这条队列已经映射。 */
	for (slot = 0; slot < GPGPU_HQD_SLOTS; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			return GPGPU_MANAGEMENT_DUPLICATE;

	/* 2. 选槽并登记占用，后续失败经 undo 撤销。 */
	for (slot = begin; slot < end && cp_slots[slot].occupied; slot++)
		;
	if (slot == end)
		return GPGPU_MANAGEMENT_NO_SLOT;
	cp_slots[slot].occupied = 1;
	cp_slots[slot].mqd = packet->address;

	/* 3. MQD 总按管理域、VMID 0 读取，再检查各字段。 */
	status = GPGPU_MANAGEMENT_MQD_FAULT;
	if (!cp_read_object(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_DESCRIPTOR,
			    packet->address, &mqd, sizeof(mqd)))
		goto undo;
	status = GPGPU_MANAGEMENT_INVALID;
	if (!cp_mqd_valid(&mqd, kind))
		goto undo;

	/*
	 * 4. 用队列自己的访问域和 VMID 读 W：用户队列在 gpuvm=on 时
	 *    要经页表翻译，读失败说明进度地址不可用。
	 *    HIQ 的 W 越过保存的 R 一整个 Ring，说明 Host 发布的
	 *    命令超过容量。
	 */
	status = GPGPU_MANAGEMENT_MQD_FAULT;
	if (!cp_read32(slot, domain, mqd.vmid, GPGPU_QUEUE_ERROR_RING, mqd.wptr_poll_addr, &wptr))
		goto undo;
	status = GPGPU_MANAGEMENT_INVALID;
	if (kind == GPGPU_QUEUE_HIQ &&
	    (u32)(wptr - (u32)mqd.saved_rptr) > mqd.pq_control / sizeof(GpgpuManagementPacket))
		goto undo;

	/* 5. 写入 HQD 并激活；新队列没有待写回的管理结果。 */
	cp_load_hqd(slot, packet->address, &mqd, domain);
	cp_slots[slot].pending = 0;
	return GPGPU_MANAGEMENT_OK;
undo:
	cp_release_slot(slot);
	return status;
}

/*
 * cp_unmap() - 执行 UNMAP_QUEUES：停止队列，把进度和错误保存回 MQD
 *
 * packet->address 是要卸载队列的 MQD 地址。只有保存成功才释放槽，
 * 保存失败时槽保持占用，Host 不能复用这个 MQD 和槽：
 *
 * 1. 在 source 允许的范围内按完整 MQD 地址找到绑定的槽，找不到返回
 *    NOT_MAPPED；读出当前 R 和首次错误；
 * 2. 卸载 HIQ 前，它不能有错误、不能有尚未写回的管理结果，
 *    两个用户槽也必须都已卸载，否则返回 RECOVERY_REQUIRED，
 *    CP 不会连带卸载用户队列；
 * 3. 没有错误的队列必须已排空：读共享 W，要求 R == W，否则返回
 *    RECOVERY_REQUIRED。已出错的队列不要求排空，直接进入下一步；
 * 4. 清 ACTIVE 并关闭 Doorbell 使能，fence 后才写 MQD，保证不再取包；
 * 5. 把 R 零扩展为 64 位写入 saved_rptr，再写 saved_error。任一写失败
 *    返回 MQD_FAULT，并记 SAVE 错误、保留占用；
 * 6. 清除 HQD 字段并释放占用，返回 OK。
 *
 * 本固件等整个 Dispatch 结束后才处理管理命令，此时没有遗留的计算访问。
 */
static u32 cp_unmap(u32 source, const GpgpuManagementPacket *packet)
{
	u32 begin, end, slot, rptr, wptr, error;

	/* 1. 按 MQD 地址定位槽，读出要保存的 R 和首次错误。 */
	cp_target_slots(source, &begin, &end);
	for (slot = begin; slot < end; slot++)
		if (cp_slots[slot].occupied && cp_slots[slot].mqd == packet->address)
			break;
	if (slot == end)
		return GPGPU_MANAGEMENT_NOT_MAPPED;
	rptr = cp_hqd_read(slot, GPGPU_HQD_RPTR);
	error = cp_hqd_read(slot, GPGPU_HQD_ERROR);

	/* 2. HIQ 卸载前提：无错误、无待写回结果、用户槽 2、3 都已释放。 */
	if (slot == GPGPU_HQD_SLOT_HIQ &&
	    (error || cp_slots[slot].pending || cp_slots[GPGPU_HQD_USER_FIRST].occupied ||
	     cp_slots[GPGPU_HQD_USER_FIRST + 1].occupied))
		return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;

	/*
	 * 3. 正常队列须已排空。读不到 W 时该队列记 RING 错误，
	 *    本次卸载返回 MQD_FAULT，槽保持占用。
	 */
	if (!error) {
		if (!cp_queue_read32(slot, GPGPU_QUEUE_ERROR_RING,
				     cp_hqd_read64(slot, GPGPU_HQD_WPTR_ADDR_LO), &wptr)) {
			cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
			return GPGPU_MANAGEMENT_MQD_FAULT;
		}
		if (rptr != wptr)
			return GPGPU_MANAGEMENT_RECOVERY_REQUIRED;
	}

	/* 4. 停止取包并关闭 Doorbell；fence 使停用先于下面的 MQD 写入生效。 */
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	cp_hqd_write(slot, GPGPU_HQD_DOORBELL,
		     cp_hqd_read(slot, GPGPU_HQD_DOORBELL) & ~GPGPU_DOORBELL_ENABLE);
	cp_fence();

	/* 5. 保存进度与首次错误；失败时停在这里，不释放槽。 */
	if (!cp_write64(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_rptr), rptr) ||
	    !cp_write32(slot, GPGPU_CP_DOMAIN_MANAGEMENT, 0, GPGPU_QUEUE_ERROR_SAVE,
			packet->address + __builtin_offsetof(GpgpuMqd, saved_error), error)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SAVE);
		return GPGPU_MANAGEMENT_MQD_FAULT;
	}

	/* 6. 保存成功后清空 HQD 字段并解除占用。 */
	cp_clear_slot(slot);
	cp_release_slot(slot);
	return GPGPU_MANAGEMENT_OK;
}

/*
 * 执行一条 MAP/UNMAP，返回 GpgpuManagementStatus。
 * 先检查命令字段：queue_kind 必须是来源槽允许的种类，value 和保留字段
 * 为 0，MQD 地址 8 字节对齐且整个 MQD 在 32 位空间内。任一项不符直接
 * 返回 INVALID，不访问 MQD；这是普通管理失败，不会停止管理 Ring。
 */
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
 * cp_write_completion() - 执行 WRITE_COMPLETION，把暂存结果写回 Host 结果区
 *
 * packet->address 指向 16 字节结果区，packet->value 是驱动分配的非零
 * 完成序号。Host 轮询 completion 字段，看到本次序号后才读 status，所以：
 *
 * 1. 检查命令字段与结果区地址，再把 cp_slots[slot].result 写入 status；
 * 2. fence 保证 status 先对 Host 可见；
 * 3. 用单条 64 位 store 写入完成序号，Host 不会读到半个序号。
 *
 * Return: 1 表示两次写回都成功；0 时调用者记 COMPLETION 错误并停止该
 * 管理 Ring，Host 只能靠管理通道故障或超时退出等待。
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
 * 主循环每轮对 KIQ、HIQ 各调用一次，每次只处理一条 32 字节命令。
 * 驱动总是成对发布“MAP/UNMAP + WRITE_COMPLETION”，所以一个请求要
 * 跨两次调用完成，中间结果暂存在 cp_slots[slot].result，pending 标记
 * 下一条应是哪种命令：
 *
 * 1. 按 R、W 定位下一条命令并整体读入，Ring 不可读时记 RING 错误；
 * 2. pending 为 1 时，这一条必须是 WRITE_COMPLETION，写回后清除 pending；
 *    pending 为 0 时，这一条必须是 MAP 或 UNMAP，执行后暂存结果并置 pending。
 *    顺序不对记 PAIR 错误，未知操作码记 PACKET 错误，写回失败记 COMPLETION
 *    错误；这些错误都会停止该管理 Ring，且不推进 R；
 * 3. 调度未停止时推进 R 并写回共享进度，Host 才能复用这条命令的槽。
 */
void cp_management(u32 slot)
{
	CpSlot *state = &cp_slots[slot];
	GpgpuManagementPacket packet;
	u64 addr;

	/* 1. 没有已发布的命令时直接返回；读命令失败停止该管理 Ring。 */
	if (!cp_next_packet(slot, &addr, sizeof(packet)))
		return;
	if (!cp_queue_read_object(slot, GPGPU_QUEUE_ERROR_RING, addr, &packet, sizeof(packet))) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return;
	}

	/* 2. 按 pending 判断这一条应是执行命令还是写回命令。 */
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

	/* 3. 推进 R；KIQ 出错或 IH 故障使调度停止时，R 保持不动。 */
	if (!cp_stopped)
		cp_retire(slot);
}
