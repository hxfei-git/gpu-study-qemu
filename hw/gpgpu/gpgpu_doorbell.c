/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU Educational GPGPU Device
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu.h"

/*
 * BAR4 is reserved: reads return zero, writes have no effect.
 * Queue submission will be implemented here in the next experiment stage.
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
