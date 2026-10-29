/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IH：设备写入 Host RAM 的通知记录 Ring
 *
 * 一条通知的路径：
 *
 *   CP 在固件私有 VRAM 写好 GpgpuIhRecord
 *     → CP 写 IH_SUBMIT（记录地址）
 *     → 设备检查记录，写入 Host Ring 的 W 号槽
 *     → 设备发布 W+1
 *     → 设备锁存 GPGPU_IRQ_IH 并通知 Host
 *     → 驱动消费记录后写回 IH_RPTR
 *
 * 任何一步失败都锁存首次故障，此后拒绝新的提交。
 */
#ifndef HW_GPGPU_IRQ_IH_H
#define HW_GPGPU_IRQ_IH_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"

/**
 * struct GPGPUIHState - IH Ring 状态
 * @base: Ring 的 GPU 物理基址，4 KiB 对齐。
 * @size: Ring 字节数，2 的幂，至少一条记录。
 * @wptr: 设备下一条待写记录的 32 位编号。
 * @rptr: Host 下一条待消费记录的 32 位编号。
 * @ctrl: IH_CTRL 中锁存的 ENABLE 位。
 * @status: IH_STATUS：BUSY、FAULT、首次故障码与最近一次提交结果。
 */
typedef struct GPGPUIHState {
	uint64_t base;
	uint32_t size;
	uint32_t wptr;
	uint32_t rptr;
	uint32_t ctrl;
	uint32_t status;
} GPGPUIHState;

/* IH 寄存器组读取（Host 与 CP 视图相同）。 */
uint32_t gpgpu_ih_read(GPGPUState *s, hwaddr reg);

/* Host 写入：Ring 配置、RPTR 与 CTRL；非法写入锁存 PROTOCOL 故障。 */
void gpgpu_ih_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* CP 写入：只接受 IH_SUBMIT，其余锁存 PROTOCOL 故障。 */
void gpgpu_ih_cp_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 访问宽度或对齐非法时由 BAR0 转发层调用，锁存 PROTOCOL 故障。 */
void gpgpu_ih_protocol_fault(GPGPUState *s);

/* Ring 中是否有未消费记录。 */
bool gpgpu_ih_pending(GPGPUState *s);

/* 是否已锁存通道故障；故障后执行器拒绝新派发。 */
bool gpgpu_ih_faulted(GPGPUState *s);

/* 是否正在写入一条记录。 */
bool gpgpu_ih_busy(GPGPUState *s);

void gpgpu_ih_reset(GPGPUState *s);

#endif /* HW_GPGPU_IRQ_IH_H */
