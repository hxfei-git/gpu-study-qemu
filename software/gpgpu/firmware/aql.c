/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cp.h"

/*
 * cp_packet_valid() - 校验 AQL Header 与派发规模
 *
 * 将总线程数换算为工作组数，拒绝未支持段大小和可能溢出的维度乘积。
 */
static int cp_packet_valid(const GpgpuAqlPacket *p, u32 grid[3], u32 block[3])
{
	u32 dimensions = p->full_header >> 16;
	u32 header = p->full_header & 0xffff;
	u32 i, groups = 1, threads = 1, total = 1;
	const u32 sizes[] = { p->grid_size_x, p->grid_size_y, p->grid_size_z };

	block[0] = p->workgroup_size_x;
	block[1] = p->workgroup_size_y;
	block[2] = p->workgroup_size_z;
	if ((header & ~GPGPU_AQL_BARRIER) != GPGPU_AQL_HEADER || dimensions < 1 || dimensions > 3 ||
	    p->reserved0 || p->reserved2 || p->private_segment_size || p->group_segment_size) {
		return 0;
	}
	for (i = 0; i < 3; i++) {
		if (!block[i] || !sizes[i] || sizes[i] % block[i] ||
		    (i >= dimensions && (sizes[i] != 1 || block[i] != 1))) {
			return 0;
		}
		grid[i] = sizes[i] / block[i];
		/* 先除后乘，避免截断后的乘积绕过任务规模限制。 */
		if (block[i] > 1024 / threads || grid[i] > 4096 / groups ||
		    sizes[i] > 65536 / total) {
			return 0;
		}
		threads *= block[i];
		groups *= grid[i];
		total *= sizes[i];
	}
	return 1;
}

/*
 * cp_descriptor() - 读取并验证 VRAM 中的 Kernel 描述对象
 *
 * 检查代码、参数大小和地址范围，避免访问固件保留区。
 */
static int cp_descriptor(const GpgpuAqlPacket *packet, GpgpuKernelDescriptor *desc)
{
	volatile const u32 *source;

	if (!cp_range(packet->kernel_object, sizeof(*desc), 8) ||
	    packet->kernel_object > GPGPU_VRAM_USABLE_SIZE - sizeof(*desc)) {
		return 0;
	}
	source = (volatile const u32 *)(unsigned long)packet->kernel_object;
	desc->code_entry = source[0];
	desc->code_size = source[1];
	desc->kernarg_size = source[2];
	desc->reserved = source[3];
	return !desc->reserved && desc->code_size && !(desc->code_size & 3) &&
		!(desc->code_entry & 3) && desc->code_entry < GPGPU_VRAM_USABLE_SIZE &&
		desc->code_size <= GPGPU_VRAM_USABLE_SIZE - desc->code_entry &&
		desc->kernarg_size >= 4 && desc->kernarg_size <= GPGPU_KERNEL_ARGS_MAX &&
		!(desc->kernarg_size & 3) &&
		cp_range(packet->kernarg_address, desc->kernarg_size, 8);
}

/*
 * cp_aql() - 处理一个已发布的用户派发包
 *
 * 确认 Header 后取包并等待执行器终态，依次发布 Signal、通知槽、IH 和 R。
 */
int cp_aql(u32 slot)
{
	GpgpuAqlPacket packet;
	GpgpuKernelDescriptor desc;
	u32 grid[3], block[3], status, detail, i, event, reserved;
	u64 address, value, mailbox;

	if (!cp_next_packet(slot, &address, sizeof(packet)))
		return 0;
	if (!cp_read32(slot, GPGPU_QUEUE_ERROR_RING, address, &packet.full_header)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	if ((packet.full_header & 0xff) == GPGPU_AQL_INVALID)
		return 0;
	cp_fence();
	if (!cp_read_object(slot, GPGPU_QUEUE_ERROR_RING, address + 4, (char *)&packet + 4,
			    sizeof(packet) - 4)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	if (!cp_packet_valid(&packet, grid, block)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
		return 0;
	}
	if (!cp_descriptor(&packet, &desc)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_DESCRIPTOR);
		return 0;
	}
	if (!cp_range(packet.completion_signal, sizeof(GpgpuSignal), 8) ||
	    !cp_read64(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal, &value) ||
	    value != 1 ||
	    !cp_read32(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal + 8, &event) ||
	    !event ||
	    !cp_read32(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal + 12, &reserved) ||
	    reserved ||
	    !cp_read64(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal + 16, &mailbox)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SIGNAL);
		return 0;
	}
	if (!cp_range(mailbox, 8, 8)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_MAILBOX);
		return 0;
	}
	if (cp_reg_read(GPGPU_REG_DISPATCH_STATUS) != GPGPU_DISPATCH_IDLE) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 0;
	}
	cp_reg_write(GPGPU_REG_KERNEL_ADDR_LO, desc.code_entry);
	cp_reg_write(GPGPU_REG_KERNEL_ADDR_HI, 0);
	cp_reg_write(GPGPU_REG_KERNEL_CODE_SIZE, desc.code_size);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_LO, (u32)packet.kernarg_address);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_HI, packet.kernarg_address >> 32);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_SIZE, desc.kernarg_size);
	for (i = 0; i < 3; i++) {
		cp_reg_write(GPGPU_REG_GRID_DIM_X + 4 * i, grid[i]);
		cp_reg_write(GPGPU_REG_BLOCK_DIM_X + 4 * i, block[i]);
	}
	cp_reg_write(GPGPU_REG_SHARED_MEM_SIZE, 0);
	cp_fence();
	if (cp_reg_read(GPGPU_REG_IH_STATUS) & GPGPU_IH_FAULT) {
		cp_stopped = 1;
		return 1;
	}
	cp_reg_write(GPGPU_REG_DISPATCH, 1);
	do {
		status = cp_reg_read(GPGPU_REG_DISPATCH_STATUS);
	} while (status == GPGPU_DISPATCH_BUSY);
	cp_fence();
	detail = cp_reg_read(GPGPU_REG_DISPATCH_ERROR);
	cp_reg_write(GPGPU_REG_DISPATCH_ACK, 1);
	if (status != GPGPU_DISPATCH_DONE || detail) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 1;
	}
	/* IH 故障可能在执行期间到达；结束执行器访问后停止成功收尾。 */
	if (cp_reg_read(GPGPU_REG_IH_STATUS) & GPGPU_IH_FAULT) {
		cp_stopped = 1;
		return 1;
	}
	cp_fence();
	if (!cp_read64(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal, &value) ||
	    value != 1 ||
	    !cp_write64(slot, GPGPU_QUEUE_ERROR_SIGNAL, packet.completion_signal, value - 1)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SIGNAL);
		return 1;
	}
	cp_fence();
	if (!cp_write64(slot, GPGPU_QUEUE_ERROR_MAILBOX, mailbox, event)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_MAILBOX);
		return 1;
	}
	cp_fence();
	if (!cp_ih_submit(slot, GPGPU_IH_SIGNAL_NOTIFY, event))
		return 1;
	if (!cp_write32(slot, GPGPU_QUEUE_ERROR_RETIRE, address, GPGPU_AQL_INVALID)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RETIRE);
		return 1;
	}
	cp_retire(slot);
	return 1;
}
