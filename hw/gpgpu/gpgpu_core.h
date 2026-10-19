/*
 * QEMU GPGPU - RISC-V SIMT Core
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * 简化的 RV32I 指令解释器，用于 GPU 核心模拟。
 * 参考 NEMU 的设计思想，但独立实现。
 */

#ifndef HW_GPGPU_CORE_H
#define HW_GPGPU_CORE_H

#include "qemu/osdep.h"
#include "fpu/softfloat.h"

/* 前向声明 */
typedef struct GPGPUState GPGPUState;

/*
 * ============================================================================
 * 常量定义
 * ============================================================================
 */
#define GPGPU_WARP_SIZE 32 /* 每个 warp 的 lane 数量 */
#define GPGPU_NUM_REGS 32  /* RISC-V 通用寄存器数量 */
#define GPGPU_NUM_FREGS 32 /* RISC-V 浮点寄存器数量 */

/* 限制在 QEMU 主循环中同步执行的工作量。 */
/* 一次派发允许的工作组总数上限。 */
#define GPGPU_MAX_BLOCKS 4096
/* 一个工作组允许的线程数上限。 */
#define GPGPU_MAX_BLOCK_THREADS 1024
/* 设备一次派发允许的总线程数上限。 */
#define GPGPU_MAX_TOTAL_THREADS 65536

/* 浮点 CSR 地址 */
/* RISC-V 浮点异常标志 CSR 编号。 */
#define CSR_FFLAGS 0x001
/* RISC-V 浮点舍入模式 CSR 编号。 */
#define CSR_FRM 0x002
/* RISC-V 浮点控制与状态 CSR 编号。 */
#define CSR_FCSR 0x003

/*
 * ============================================================================
 * mhartid CSR 定义
 * ============================================================================
 * 位域布局:
 *   31        13 12     5 4    0
 *   +---------+--------+------+
 *   | block   | warp   | tid  |
 *   | (19bit) | (8bit) | (5b) |
 *   +---------+--------+------+
 */
/* 线程读取 block、warp、lane 身份的 CSR 编号。 */
#define CSR_MHARTID 0xF14

/* mhartid 中 lane 编号占用的位数。 */
#define MHARTID_THREAD_BITS 5
/* mhartid 中 warp 编号占用的位数。 */
#define MHARTID_WARP_BITS 8
/* mhartid 中线性工作组编号占用的位数。 */
#define MHARTID_BLOCK_BITS 19
/* 提取 mhartid 低位 lane 编号的掩码。 */
#define MHARTID_THREAD_MASK 0x1F
/* 右移后提取 warp 编号的掩码。 */
#define MHARTID_WARP_MASK 0xFF

/* 将线性工作组、warp 与 lane 编号拼成 mhartid。 */
#define MHARTID_ENCODE(block, warp, thread) (((block) << 13) | ((warp) << 5) | ((thread) & 0x1F))
/* 从 mhartid 提取 lane 编号。 */
#define MHARTID_THREAD(id) ((id) & 0x1F)
/* 从 mhartid 提取工作组内的 warp 编号。 */
#define MHARTID_WARP(id) (((id) >> 5) & 0xFF)
/* 从 mhartid 提取线性工作组编号。 */
#define MHARTID_BLOCK(id) ((id) >> 13)

/**
 * struct GPGPULane - 单个 RV32 线程的寄存器和执行状态
 * @gpr: 通用寄存器 x0-x31
 * @fpr: 浮点寄存器 f0-f31
 * @pc: 程序计数器
 * @mhartid: 完整 hart ID (block|warp|lane)
 * @fcsr: fflags[4:0] | frm[7:5]
 * @fp_status: softfloat 运行状态
 * @active: 是否活跃
 */
typedef struct GPGPULane {
	uint32_t gpr[GPGPU_NUM_REGS];
	uint32_t fpr[GPGPU_NUM_FREGS];
	uint32_t pc;
	uint32_t mhartid;
	uint32_t fcsr;
	float_status fp_status;
	bool active;
} GPGPULane;

/**
 * struct GPGPUWarp - 最多 32 个 lane 的执行上下文
 * @lanes: 32 个 lane
 * @active_mask: 活跃掩码，每位代表一个 lane
 * @thread_id_base: 这个 warp 的起始 thread_id
 * @warp_id: warp 在 block 内的编号
 * @block_id: 所属 block 的 ID
 */
typedef struct GPGPUWarp {
	GPGPULane lanes[GPGPU_WARP_SIZE];
	uint32_t active_mask;
	uint32_t thread_id_base;
	uint32_t warp_id;
	uint32_t block_id[3];
} GPGPUWarp;

/**
 * struct GPGPUDispatchTask - 跨定时器回调保存派发位置与累计执行预算
 * @params: 提交时固定的派发配置，后续寄存器写入不能覆盖。
 * @warp: 当前工作组中正在执行的 warp 上下文。
 * @block_count: 本次派发的工作组总数。
 * @threads_per_block: 一个工作组的线程总数。
 * @warp_count: 每个工作组需要执行的 warp 数。
 * @block_index: 当前工作组的线性编号，X 维变化最快。
 * @warp_index: 当前 warp 在工作组内的编号。
 * @warp_cycles: 当前 warp 已消耗的解释执行周期数。
 * @total_cycles: 整个 Dispatch 已消耗的累计周期数。
 * @warp_ready: 当前 warp 是否已初始化，可在下次回调继续执行。
 */
struct GPGPUDispatchTask {
	GPGPUKernelParams params;
	GPGPUWarp warp;
	uint32_t block_count;
	uint32_t threads_per_block;
	uint32_t warp_count;
	uint32_t block_index;
	uint32_t warp_index;
	uint32_t warp_cycles;
	uint64_t total_cycles;
	bool warp_ready;
};

/*
 * gpgpu_core_params() - 取得当前执行使用的派发参数
 *
 * 存在分段任务时返回其提交快照，否则使用同步配置寄存器。
 */
static inline const GPGPUKernelParams *gpgpu_core_params(GPGPUState *s)
{
	return s->task ? &s->task->params : &s->kernel;
}

/*
 * ============================================================================
 * CTRL 设备地址定义 (GPU 核心视角)
 * ============================================================================
 * GPU 核心通过访问这些地址来获取自己的线程 ID
 */
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

/*
 * ============================================================================
 * 执行引擎 API
 * ============================================================================
 */

/**
 * gpgpu_core_init_warp - 初始化一个 warp
 * @warp: warp 状态指针
 * @pc: 初始程序计数器（内核代码地址）
 * @thread_id_base: 起始线程 ID
 * @block_id: block ID 数组 [x, y, z]
 * @num_threads: 活跃线程数量 (最多 32)
 * @warp_id: warp 在 block 内的编号
 * @block_id_linear: 线性化的 block ID
 */
void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc, uint32_t thread_id_base,
			  const uint32_t block_id[3], uint32_t num_threads, uint32_t warp_id,
			  uint32_t block_id_linear);

/**
 * gpgpu_core_exec_warp - 推进一个 warp
 * @s: GPGPU 设备状态
 * @warp: warp 状态指针
 * @max_cycles: 本次调用周期上限；队列任务另保存累计预算
 *
 * 返回: 0 完成，-1 错误；队列任务返回 1 表示需要后续分段
 */
int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles);

/**
 * gpgpu_core_exec_kernel - 执行完整的 kernel
 * @s: GPGPU 设备状态
 *
 * 派发入口检查代码地址和维度非零后调用本函数。
 * 根据 s->kernel 中配置的 grid/block 维度执行内核。
 * 返回: 0 成功，-1 错误
 */
int gpgpu_core_exec_kernel(GPGPUState *s);
/* 继续执行队列模式的一段计算。 */
int gpgpu_core_step_task(GPGPUState *s);

#endif /* HW_GPGPU_CORE_H */
