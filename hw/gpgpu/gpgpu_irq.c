/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "gpgpu.h"

/* 设备事件锁存、使能、确认与 PCI 中断发送。 */

void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events)
{
    s->irq_status |= events;
}

void gpgpu_set_error(GPGPUState *s, GPGPUError error)
{
    s->error_status |= error;
    s->global_status |= GPGPU_STATUS_ERROR;
}

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

void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case GPGPU_REG_IRQ_ENABLE:
        s->irq_enable = val & (GPGPU_IRQ_KERNEL_DONE |
                              GPGPU_IRQ_DMA_DONE |
                              GPGPU_IRQ_ERROR);
        break;
    case GPGPU_REG_IRQ_ACK:
        s->irq_status &= ~val;
        break;
    default:
        break;
    }
}

void gpgpu_irq_clear(GPGPUState *s)
{
    s->irq_enable = 0;
    s->irq_status = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

bool gpgpu_irq_init(GPGPUState *s, Error **errp)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    if (msix_init(pdev, GPGPU_MSIX_VECTORS,
                  &s->ctrl_mmio, 0, 0xFE000,
                  &s->ctrl_mmio, 0, 0xFF000, 0, errp)) {
        return false;
    }
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        msix_uninit(pdev, &s->ctrl_mmio, &s->ctrl_mmio);
        return false;
    }
    return true;
}

void gpgpu_irq_cleanup(GPGPUState *s)
{
    msix_uninit(PCI_DEVICE(s), &s->ctrl_mmio, &s->ctrl_mmio);
    msi_uninit(PCI_DEVICE(s));
}
