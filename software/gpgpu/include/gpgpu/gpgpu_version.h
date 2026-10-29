/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：版本号
 *
 * 设备、固件、驱动和 Kernel 描述对象各自核对一个版本值。四个值当前都是 2，
 * 组件必须一起更新。
 */
#ifndef GPGPU_VERSION_H
#define GPGPU_VERSION_H

/* BAR0 DEV_VERSION 读回的设备版本编码，高 16 位为主版本。 */
#define GPGPU_DEV_VERSION_VALUE 0x00020000
/* 统一访存协议版本：设备在 VM_VERSION 报告，固件启动时写回 VM_FIRMWARE 握手。 */
#define GPGPU_VM_VERSION 2U
/* Kernel 描述对象 kernel_abi_version 字段要求的值：启动 CSR 与真实参数指针。 */
#define GPGPU_KERNEL_ABI_VERSION 2U

#endif /* GPGPU_VERSION_H */
