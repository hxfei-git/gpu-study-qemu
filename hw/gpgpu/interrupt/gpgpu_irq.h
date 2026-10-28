/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 集中管理 PCI 中断能力与事件锁存，确认后按 IH 状态决定重新通知。 */
#ifndef HW_INTERRUPT_GPGPU_IRQ_H
#define HW_INTERRUPT_GPGPU_IRQ_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "qemu/typedefs.h"

/* 设备暴露的 MSI-X 向量总数。 */
#define GPGPU_MSIX_VECTORS 4
/* 向量 0 保留，计算完成统一经 IH 向量通知。 */
/* DMA 完成使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_DMA 1
/* 设备错误使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_ERROR 2
/* IH Ring 非空通知使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_IH 3

/**
 * gpgpu_irq_read() - 读取中断掩码或锁存状态
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 *
 * Return: 寄存器值；未定义偏移返回零。
 */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_irq_write() - 更新使能或确认已处理事件
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 * @val: 本次写入的寄存器值。
 */
void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val);

/**
 * gpgpu_raise_irq() - 先锁存事件再发送 PCI 中断
 * @s: 设备实例；调用期间须保持设备存活。
 * @events: 可组合的设备事件位。
 */
void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events);

/**
 * gpgpu_irq_init() - 建立 PCI 中断能力
 * @s: 设备实例；调用期间须保持设备存活。
 * @errp: 建立失败时返回错误原因。
 *
 * Return: 成功为 true；失败时撤销已建立的中断能力。
 */
bool gpgpu_irq_init(GPGPUState *s, Error **errp);

/**
 * gpgpu_irq_cleanup() - 释放 MSI 和 MSI-X 能力
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_irq_cleanup(GPGPUState *s);

/**
 * gpgpu_irq_reset() - 复位 PCI 中断能力状态
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_irq_reset(GPGPUState *s);

/**
 * gpgpu_irq_clear() - 关闭使能并清除事件锁存及 INTx
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_irq_clear(GPGPUState *s);

/**
 * struct GPGPUIRQState - IRQ 模块维护的事件状态
 * @enable: 客体设置的事件使能掩码。
 * @status: 待确认的设备事件位。
 */
typedef struct GPGPUIRQState {
	GPGPUIrq enable;
	GPGPUIrq status;
} GPGPUIRQState;

#endif /* HW_INTERRUPT_GPGPU_IRQ_H */
