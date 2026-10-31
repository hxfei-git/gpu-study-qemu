// SPDX-License-Identifier: GPL-2.0-or-later
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
 * cp_ih_submit() - 发布一条 IH 记录，通知 Host 驱动
 *
 * slot 是记录归属的 HQD 槽，取其绑定的 MQD 地址供驱动找到队列；
 * 设备错误不属于任何槽，传 ~0u，MQD 地址填 0。设备在 IH_SUBMIT 写入时
 * 同步完成复制、发布 W 和中断，所以写完即可读回本次结果：
 *
 * 1. 在固件私有 VRAM 的 cp_ih_record 中填好类型、槽位、MQD 地址和数据；
 * 2. fence 保证记录内容先写完，再把记录地址写入 IH_SUBMIT；
 *    第二个 fence 保证提交完成后才读状态；
 * 3. 读 IH_STATUS：通道故障或本次结果非零都说明 Host 收不到通知，
 *    置 cp_stopped 停止全部调度。
 *
 * Return: 1 表示记录已发布。
 */
int cp_ih_submit(u32 slot, u32 type, u32 data)
{
	u32 status;

	/* 1. 填写记录；槽号越界（设备错误）时没有 MQD 地址。 */
	cp_ih_record = (GpgpuIhRecord) {
		.type = type,
		.hqd_slot = slot,
		.mqd_addr = slot < GPGPU_HQD_SLOTS ? cp_slots[slot].mqd : 0,
		.data = data,
	};

	/* 2. 记录写完后再提交，提交后再读结果。 */
	cp_fence();
	cp_reg_write(GPGPU_REG_IH_SUBMIT, (unsigned long)&cp_ih_record);
	cp_fence();

	/* 3. 检查通道故障位和本次提交的结果码。 */
	status = cp_reg_read(GPGPU_REG_IH_STATUS);
	if ((status & GPGPU_IH_FAULT) || ((status >> GPGPU_IH_RESULT_SHIFT) & 0xff)) {
		cp_stopped = 1;
		return 0;
	}
	return 1;
}

/*
 * 检查 IH 通道是否已故障。故障可能来自本固件的提交，
 * 也可能来自 Host 侧的配置或 DMA 错误；无论哪种，
 * 之后的完成通知都送不到 Host，所以置 cp_stopped。
 * 主循环和派发路径在关键步骤前调用它。
 */
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
 * error 是 GPGPU_QUEUE_ERROR_* 编号。处理分三步：
 *
 * 1. 清除 ACTIVE，cp_next_packet() 此后不再从该槽取包；
 * 2. HQD_ERROR 为 0 时写入本次错误并提交一条 QUEUE_ERROR，驱动据此让
 *    相关等待失败；已有错误时保持首次错误，不重复通知；
 * 3. KIQ 是全部管理命令的入口，KIQ 出错时置 cp_stopped 停止全部调度。
 *
 * 槽的占用和 MQD 绑定保留，等 Host 卸载时再保存错误并释放。
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

/*
 * 固件自身出现无法恢复的异常（启动检查失败或非预期 trap）时调用：
 * 先置 cp_stopped，再尝试提交一条 DEVICE_ERROR（设备随之锁存 FIRMWARE
 * 错误），最后停在 WFI，不再访问任何 Host 对象。
 */
void cp_fatal(void)
{
	cp_stopped = 1;
	cp_ih_submit(~0u, GPGPU_IH_DEVICE_ERROR, GPGPU_ERR_FIRMWARE);
	for (;;)
		__asm__ volatile("wfi");
}
