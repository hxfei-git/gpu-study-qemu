/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 分发 Host 与 CP 寄存器访问。 */
#ifndef HW_GPGPU_MMIO_H
#define HW_GPGPU_MMIO_H

#include "gpgpu.h"
#include "system/memory.h"

/* BAR0 和 CP 控制窗口的回调，opaque 均为设备实例。 */
extern const MemoryRegionOps gpgpu_ctrl_ops;
extern const MemoryRegionOps gpgpu_cp_regs_ops;

#endif /* HW_GPGPU_MMIO_H */
