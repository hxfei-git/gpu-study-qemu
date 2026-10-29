/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：IH 通知记录
 *
 * CP 在固件私有 VRAM 中准备一条记录，写 IH_SUBMIT 交给设备；设备把记录
 * 写入 Host RAM 中的 IH Ring，再发布 IH_WPTR，最后发 GPGPU_IRQ_IH。
 * 驱动按 mqd_addr 找到队列，再按 data 处理事件或错误。
 */
#ifndef GPGPU_IH_ABI_H
#define GPGPU_IH_ABI_H

#include "gpgpu_abi_types.h"

/**
 * enum GpgpuIhType - IH 记录类型，决定 data 和来源字段的含义
 * @GPGPU_IH_SIGNAL_NOTIFY: 用户任务完成；data 为事件编号。
 * @GPGPU_IH_QUEUE_ERROR: 队列首次出错；data 为 GpgpuQueueError。
 * @GPGPU_IH_DEVICE_ERROR: 固件级错误；data 为 GPGPU_ERR_* 位，hqd_slot 全 1，mqd_addr 为 0。
 */
enum GpgpuIhType {
	GPGPU_IH_SIGNAL_NOTIFY = 1,
	GPGPU_IH_QUEUE_ERROR = 2,
	GPGPU_IH_DEVICE_ERROR = 3,
};

/**
 * enum GpgpuIhError - IH_STATUS 中首次故障码与最近提交结果的取值
 * @GPGPU_IH_OK: 提交成功，或尚无故障。
 * @GPGPU_IH_ERR_DISABLED: 通道未使能。
 * @GPGPU_IH_ERR_CONFIG: Ring 基址或容量非法。
 * @GPGPU_IH_ERR_FULL: Host 尚未归还空槽。
 * @GPGPU_IH_ERR_DMA: 写 Host Ring 失败。
 * @GPGPU_IH_ERR_PROTOCOL: 寄存器访问的权限、宽度或状态转换非法。
 * @GPGPU_IH_ERR_RECORD: 记录不在固件私有 VRAM，或内容非法。
 * @GPGPU_IH_ERR_FAULTED: 通道已故障，拒绝新的提交。
 */
enum GpgpuIhError {
	GPGPU_IH_OK,
	GPGPU_IH_ERR_DISABLED,
	GPGPU_IH_ERR_CONFIG,
	GPGPU_IH_ERR_FULL,
	GPGPU_IH_ERR_DMA,
	GPGPU_IH_ERR_PROTOCOL,
	GPGPU_IH_ERR_RECORD,
	GPGPU_IH_ERR_FAULTED,
};

/**
 * struct GpgpuIhRecord - 32 字节通知记录
 * @type: GpgpuIhType。
 * @hqd_slot: 来源 HQD 槽号；设备错误为全 1。
 * @mqd_addr: 来源队列 MQD 的 GPU 物理地址；设备错误为 0。
 * @data: 事件编号、队列错误或设备错误位，由 type 决定。
 * @reserved: 必须为零。
 */
typedef struct GpgpuIhRecord {
	gpgpu_q_u32 type;
	gpgpu_q_u32 hqd_slot;
	gpgpu_q_u64 mqd_addr;
	gpgpu_q_u32 data;
	gpgpu_q_u32 reserved[3];
} GpgpuIhRecord;

GPGPU_LAYOUT_ASSERT(GpgpuIhRecord, 32, data, 0x10);

#endif /* GPGPU_IH_ABI_H */
