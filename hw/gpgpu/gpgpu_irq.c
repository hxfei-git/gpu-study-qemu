/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "gpgpu.h"

/*
 * gpgpu_update_irq() - 按使能状态发送 PCI 中断
 *
 * 锁存位决定 INTx 电平，新增或重新使能的事件触发 MSI/MSI-X 消息。
 */
static void gpgpu_update_irq(GPGPUState *s, GPGPUIrq events)
{
	PCIDevice *pdev = PCI_DEVICE(s);
	GPGPUIrq pending = s->irq_status & s->irq_enable;

	events &= pending;
	pci_set_irq(pdev, pending && !msix_enabled(pdev) && !msi_enabled(pdev));
	if (msix_enabled(pdev)) {
		if (events & GPGPU_IRQ_KERNEL_DONE)
			msix_notify(pdev, GPGPU_MSIX_VEC_KERNEL);
		if (events & GPGPU_IRQ_DMA_DONE)
			msix_notify(pdev, GPGPU_MSIX_VEC_DMA);
		if (events & GPGPU_IRQ_IH)
			msix_notify(pdev, GPGPU_MSIX_VEC_IH);
		if (events & GPGPU_IRQ_ERROR)
			msix_notify(pdev, GPGPU_MSIX_VEC_ERROR);
	} else if (msi_enabled(pdev) && events) {
		msi_notify(pdev, 0);
	}
}

/*
 * gpgpu_raise_irq() - 锁存设备事件并通知 PCI 中断层
 *
 * 先保存 IRQ_STATUS，再根据使能掩码发送通知。
 */
void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events)
{
	s->irq_status |= events;
	gpgpu_update_irq(s, events);
}

/*
 * gpgpu_set_error() - 保存全局错误并发出错误事件
 *
 * 错误位按位累积，同时设置 GLOBAL_STATUS 的 ERROR 标志。
 */
void gpgpu_set_error(GPGPUState *s, GPGPUError error)
{
	s->error_status |= error;
	s->global_status |= GPGPU_STATUS_ERROR;
	gpgpu_raise_irq(s, GPGPU_IRQ_ERROR);
}

/*
 * gpgpu_irq_read() - 读回中断使能与挂起状态
 *
 * 未定义的中断寄存器偏移返回零。
 */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr addr)
{
	switch (addr) {
	case GPGPU_REG_IRQ_ENABLE:
		return s->irq_enable;
	case GPGPU_REG_IRQ_STATUS:
		return s->irq_status;
	default:
		return 0;
	}
}

/*
 * gpgpu_irq_write() - 更新中断使能或确认已处理事件
 *
 * ACK 按写 1 清除；IH 尚有未消费记录时会再次通知。
 */
void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	switch (addr) {
	case GPGPU_REG_IRQ_ENABLE: {
		GPGPUIrq old = s->irq_enable;

		s->irq_enable = val &
			(GPGPU_IRQ_KERNEL_DONE | GPGPU_IRQ_DMA_DONE | GPGPU_IRQ_ERROR |
			 GPGPU_IRQ_IH);
		if ((s->irq_enable & GPGPU_IRQ_IH) && s->ih.wptr != s->ih.rptr)
			s->irq_status |= GPGPU_IRQ_IH;
		gpgpu_update_irq(s, s->irq_enable & ~old);
		break;
	}
	case GPGPU_REG_IRQ_ACK:
		s->irq_status &= ~val;
		gpgpu_update_irq(s, 0);
		if (val & GPGPU_IRQ_IH)
			gpgpu_ih_refresh_irq(s);
		break;
	default:
		break;
	}
}

/*
 * gpgpu_irq_clear() - 清除软件锁存的中断状态
 *
 * 同时关闭使能并撤销 INTx 电平，不重新创建 PCI 能力。
 */
void gpgpu_irq_clear(GPGPUState *s)
{
	s->irq_enable = 0;
	s->irq_status = 0;
	pci_set_irq(PCI_DEVICE(s), 0);
}

/*
 * gpgpu_irq_init() - 建立 MSI-X 和 MSI 能力
 *
 * 分配向量与 BAR0 表空间，失败时撤销已建立的能力并返回 false。
 */
bool gpgpu_irq_init(GPGPUState *s, Error **errp)
{
	PCIDevice *pdev = PCI_DEVICE(s);

	if (msix_init(pdev, GPGPU_MSIX_VECTORS, &s->ctrl_mmio, 0, 0xFE000, &s->ctrl_mmio, 0,
		      0xFF000, 0, errp)) {
		return false;
	}
	if (msi_init(pdev, 0, 1, true, false, errp)) {
		msix_uninit(pdev, &s->ctrl_mmio, &s->ctrl_mmio);
		return false;
	}
	for (unsigned int i = 0; i < GPGPU_MSIX_VECTORS; i++)
		msix_vector_use(pdev, i);
	return true;
}

/*
 * gpgpu_irq_cleanup() - 撤销 PCI 中断能力
 *
 * 设备退出时释放 MSI-X 表及 MSI 状态。
 */
void gpgpu_irq_cleanup(GPGPUState *s)
{
	msix_uninit(PCI_DEVICE(s), &s->ctrl_mmio, &s->ctrl_mmio);
	msi_uninit(PCI_DEVICE(s));
}

/*
 * gpgpu_irq_reset() - 复位 PCI 中断能力状态
 *
 * 沿用 QEMU 的 MSI/MSI-X 复位辅助函数。
 */
void gpgpu_irq_reset(GPGPUState *s)
{
	msix_reset(PCI_DEVICE(s));
	msi_reset(PCI_DEVICE(s));
}
