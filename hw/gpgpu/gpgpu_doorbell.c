/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu.h"

/*
 * BAR4 目前预留：读取返回零，写入不产生作用。
 * 下一实验阶段将在此实现队列提交。
 */

static uint64_t gpgpu_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void gpgpu_doorbell_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}

const MemoryRegionOps gpgpu_doorbell_ops = {
    .read = gpgpu_doorbell_read,
    .write = gpgpu_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};
