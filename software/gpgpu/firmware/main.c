/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：启动检查与调度循环
 */
#include "cp.h"

CpSlot cp_slots[GPGPU_HQD_SLOTS];
volatile u32 cp_stopped;
u32 cp_gpuvm;

/*
 * cp_boot_check() - 核对 Host 释放 CP 复位前准备好的启动状态
 *
 * Host 驱动装载固件后，只直接写好 KIQ 的 HQD 并激活，HIQ 和用户队列都要
 * 经管理命令建立。固件开始调度前确认这一约定成立：
 *
 * 1. 记下设备是否以 gpuvm=on 启动；设备协议版本须与固件编译时一致，
 *    一致才写 VM_FIRMWARE 完成握手，Host 读到它才知道固件已就绪；
 * 2. KIQ 槽已激活、是管理 Ring（AQL 标志为 0）且没有错误；
 * 3. HIQ 和两个用户槽未激活，也没有绑定 MQD。
 *
 * 任一条件不满足都说明 Host 与固件不匹配，调用 cp_fatal() 停机。
 * 检查通过后把 KIQ 的 MQD 地址登记为占用，与 MAP 建立的槽一致。
 */
static void cp_boot_check(void)
{
	/* 1. 读取 GPUVM 模式，核对协议版本后握手。 */
	cp_gpuvm = cp_reg_read(GPGPU_REG_VM_MODE) != 0;
	if (cp_reg_read(GPGPU_REG_VM_VERSION) != GPGPU_PROTOCOL_VERSION)
		cp_fatal();
	cp_reg_write(GPGPU_REG_VM_FIRMWARE, GPGPU_PROTOCOL_VERSION);

	/* 2. KIQ 由 Host 直接装载，必须已经可用。 */
	if (!cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_ACTIVE) ||
	    cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_AQL) ||
	    cp_hqd_read(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_ERROR))
		cp_fatal();

	/* 3. 其余槽必须为空，不能残留上一次运行的绑定。 */
	for (u32 slot = GPGPU_HQD_SLOT_HIQ; slot < GPGPU_HQD_SLOTS; slot++)
		if (cp_hqd_read(slot, GPGPU_HQD_ACTIVE) || cp_hqd_read64(slot, GPGPU_HQD_MQD_LO))
			cp_fatal();

	cp_slots[GPGPU_HQD_SLOT_KIQ].mqd = cp_hqd_read64(GPGPU_HQD_SLOT_KIQ, GPGPU_HQD_MQD_LO);
	cp_slots[GPGPU_HQD_SLOT_KIQ].occupied = 1;
}

/*
 * cp_main() - 固件主循环，由 start.S 建好运行环境后调用，不返回
 *
 * 启动检查通过后不断轮询，不依赖中断：
 *
 * 1. cp_stopped 已置位或 IH 通道故障时只执行 WFI，不再访问任何队列；
 * 2. KIQ、HIQ 各处理一条管理命令，管理请求不会被长任务饿死；
 * 3. 再处理一个用户派发包。first 是本轮优先的用户槽：它有工作时，
 *    下一轮换另一个槽优先；它空闲时才检查另一个槽，优先顺序不变。
 *    这样两条用户队列都有任务时交替执行。
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
