/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#ifndef HW_GPGPU_REGS_H
#define HW_GPGPU_REGS_H

/*
 * BAR0 ABI：使用字节偏移，寄存器为小端 32 位。
 * 未实现的偏移读取返回零，写入忽略。
 */

/* 设备信息：只读。 */
#define GPGPU_REG_DEV_ID            0x0000  /* 设备标识符 */
#define GPGPU_REG_DEV_VERSION       0x0004  /* 版本号 */
#define GPGPU_REG_DEV_CAPS          0x0008  /* 设备能力 */
#define GPGPU_REG_VRAM_SIZE_LO      0x000C  /* 显存大小低 32 位 */
#define GPGPU_REG_VRAM_SIZE_HI      0x0010  /* 显存大小高 32 位 */

/*
 * GLOBAL_CTRL：读写；GLOBAL_STATUS：只读；
 * ERROR_STATUS：写 1 清除。
 */
#define GPGPU_REG_GLOBAL_CTRL       0x0100  /* 全局控制 (使能/复位) */
#define GPGPU_REG_GLOBAL_STATUS     0x0104  /* 只读状态位 */
#define GPGPU_REG_ERROR_STATUS      0x0108  /* 错误状态 (写 1 清除) */

/* IRQ_ENABLE：读写；IRQ_STATUS：只读；IRQ_ACK：写 1 清除。 */
#define GPGPU_REG_IRQ_ENABLE        0x0200  /* 中断使能掩码 */
#define GPGPU_REG_IRQ_STATUS        0x0204  /* 中断状态 (挂起的中断) */
#define GPGPU_REG_IRQ_ACK           0x0208  /* 中断确认 (写 1 清除) */

/* 启动描述符可读写；DISPATCH 只写。 */
#define GPGPU_REG_KERNEL_ADDR_LO    0x0300  /* 内核代码地址低 32 位 */
#define GPGPU_REG_KERNEL_ADDR_HI    0x0304  /* 内核代码地址高 32 位 */
#define GPGPU_REG_KERNEL_ARGS_LO    0x0308  /* 内核参数地址低 32 位 */
#define GPGPU_REG_KERNEL_ARGS_HI    0x030C  /* 内核参数地址高 32 位 */
#define GPGPU_REG_GRID_DIM_X        0x0310  /* Grid X 维度 (Block 数量) */
#define GPGPU_REG_GRID_DIM_Y        0x0314  /* Grid Y 维度 */
#define GPGPU_REG_GRID_DIM_Z        0x0318  /* Grid Z 维度 */
#define GPGPU_REG_BLOCK_DIM_X       0x031C  /* Block X 维度 (线程数量) */
#define GPGPU_REG_BLOCK_DIM_Y       0x0320  /* Block Y 维度 */
#define GPGPU_REG_BLOCK_DIM_Z       0x0324  /* Block Z 维度 */
#define GPGPU_REG_SHARED_MEM_SIZE   0x0328  /* 每 block 字节数 */
#define GPGPU_REG_DISPATCH          0x0330  /* 写任意值启动内核执行 */

/* DMA 描述符和控制寄存器可读写；DMA_STATUS 只读。 */
#define GPGPU_REG_DMA_SRC_LO        0x0400  /* DMA 源地址低 32 位 */
#define GPGPU_REG_DMA_SRC_HI        0x0404  /* DMA 源地址高 32 位 */
#define GPGPU_REG_DMA_DST_LO        0x0408  /* DMA 目标地址低 32 位 */
#define GPGPU_REG_DMA_DST_HI        0x040C  /* DMA 目标地址高 32 位 */
#define GPGPU_REG_DMA_SIZE          0x0410  /* 传输大小 (字节) */
#define GPGPU_REG_DMA_CTRL          0x0414  /* DMA 控制寄存器 */
#define GPGPU_REG_DMA_STATUS        0x0418  /* DMA 状态寄存器 */

/* 可读写的教学寄存器，与 RV32 解释器控制窗口分开。 */
#define GPGPU_REG_THREAD_ID_X       0x1000  /* threadIdx.x */
#define GPGPU_REG_THREAD_ID_Y       0x1004  /* threadIdx.y */
#define GPGPU_REG_THREAD_ID_Z       0x1008  /* threadIdx.z */
#define GPGPU_REG_BLOCK_ID_X        0x1010  /* Block 在 Grid 中的 X 索引 */
#define GPGPU_REG_BLOCK_ID_Y        0x1014  /* Block 在 Grid 中的 Y 索引 */
#define GPGPU_REG_BLOCK_ID_Z        0x1018  /* Block 在 Grid 中的 Z 索引 */
#define GPGPU_REG_WARP_ID           0x1020  /* Warp 在 Block 中的索引 */
#define GPGPU_REG_LANE_ID           0x1024  /* lane 0-31 */

/* BARRIER：预留；THREAD_MASK：可读写的教学寄存器。 */
#define GPGPU_REG_BARRIER           0x2000  /* 保留；读 0，写入忽略 */
#define GPGPU_REG_THREAD_MASK       0x2004  /* 活跃线程掩码 */

/* GLOBAL_CTRL：保存 ENABLE；写入 RESET 会复位设备寄存器。 */
typedef enum GPGPUControl {
    GPGPU_CTRL_ENABLE = 1 << 0,
    GPGPU_CTRL_RESET  = 1 << 1,
} GPGPUControl;

/* GLOBAL_STATUS：只读标志；ERROR 可与 READY 或 BUSY 同时置位。 */
typedef enum GPGPUStatus {
    GPGPU_STATUS_READY = 1 << 0,
    GPGPU_STATUS_BUSY  = 1 << 1,
    GPGPU_STATUS_ERROR = 1 << 2,
} GPGPUStatus;

/* ERROR_STATUS：锁存错误标志，向对应位写 1 清除。 */
typedef enum GPGPUError {
    GPGPU_ERR_INVALID_CMD  = 1 << 0,
    GPGPU_ERR_VRAM_FAULT   = 1 << 1,
    GPGPU_ERR_KERNEL_FAULT = 1 << 2,
    GPGPU_ERR_DMA_FAULT    = 1 << 3,
} GPGPUError;

/* IRQ_ENABLE/STATUS/ACK 共用这些标志；ACK 清除指定事件。 */
typedef enum GPGPUIrq {
    GPGPU_IRQ_KERNEL_DONE = 1 << 0,
    GPGPU_IRQ_DMA_DONE    = 1 << 1,
    GPGPU_IRQ_ERROR       = 1 << 2,
} GPGPUIrq;

/* DMA_CTRL：控制标志；START 为命令位，提交后自动清除。 */
typedef enum GPGPUDMAControl {
    GPGPU_DMA_START         = 1 << 0,
    GPGPU_DMA_DIR_TO_VRAM   = 0 << 1,
    GPGPU_DMA_DIR_FROM_VRAM = 1 << 1,
    GPGPU_DMA_IRQ_ENABLE    = 1 << 2,
} GPGPUDMAControl;

/* DMA_STATUS：取一个互斥状态，沿用原寄存器编码。 */
typedef enum GPGPUDMAStatus {
    GPGPU_DMA_IDLE     = 0,
    GPGPU_DMA_BUSY     = 1 << 0,
    GPGPU_DMA_COMPLETE = 1 << 1,
    GPGPU_DMA_ERROR    = 1 << 2,
} GPGPUDMAStatus;

#define GPGPU_DEV_ID_VALUE       0x47505055
#define GPGPU_DEV_VERSION_VALUE  0x00010000

#endif /* HW_GPGPU_REGS_H */
