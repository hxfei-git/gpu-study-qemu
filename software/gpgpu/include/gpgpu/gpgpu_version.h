/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：版本号
 *
 * 设备、固件、驱动、用户库和 Kernel 描述对象共用一个协议版本，组件必须一起
 * 更新。各方在以下位置核对它：
 *
 *   设备  BAR0 DEV_VERSION 高 16 位、VM_VERSION
 *   固件  启动时读 VM_VERSION，相同则写回 VM_FIRMWARE 完成握手
 *   驱动  probe 核对 VM_VERSION，启动后核对 VM_FIRMWARE；DRM driver major
 *   用户  每个 ioctl 参数的 version 字段；INFO 返回的 version
 *   Kernel 描述对象的 kernel_abi_version
 */
#ifndef GPGPU_VERSION_H
#define GPGPU_VERSION_H

/* 协议版本。 */
#define GPGPU_PROTOCOL_VERSION 3U
/* BAR0 DEV_VERSION 读回的值：高 16 位为协议版本，低 16 位为修订号。 */
#define GPGPU_DEV_VERSION_VALUE (GPGPU_PROTOCOL_VERSION << 16)
/* Kernel 描述对象 kernel_abi_version 字段要求的值。 */
#define GPGPU_KERNEL_ABI_VERSION GPGPU_PROTOCOL_VERSION

#endif /* GPGPU_VERSION_H */
