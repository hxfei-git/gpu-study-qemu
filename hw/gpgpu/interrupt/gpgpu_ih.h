/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IH 管理 Host 记录 Ring，先写记录、再发布进度、最后通知 IRQ。 */
#ifndef HW_INTERRUPT_GPGPU_IH_H
#define HW_INTERRUPT_GPGPU_IH_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"

/**
 * struct GPGPUIHState - 设备共用的 Host RAM 中断记录 Ring
 * @base: Ring 的完整 Host DMA 基址，按 4 KiB 对齐。
 * @size: Ring 的字节容量，必须为 2 的幂。
 * @wptr: 设备下一条待写记录的 32 位编号。
 * @rptr: Host 下一条待消费记录的 32 位编号。
 * @ctrl: IH_ENABLE 使能位；RESET 是命令位，不锁存。
 * @status: 忙、首次故障以及最近一次提交结果的组合位域。
 */
typedef struct GPGPUIHState {
	uint64_t base;
	uint32_t size;
	uint32_t wptr;
	uint32_t rptr;
	uint32_t ctrl;
	uint32_t status;
} GPGPUIHState;

/**
 * gpgpu_ih_read() - 读取 IH 配置、进度与故障
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 *
 * Return: 寄存器值；读取不清除故障。
 */
uint32_t gpgpu_ih_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_ih_write() - 处理 Host 配置或 CP 记录提交
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 * @val: 本次写入的寄存器值。
 * @from_cp: CP 仅允许提交记录，Host 按配置权限处理。
 */
void gpgpu_ih_write(GPGPUState *s, hwaddr addr, uint32_t val, bool from_cp);

/**
 * gpgpu_ih_refresh_irq() - 为未消费记录重新请求 IH 中断
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_ih_refresh_irq(GPGPUState *s);

/**
 * gpgpu_ih_pending() - 查询 IH 是否仍有未消费记录
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: 写指针与读指针不同时为 true。
 */
bool gpgpu_ih_pending(GPGPUState *s);

/**
 * gpgpu_ih_faulted() - 查询 IH 通道首次故障是否锁存
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: 已锁存通道故障时为 true。
 */
bool gpgpu_ih_faulted(GPGPUState *s);

/**
 * gpgpu_ih_reset() - 清零 IH 配置、进度及故障
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_ih_reset(GPGPUState *s);

#endif /* HW_INTERRUPT_GPGPU_IH_H */
