/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议总入口
 *
 * 设备模型和固件需要完整协议时包含本文件；只需部分内容的一方直接包含
 * 对应子头文件。用户态只应包含 uapi/gpgpu_drm.h 和 gpgpu_queue_abi.h。
 */
#ifndef GPGPU_PROTOCOL_H
#define GPGPU_PROTOCOL_H

#include "gpgpu_version.h"
#include "gpgpu_regs.h"
#include "gpgpu_cp_map.h"
#include "gpgpu_vm_abi.h"
#include "gpgpu_queue_abi.h"
#include "gpgpu_ih_abi.h"

#endif /* GPGPU_PROTOCOL_H */
