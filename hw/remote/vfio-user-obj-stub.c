#include "qemu/osdep.h"
#include "hw/remote/vfio-user-obj.h"

MemTxResult vfu_object_dma_rw(PCIDevice *dev, dma_addr_t addr, void *buf,
                             dma_addr_t len, bool is_write)
{
    DMADirection dir = is_write ? DMA_DIRECTION_FROM_DEVICE :
                                   DMA_DIRECTION_TO_DEVICE;

    return pci_dma_rw(dev, addr, buf, len, dir, MEMTXATTRS_UNSPECIFIED);
}

void vfu_object_set_bus_irq(PCIBus *pci_bus)
{
}
