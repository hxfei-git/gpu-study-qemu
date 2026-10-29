/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 中断：事件锁存与 PCI 通知
 *
 * 各模块调用 gpgpu_raise_irq() 锁存事件；本模块按 IRQ_ENABLE 决定是否通知，
 * 优先 MSI-X（每类事件一个向量），其次 MSI，最后 INTx 电平。
 */
#ifndef HW_GPGPU_IRQ_IRQ_H
#define HW_GPGPU_IRQ_IRQ_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"

/**
 * struct GPGPUIRQState - 中断状态
 * @enable: IRQ_ENABLE，只保留 GPGPU_IRQ_ALL 中的位。
 * @status: IRQ_STATUS，已锁存待确认的事件。
 */
typedef struct GPGPUIRQState {
	uint32_t enable;
	uint32_t status;
} GPGPUIRQState;

/* 锁存 GPGPU_IRQ_* 事件，再按使能状态通知 Host。 */
void gpgpu_raise_irq(GPGPUState *s, uint32_t events);

/* 中断寄存器组（BAR0 0x200）。 */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr reg);
void gpgpu_irq_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 建立 MSI-X（表与 PBA 位于 BAR0 末尾）和 MSI 能力。 */
bool gpgpu_irq_init(GPGPUState *s, Error **errp);
void gpgpu_irq_cleanup(GPGPUState *s);

/* 寄存器复位：关闭使能、清除锁存并撤销 INTx。 */
void gpgpu_irq_reset(GPGPUState *s);

/* QOM 复位：复位 PCI 的 MSI/MSI-X 能力状态。 */
void gpgpu_irq_reset_pci(GPGPUState *s);

#endif /* HW_GPGPU_IRQ_IRQ_H */
