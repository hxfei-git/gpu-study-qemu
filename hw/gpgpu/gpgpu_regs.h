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
#define GPGPU_REG_DEV_ID         0x0000     /* 设备标识符 */
#define GPGPU_REG_DEV_VERSION    0x0004     /* 版本号 */
#define GPGPU_REG_DEV_CAPS       0x0008     /* 设备能力 */
#define GPGPU_REG_VRAM_SIZE_LO   0x000C     /* 显存大小低 32 位 */
#define GPGPU_REG_VRAM_SIZE_HI   0x0010     /* 显存大小高 32 位 */

/*
 * GLOBAL_CTRL：读写；GLOBAL_STATUS：只读；
 * ERROR_STATUS：写 1 清除。
 */
#define GPGPU_REG_GLOBAL_CTRL    0x0100     /* 全局控制 (使能/复位) */
#define GPGPU_REG_GLOBAL_STATUS  0x0104     /* 只读状态位 */
#define GPGPU_REG_ERROR_STATUS   0x0108     /* 错误状态 (写 1 清除) */

/* IRQ_ENABLE：读写；IRQ_STATUS：只读；IRQ_ACK：写 1 清除。 */
#define GPGPU_REG_IRQ_ENABLE     0x0200     /* 中断使能掩码 */
#define GPGPU_REG_IRQ_STATUS     0x0204     /* 中断状态 (挂起的中断) */
#define GPGPU_REG_IRQ_ACK        0x0208     /* 中断确认 (写 1 清除) */

/* 启动描述符由 CP 写入，Host 只读；DISPATCH 仅允许 CP 提交。 */
#define GPGPU_REG_KERNEL_ADDR_LO 0x0300     /* 内核代码地址低 32 位 */
#define GPGPU_REG_KERNEL_ADDR_HI 0x0304     /* 内核代码地址高 32 位 */
#define GPGPU_REG_KERNEL_ARGS_LO 0x0308     /* 内核参数地址低 32 位 */
#define GPGPU_REG_KERNEL_ARGS_HI 0x030C     /* 内核参数地址高 32 位 */
#define GPGPU_REG_GRID_DIM_X     0x0310     /* Grid X 维度 (Block 数量) */
#define GPGPU_REG_GRID_DIM_Y     0x0314     /* Grid Y 维度 */
#define GPGPU_REG_GRID_DIM_Z     0x0318     /* Grid Z 维度 */
#define GPGPU_REG_BLOCK_DIM_X    0x031C     /* Block X 维度 (线程数量) */
#define GPGPU_REG_BLOCK_DIM_Y    0x0320     /* Block Y 维度 */
#define GPGPU_REG_BLOCK_DIM_Z    0x0324     /* Block Z 维度 */
#define GPGPU_REG_SHARED_MEM_SIZE 0x0328    /* 每 block 字节数 */
#define GPGPU_REG_DISPATCH       0x0330     /* CP 写 1 提交 Kernel */

/* DMA 描述符和控制寄存器可读写；DMA_STATUS 只读。 */
#define GPGPU_REG_DMA_SRC_LO     0x0400     /* DMA 源地址低 32 位 */
#define GPGPU_REG_DMA_SRC_HI     0x0404     /* DMA 源地址高 32 位 */
#define GPGPU_REG_DMA_DST_LO     0x0408     /* DMA 目标地址低 32 位 */
#define GPGPU_REG_DMA_DST_HI     0x040C     /* DMA 目标地址高 32 位 */
#define GPGPU_REG_DMA_SIZE       0x0410     /* 传输大小 (字节) */
#define GPGPU_REG_DMA_CTRL       0x0414     /* DMA 控制寄存器 */
#define GPGPU_REG_DMA_STATUS     0x0418     /* DMA 状态寄存器 */

/* 原生 CP：未连接时读零，写入忽略。 */
#define GPGPU_REG_CP_CTRL        0x0500     /* RESET_HOLD、只读 PRESENT */
#define GPGPU_REG_CP_RESET_VECTOR 0x0504    /* CP 启动入口 */
/* CP_CTRL 的保持复位位，清零后从复位向量运行。 */
#define GPGPU_CP_RESET_HOLD      (1 << 0)
/* CP_CTRL 的只读连接标记，表明已接入原生 CP。 */
#define GPGPU_CP_PRESENT         (1 << 1)

/* CP 本地地址，非 BAR0 偏移；目标 GPU 地址基址按 64 KiB 对齐。 */
/* CP 本地 Host 访存窗口起始地址。 */
#define GPGPU_CP_HOST_WINDOW_BASE 0x80000000
/* CP Host 窗口字节容量，也是目标地址基址对齐单位。 */
#define GPGPU_CP_HOST_WINDOW_SIZE 0x00010000
/* CP 本地窗口映射基址寄存器，保存低 32 位。 */
#define GPGPU_CP_IOVA_BASE_LO    0xfffe0000
/* CP 本地窗口映射基址寄存器，保存高 32 位。 */
#define GPGPU_CP_IOVA_BASE_HI    0xfffe0004

/**
 * enum GPGPUControl - GLOBAL_CTRL 控制标志
 * @GPGPU_CTRL_ENABLE: 设备使能状态位。
 * @GPGPU_CTRL_RESET: 复位命令位，处理后不锁存。
 */
typedef enum GPGPUControl {
	GPGPU_CTRL_ENABLE = 1 << 0,
	GPGPU_CTRL_RESET = 1 << 1,
} GPGPUControl;

/**
 * enum GPGPUStatus - GLOBAL_STATUS 只读标志，错误可与其他状态并存
 * @GPGPU_STATUS_READY: 设备已就绪。
 * @GPGPU_STATUS_BUSY: 计算执行器正忙。
 * @GPGPU_STATUS_ERROR: 至少存在一个锁存错误。
 */
typedef enum GPGPUStatus {
	GPGPU_STATUS_READY = 1 << 0,
	GPGPU_STATUS_BUSY = 1 << 1,
	GPGPU_STATUS_ERROR = 1 << 2,
} GPGPUStatus;

/**
 * enum GPGPUError - ERROR_STATUS 错误标志，写 1 清除
 * @GPGPU_ERR_INVALID_CMD: 寄存器访问、命令或配置非法。
 * @GPGPU_ERR_VRAM_FAULT: VRAM 访问越界。
 * @GPGPU_ERR_DMA_FAULT: DMA 描述符或 Host 传输失败。
 * @GPGPU_ERR_IH_FAULT: IH 通道配置、协议或记录提交失败。
 */
typedef enum GPGPUError {
	GPGPU_ERR_INVALID_CMD = 1 << 0,
	GPGPU_ERR_VRAM_FAULT = 1 << 1,
	GPGPU_ERR_DMA_FAULT = 1 << 3,
	GPGPU_ERR_IH_FAULT = 1 << 4,
} GPGPUError;

/**
 * enum GPGPUIrq - IRQ_ENABLE/STATUS/ACK 共用的事件位
 * @GPGPU_IRQ_DMA_DONE: DMA 搬运完成。
 * @GPGPU_IRQ_ERROR: 设备已锁存错误。
 * @GPGPU_IRQ_IH: IH Ring 中存在待消费记录。
 */
typedef enum GPGPUIrq {
	GPGPU_IRQ_DMA_DONE = 1 << 1,
	GPGPU_IRQ_ERROR = 1 << 2,
	GPGPU_IRQ_IH = 1 << 3,
} GPGPUIrq;

/**
 * enum GPGPUDMAControl - DMA_CTRL 描述符控制位
 * @GPGPU_DMA_START: 触发搬运，提交后自动清除。
 * @GPGPU_DMA_DIR_TO_VRAM: 方向位为零，Host 到 VRAM。
 * @GPGPU_DMA_DIR_FROM_VRAM: 方向位置位，VRAM 到 Host。
 * @GPGPU_DMA_IRQ_ENABLE: 完成时发送 DMA 中断。
 */
typedef enum GPGPUDMAControl {
	GPGPU_DMA_START = 1 << 0,
	GPGPU_DMA_DIR_TO_VRAM = 0 << 1,
	GPGPU_DMA_DIR_FROM_VRAM = 1 << 1,
	GPGPU_DMA_IRQ_ENABLE = 1 << 2,
} GPGPUDMAControl;

/**
 * enum GPGPUDMAStatus - DMA_STATUS 互斥状态，沿用设备寄存器编码
 * @GPGPU_DMA_IDLE: 空闲，尚未提交传输。
 * @GPGPU_DMA_BUSY: 已启动，完成通知尚未发布。
 * @GPGPU_DMA_COMPLETE: 数据搬运和延迟完成通知均已结束。
 * @GPGPU_DMA_ERROR: 描述符或传输失败。
 */
typedef enum GPGPUDMAStatus {
	GPGPU_DMA_IDLE = 0,
	GPGPU_DMA_BUSY = 1 << 0,
	GPGPU_DMA_COMPLETE = 1 << 1,
	GPGPU_DMA_ERROR = 1 << 2,
} GPGPUDMAStatus;

/* DEV_ID 读回的教学设备签名。 */
#define GPGPU_DEV_ID_VALUE 0x47505055
/* DEV_VERSION 读回的设备版本编码。 */
#define GPGPU_DEV_VERSION_VALUE 0x00020000

#endif /* HW_GPGPU_REGS_H */
