/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

/* 启动寄存器访问、命令检查与同步派发。 */

static void gpgpu_dispatch_kernel(GPGPUState *s)
{
    int ret;

    if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) ||
        (s->global_status & GPGPU_STATUS_BUSY) ||
        !s->kernel.grid_dim[0] || !s->kernel.grid_dim[1] ||
        !s->kernel.grid_dim[2] || !s->kernel.block_dim[0] ||
        !s->kernel.block_dim[1] || !s->kernel.block_dim[2] ||
        s->kernel.shared_mem_size ||
        !gpgpu_vram_word_valid(s, s->kernel.kernel_addr) ||
        !gpgpu_vram_word_valid(s, s->kernel.kernel_args)) {
        gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
        return;
    }

    s->global_status |= GPGPU_STATUS_BUSY;
    ret = gpgpu_core_exec_kernel(s);
    s->global_status &= ~GPGPU_STATUS_BUSY;
    s->global_status |= GPGPU_STATUS_READY;

    if (ret < 0) {
        gpgpu_set_error(s, GPGPU_ERR_KERNEL_FAULT);
    } else {
        gpgpu_raise_irq(s, GPGPU_IRQ_KERNEL_DONE);
    }
}

uint32_t gpgpu_kernel_read(GPGPUState *s, hwaddr addr)
{
    switch (addr) {
    case GPGPU_REG_KERNEL_ADDR_LO:
        return (uint32_t)s->kernel.kernel_addr;
    case GPGPU_REG_KERNEL_ADDR_HI:
        return (uint32_t)(s->kernel.kernel_addr >> 32);
    case GPGPU_REG_KERNEL_ARGS_LO:
        return (uint32_t)s->kernel.kernel_args;
    case GPGPU_REG_KERNEL_ARGS_HI:
        return (uint32_t)(s->kernel.kernel_args >> 32);
    case GPGPU_REG_GRID_DIM_X:
        return s->kernel.grid_dim[0];
    case GPGPU_REG_GRID_DIM_Y:
        return s->kernel.grid_dim[1];
    case GPGPU_REG_GRID_DIM_Z:
        return s->kernel.grid_dim[2];
    case GPGPU_REG_BLOCK_DIM_X:
        return s->kernel.block_dim[0];
    case GPGPU_REG_BLOCK_DIM_Y:
        return s->kernel.block_dim[1];
    case GPGPU_REG_BLOCK_DIM_Z:
        return s->kernel.block_dim[2];
    case GPGPU_REG_SHARED_MEM_SIZE:
        return s->kernel.shared_mem_size;
    default:
        return 0;
    }
}

void gpgpu_kernel_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case GPGPU_REG_KERNEL_ADDR_LO:
        s->kernel.kernel_addr = deposit64(s->kernel.kernel_addr, 0, 32, val);
        break;
    case GPGPU_REG_KERNEL_ADDR_HI:
        s->kernel.kernel_addr = deposit64(s->kernel.kernel_addr, 32, 32, val);
        break;
    case GPGPU_REG_KERNEL_ARGS_LO:
        s->kernel.kernel_args = deposit64(s->kernel.kernel_args, 0, 32, val);
        break;
    case GPGPU_REG_KERNEL_ARGS_HI:
        s->kernel.kernel_args = deposit64(s->kernel.kernel_args, 32, 32, val);
        break;
    case GPGPU_REG_GRID_DIM_X:
        s->kernel.grid_dim[0] = val;
        break;
    case GPGPU_REG_GRID_DIM_Y:
        s->kernel.grid_dim[1] = val;
        break;
    case GPGPU_REG_GRID_DIM_Z:
        s->kernel.grid_dim[2] = val;
        break;
    case GPGPU_REG_BLOCK_DIM_X:
        s->kernel.block_dim[0] = val;
        break;
    case GPGPU_REG_BLOCK_DIM_Y:
        s->kernel.block_dim[1] = val;
        break;
    case GPGPU_REG_BLOCK_DIM_Z:
        s->kernel.block_dim[2] = val;
        break;
    case GPGPU_REG_SHARED_MEM_SIZE:
        s->kernel.shared_mem_size = val;
        break;
    case GPGPU_REG_DISPATCH:
        gpgpu_dispatch_kernel(s);
        break;
    default:
        break;
    }
}
