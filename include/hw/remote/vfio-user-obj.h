#ifndef VFIO_USER_OBJ_H
#define VFIO_USER_OBJ_H

#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"

void vfu_object_set_bus_irq(PCIBus *pci_bus);
MemTxResult vfu_object_dma_rw(PCIDevice *dev, dma_addr_t addr, void *buf,
                             dma_addr_t len, bool is_write);

#endif
