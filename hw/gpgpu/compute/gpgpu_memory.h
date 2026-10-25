/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef HW_GPGPU_MEMORY_H
#define HW_GPGPU_MEMORY_H

typedef struct GPGPUExecContext GPGPUExecContext;
typedef struct GPGPUWarp GPGPUWarp;

/* 线程控制窗口的字节地址，由计算访存解释，不映射为 Host MMIO。 */
#define GPGPU_CORE_CTRL_BASE 0x80000000 /* CTRL 基地址 */
/* RV32 控制窗口中的 X 维线程在工作组内的索引，只读。 */
#define GPGPU_CORE_CTRL_THREAD_ID_X (GPGPU_CORE_CTRL_BASE + 0x00)
/* RV32 控制窗口中的 Y 维线程在工作组内的索引，只读。 */
#define GPGPU_CORE_CTRL_THREAD_ID_Y (GPGPU_CORE_CTRL_BASE + 0x04)
/* RV32 控制窗口中的 Z 维线程在工作组内的索引，只读。 */
#define GPGPU_CORE_CTRL_THREAD_ID_Z (GPGPU_CORE_CTRL_BASE + 0x08)
/* RV32 控制窗口中的 X 维工作组在网格内的索引，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_ID_X (GPGPU_CORE_CTRL_BASE + 0x10)
/* RV32 控制窗口中的 Y 维工作组在网格内的索引，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_ID_Y (GPGPU_CORE_CTRL_BASE + 0x14)
/* RV32 控制窗口中的 Z 维工作组在网格内的索引，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_ID_Z (GPGPU_CORE_CTRL_BASE + 0x18)
/* RV32 控制窗口中的 X 维工作组线程数，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_DIM_X (GPGPU_CORE_CTRL_BASE + 0x20)
/* RV32 控制窗口中的 Y 维工作组线程数，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_DIM_Y (GPGPU_CORE_CTRL_BASE + 0x24)
/* RV32 控制窗口中的 Z 维工作组线程数，只读。 */
#define GPGPU_CORE_CTRL_BLOCK_DIM_Z (GPGPU_CORE_CTRL_BASE + 0x28)
/* RV32 控制窗口中的 X 维网格工作组数，只读。 */
#define GPGPU_CORE_CTRL_GRID_DIM_X (GPGPU_CORE_CTRL_BASE + 0x30)
/* RV32 控制窗口中的 Y 维网格工作组数，只读。 */
#define GPGPU_CORE_CTRL_GRID_DIM_Y (GPGPU_CORE_CTRL_BASE + 0x34)
/* RV32 控制窗口中的 Z 维网格工作组数，只读。 */
#define GPGPU_CORE_CTRL_GRID_DIM_Z (GPGPU_CORE_CTRL_BASE + 0x38)

/**
 * gpgpu_memory_fetch() - 读取当前模式下的一条 RV32 指令
 * @ctx: 保存代码范围和地址模式的执行上下文。
 * @address: lane 当前 PC。
 * @instruction: 成功时接收小端解码后的指令。
 *
 * 队列模式先检查提交的代码范围，GPUVM 模式再执行带 EXEC 权限的翻译。
 *
 * Return: 读取成功返回 true；失败返回 false，并保存细分错误和必要的 VM 故障。
 */
bool gpgpu_memory_fetch(GPGPUExecContext *ctx, uint32_t address, uint32_t *instruction);

/**
 * gpgpu_memory_load_word() - 读取线程可见的一个 32 位字
 * @ctx: 参数快照、地址模式和错误结果。
 * @warp: 提供所属工作组坐标和线程起始编号。
 * @lane_id: 发起读取的 lane 编号。
 * @address: RV32 数据地址。
 * @value: 成功时接收小端解码后的值。
 *
 * 参数窗口优先按快照解释，其余地址按模式访问 GPUVM、VRAM 或只读线程控制页。
 *
 * Return: 成功返回 true；失败返回 false，按既有模式保存错误信息。
 */
bool gpgpu_memory_load_word(GPGPUExecContext *ctx, const GPGPUWarp *warp, unsigned int lane_id,
			    uint32_t address, uint32_t *value);

/**
 * gpgpu_memory_store_word() - 写入线程可见的一个 32 位字
 * @ctx: 地址模式、VM 归属和错误结果。
 * @address: RV32 数据地址。
 * @value: 要写入的值，存储时转换为小端。
 *
 * 只读窗口拒绝写入；普通 VRAM 写保留 AddressSpace 失效处理，GPUVM 写沿用
 * 其访问接口。返回前本次同步访问已经结束。
 *
 * Return: 成功返回 true；失败返回 false，并保存细分错误和必要的 VM 故障。
 */
bool gpgpu_memory_store_word(GPGPUExecContext *ctx, uint32_t address, uint32_t value);

#endif /* HW_GPGPU_MEMORY_H */
