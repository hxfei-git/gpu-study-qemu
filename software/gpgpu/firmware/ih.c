/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：IH 通知、队列错误与固件致命错误
 *
 * IH 记录放在固件私有 VRAM 中，CP 写 IH_SUBMIT 后设备同步把它写入 Host Ring。
 * IH 通道一旦故障，Host 就收不到任何通知，因此固件停止全部调度。
 */
#include "cp.h"

/* 待提交的记录；设备只接受固件保留区内、8 字节对齐的记录地址。 */
static GpgpuIhRecord cp_ih_record __attribute__((aligned(8)));

/*
 * cp_ih_submit() - 发布一条 IH 记录
 *
 * 1. 在私有 VRAM 中填好记录；2. 写 IH_SUBMIT；3. 读回提交结果，失败则停止调度。
 */
int cp_ih_submit(u32 slot, u32 type, u32 data)
{
	u32 status;

	/* 1. 记录。 */
	cp_ih_record = (GpgpuIhRecord) {
		.type = type,
		.hqd_slot = slot,
		.mqd_addr = slot < GPGPU_HQD_SLOTS ? cp_slots[slot].mqd : 0,
		.data = data,
	};

	/* 2. 提交。 */
	cp_fence();
	cp_reg_write(GPGPU_REG_IH_SUBMIT, (unsigned long)&cp_ih_record);
	cp_fence();

	/* 3. 结果。 */
	status = cp_reg_read(GPGPU_REG_IH_STATUS);
	if ((status & GPGPU_IH_FAULT) || ((status >> GPGPU_IH_RESULT_SHIFT) & 0xff)) {
		cp_stopped = 1;
		return 0;
	}
	return 1;
}

int cp_ih_faulted(void)
{
	if (!(cp_reg_read(GPGPU_REG_IH_STATUS) & GPGPU_IH_FAULT))
		return 0;
	cp_stopped = 1;
	return 1;
}

/*
 * cp_queue_error() - 停止一条队列并记录首次错误
 *
 * 清除 ACTIVE 停止取包；HQD_ERROR 为 0 时写入本次错误并发 QUEUE_ERROR，
 * 已有错误时保持不变、不重复通知。槽的占用保留，等 Host 卸载。
 * KIQ 是所有管理命令的入口，KIQ 出错时停止全部调度。
 */
void cp_queue_error(u32 slot, u32 error)
{
	cp_hqd_write(slot, GPGPU_HQD_ACTIVE, 0);
	if (!cp_hqd_read(slot, GPGPU_HQD_ERROR)) {
		cp_hqd_write(slot, GPGPU_HQD_ERROR, error);
		cp_ih_submit(slot, GPGPU_IH_QUEUE_ERROR, error);
	}
	if (slot == GPGPU_HQD_SLOT_KIQ)
		cp_stopped = 1;
}

/* 报告设备错误后进入 WFI，不再访问任何对象。 */
void cp_fatal(void)
{
	cp_stopped = 1;
	cp_ih_submit(~0u, GPGPU_IH_DEVICE_ERROR, GPGPU_ERR_FIRMWARE);
	for (;;)
		__asm__ volatile("wfi");
}
