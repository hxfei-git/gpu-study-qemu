/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PCI 教学设备的 QOM 类型与全局控制接口。 */
#ifndef HW_GPGPU_H
#define HW_GPGPU_H

#include "qom/object.h"
#include "hw/pci/pci_ids.h"
#include "gpgpu/gpgpu_protocol.h"

/* QOM 注册名，供命令行 -device 与类型转换使用。 */
#define TYPE_GPGPU "gpgpu"
OBJECT_DECLARE_SIMPLE_TYPE(GPGPUState, GPGPU)

/* PCI 修订版本号。 */
#define GPGPU_REVISION 0x01
/* PCI 设备类别，声明为 3D 控制器。 */
#define GPGPU_CLASS_CODE PCI_CLASS_DISPLAY_3D

/* BAR2 默认显存字节容量。 */
#define GPGPU_VRAM_BAR_SIZE (64 * 1024 * 1024)
/* DEV_CAPS 默认报告的计算单元数。 */
#define GPGPU_DEFAULT_NUM_CUS 4
/* DEV_CAPS 默认报告的每计算单元 warp 数。 */
#define GPGPU_DEFAULT_WARPS_PER_CU 4
/* 默认每 warp 的 lane 数，解释器固定支持 32。 */
#define GPGPU_DEFAULT_WARP_SIZE 32
/* QOM 未指定 vram_size 时使用的字节容量。 */
#define GPGPU_DEFAULT_VRAM_SIZE GPGPU_VRAM_BAR_SIZE

/**
 * gpgpu_set_error() - 累积全局错误并通知 Host
 * @s: 设备实例；调用期间须保持设备存活。
 * @error: 要锁存的错误位。
 *
 * 先更新错误和全局状态，再请求错误中断。
 */
void gpgpu_set_error(GPGPUState *s, uint32_t error);

/**
 * gpgpu_reset_state() - 停止执行并复位设备寄存器
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 由设备生命周期或全局 RESET 写入调用。此入口保留 VRAM 和 PCI 中断能力。
 */
void gpgpu_reset_state(GPGPUState *s);

#endif /* HW_GPGPU_H */
