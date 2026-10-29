/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：启动检查与调度循环
 */
#include "cp.h"

CpSlot cp_slots[GPGPU_HQD_SLOTS];
volatile u32 cp_stopped;
u32 cp_gpuvm;

/*
 * cp_boot_check() - 核对冷启动状态
 *
 * 1. 协议版本与设备一致，并写回 VM_FIRMWARE 完成握手；
 * 2. KIQ 已由 Host 激活，是管理 Ring 且没有错误；
 * 3. 其余槽未激活且没有绑定 MQD。
 * 任一条件不满足都说明 Host 与固件不匹配，报告致命错误。
 */
static void cp_boot_check(void)
{
	/* 1. 版本握手。 */
	cp_gpuvm = cp_reg_read(GPGPU_REG_VM_MODE) != 0;
	if (cp_reg_read(GPGPU_REG_VM_VERSION) != GPGPU_PROTOCOL_VERSION)
		cp_fatal();
	cp_reg_write(GPGPU_REG_VM_FIRMWARE, GPGPU_PROTOCOL_VERSION);

	/* 2. KIQ。 */
	if (!cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_ACTIVE) ||
	    cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_AQL) ||
	    cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_ERROR))
		cp_fatal();

	/* 3. 其余槽。 */
	for (u32 slot = GPGPU_HQD_SLOT_HIQ; slot < GPGPU_HQD_SLOTS; slot++)
		if (cp_hqd_read(slot, GPGPU_HQD_ACTIVE) || cp_hqd_read64(slot, GPGPU_HQD_MQD_LO))
			cp_fatal();

	cp_slots[GPGPU_HQD_SLOT_KIQ].mqd = cp_hqd_read64(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_MQD_LO);
	cp_slots[GPGPU_HQD_SLOT_KIQ].occupied = 1;
}

/*
 * cp_main() - 调度循环
 *
 * 每轮先处理 KIQ、HIQ 各一条管理命令，再处理一个用户派发包。两个用户槽
 * 轮流优先：优先槽本轮有工作时，下一轮换另一个槽优先；优先槽空闲时才看
 * 另一个槽，且不改变优先顺序。停止后只等待中断。
 */
void cp_main(void)
{
	u32 first = GPGPU_HQD_USER_FIRST;

	cp_boot_check();
	for (;;) {
		if (cp_stopped || cp_ih_faulted()) {
			__asm__ volatile("wfi");
			continue;
		}
		cp_management(GPGPU_HQD_SLOT_KIQ);
		cp_management(GPGPU_HQD_SLOT_HIQ);
		if (cp_aql(first))
			first ^= 1;
		else
			cp_aql(first ^ 1);
	}
}
