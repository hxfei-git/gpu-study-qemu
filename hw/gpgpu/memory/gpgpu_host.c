// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemu/osdep.h"
#include "gpgpu_host.h"
#include "hw/remote/vfio-user-obj.h"
#include "hw/pci/pci_device.h"

MemTxResult gpgpu_host_access(GPGPUState *s, uint64_t address, void *data, size_t length,
			      bool is_write)
{
	/* 由既有 PCI/vfio-user 路径处理系统 IOMMU 和跨进程访问。 */
	return vfu_object_dma_rw(PCI_DEVICE(s), address, data, length, is_write);
}

bool gpgpu_host_read(GPGPUState *s, uint64_t address, void *data, size_t length)
{
	return gpgpu_host_access(s, address, data, length, false) == MEMTX_OK;
}
