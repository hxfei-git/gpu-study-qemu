// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 中断：事件锁存、确认与 PCI 通知
 */
#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "irq.h"
#include "ih.h"

/* MSI-X 表与 PBA 在 BAR0 内的偏移，位于寄存器区之后。 */
#define MSIX_TABLE_OFFSET 0xfe000
#define MSIX_PBA_OFFSET 0xff000

/*
 * irq_update() - 按使能状态通知 Host
 * @events: 本次新增或新使能的事件；为 0 时只刷新 INTx 电平。
 *
 * INTx 电平反映全部待处理事件；MSI-X/MSI 只对新事件发消息。
 */
static void irq_update(GPGPUState *s, uint32_t events)
{
	PCIDevice *pdev = PCI_DEVICE(s);
	uint32_t pending = s->irq.status & s->irq.enable;

	events &= pending;
	pci_set_irq(pdev, pending && !msix_enabled(pdev) && !msi_enabled(pdev));
	if (msix_enabled(pdev)) {
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

void gpgpu_raise_irq(GPGPUState *s, uint32_t events)
{
	s->irq.status |= events;
	irq_update(s, events);
}

uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr reg)
{
	switch (reg) {
	case GPGPU_REG_IRQ_ENABLE:
		return s->irq.enable;
	case GPGPU_REG_IRQ_STATUS:
		return s->irq.status;
	default:
		return 0;
	}
}

/*
 * ENABLE：新使能 IH 时若 Ring 中已有记录，补锁存 IH 事件；新使能的事件立即通知。
 * ACK：写 1 清除；清除 IH 后若仍有未消费记录，重新锁存并通知。
 */
void gpgpu_irq_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	uint32_t old;

	switch (reg) {
	case GPGPU_REG_IRQ_ENABLE:
		old = s->irq.enable;
		s->irq.enable = value & GPGPU_IRQ_ALL;
		if ((s->irq.enable & GPGPU_IRQ_IH) && gpgpu_ih_pending(s))
			s->irq.status |= GPGPU_IRQ_IH;
		irq_update(s, s->irq.enable & ~old);
		break;
	case GPGPU_REG_IRQ_ACK:
		s->irq.status &= ~value;
		irq_update(s, 0);
		if ((value & GPGPU_IRQ_IH) && gpgpu_ih_pending(s))
			gpgpu_raise_irq(s, GPGPU_IRQ_IH);
		break;
	default:
		break;
	}
}

bool gpgpu_irq_init(GPGPUState *s, Error **errp)
{
	PCIDevice *pdev = PCI_DEVICE(s);

	if (msix_init(pdev, GPGPU_MSIX_VECTORS, &s->bar0, 0, MSIX_TABLE_OFFSET, &s->bar0, 0,
		      MSIX_PBA_OFFSET, 0, errp))
		return false;
	if (msi_init(pdev, 0, 1, true, false, errp)) {
		msix_uninit(pdev, &s->bar0, &s->bar0);
		return false;
	}
	for (unsigned int i = 0; i < GPGPU_MSIX_VECTORS; i++)
		msix_vector_use(pdev, i);
	return true;
}

void gpgpu_irq_cleanup(GPGPUState *s)
{
	msix_uninit(PCI_DEVICE(s), &s->bar0, &s->bar0);
	msi_uninit(PCI_DEVICE(s));
}

void gpgpu_irq_reset(GPGPUState *s)
{
	s->irq = (GPGPUIRQState) { 0 };
	pci_set_irq(PCI_DEVICE(s), 0);
}

void gpgpu_irq_reset_pci(GPGPUState *s)
{
	msix_reset(PCI_DEVICE(s));
	msi_reset(PCI_DEVICE(s));
}
