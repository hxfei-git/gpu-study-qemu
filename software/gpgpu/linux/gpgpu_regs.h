/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_REGS_H
#define GPGPU_REGS_H

/* 驱动使用的设备协议子集，对应 hw/gpgpu/gpgpu_regs.h。 */
/* 教学设备的 PCI 厂商编号。 */
#define GPGPU_VENDOR_ID 0x1234
/* 教学设备的 PCI 设备编号。 */
#define GPGPU_DEVICE_ID 0x1337
/* DEV_ID 读回的教学设备签名。 */
#define GPGPU_DEV_ID_VALUE 0x47505055

/* BAR0 字节偏移，小端 32 位寄存器；以下设备信息只读。 */
/* 设备标识符。 */
#define GPGPU_REG_DEV_ID 0x0000
/* 设备能力。 */
#define GPGPU_REG_DEV_CAPS 0x0008
/* 显存大小低 32 位。 */
#define GPGPU_REG_VRAM_SIZE_LO 0x000c
/* 显存大小高 32 位。 */
#define GPGPU_REG_VRAM_SIZE_HI 0x0010

/* GLOBAL_CTRL 可读写；ERROR_STATUS 向对应位写 1 清除错误。 */
/* 全局控制 (使能/复位)。 */
#define GPGPU_REG_GLOBAL_CTRL 0x0100
/* 错误状态 (写 1 清除)。 */
#define GPGPU_REG_ERROR_STATUS 0x0108

/* ENABLE 为使能掩码，STATUS 只读，ACK 写 1 清除。 */
/* 中断使能掩码。 */
#define GPGPU_REG_IRQ_ENABLE 0x0200
/* 中断状态 (挂起的中断)。 */
#define GPGPU_REG_IRQ_STATUS 0x0204
/* 中断确认 (写 1 清除)。 */
#define GPGPU_REG_IRQ_ACK 0x0208

/*
 * 代码与参数地址为 VRAM 偏移；各地址的高 32 位位于 LO + 4。
 * GRID/BLOCK 的 X、Y、Z 寄存器依次相隔 4 字节。
 * 先写描述符，最后向只写的 DISPATCH 写任意值启动计算。
 */
/* 内核代码地址低 32 位。 */
#define GPGPU_REG_KERNEL_ADDR_LO 0x0300
/* 内核参数地址低 32 位。 */
#define GPGPU_REG_KERNEL_ARGS_LO 0x0308
/* Grid X 维度 (Block 数量)。 */
#define GPGPU_REG_GRID_DIM_X 0x0310
/* Block X 维度 (线程数量)。 */
#define GPGPU_REG_BLOCK_DIM_X 0x031c
/* 每 block 字节数。 */
#define GPGPU_REG_SHARED_MEM_SIZE 0x0328
/* 写任意值启动内核执行。 */
#define GPGPU_REG_DISPATCH 0x0330

/*
 * DMA 地址的高 32 位同样位于 LO + 4，SIZE 的单位为字节。
 * H2D：SRC 是客体 DMA 地址，DST 是 VRAM 偏移；D2H 则交换两端。
 */
/* DMA 源地址低 32 位。 */
#define GPGPU_REG_DMA_SRC_LO 0x0400
/* DMA 目标地址低 32 位。 */
#define GPGPU_REG_DMA_DST_LO 0x0408
/* 传输大小 (字节)。 */
#define GPGPU_REG_DMA_SIZE 0x0410
/* DMA 控制寄存器。 */
#define GPGPU_REG_DMA_CTRL 0x0414

/* GLOBAL_CTRL：使能位保持；写入 RESET 复位设备寄存器。 */
/* 全局使能位，允许计算和 DMA。 */
#define GPGPU_CTRL_ENABLE 1U
/* 全局复位命令位，设备处理后不锁存。 */
#define GPGPU_CTRL_RESET 2U

/* MSI-X 向量编号也对应 IRQ 状态位和驱动统计数组的下标。 */
/* 同步计算完成的 MSI-X 向量号，也是统计数组下标。 */
#define GPGPU_VECTOR_KERNEL 0
/* DMA 完成的 MSI-X 向量号，也是统计数组下标。 */
#define GPGPU_VECTOR_DMA 1
/* 设备错误的 MSI-X 向量号，也是统计数组下标。 */
#define GPGPU_VECTOR_ERROR 2
/* 同步模式申请的 MSI-X 向量数。 */
#define GPGPU_IRQ_VECTOR_COUNT 3U

/* IRQ_ENABLE、IRQ_STATUS、IRQ_ACK 共用这些位定义。 */
/* IRQ 寄存器中的同步计算完成位。 */
#define GPGPU_IRQ_KERNEL 1U
/* IRQ 寄存器中的 DMA 完成位。 */
#define GPGPU_IRQ_DMA 2U
/* IRQ 寄存器中的设备错误位。 */
#define GPGPU_IRQ_ERROR 4U
/* 同步模式使用的三类中断位掩码。 */
#define GPGPU_IRQ_ALL 7U

/* DMA_CTRL：START 触发传输后自动清除；D2H 为 0 时执行 H2D。 */
/* DMA_CTRL 的启动命令位，提交后自动清除。 */
#define GPGPU_DMA_START 1U
/* DMA_CTRL 的方向位；置位表示 VRAM 到 Host。 */
#define GPGPU_DMA_D2H 2U
/* DMA_CTRL 的完成中断使能位。 */
#define GPGPU_DMA_IRQ 4U

#endif /* GPGPU_REGS_H */
