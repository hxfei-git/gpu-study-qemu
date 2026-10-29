/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 计算执行器：Dispatch 与调度器之间共享的类型
 *
 * 一次派发的流向：
 *
 *   CP 写 0x300 寄存器组 ─▶ dispatch.c 检查并锁存 GPGPUKernelParams
 *     ─▶ scheduler.c 创建任务，按 warp 分段推进（定时器驱动）
 *       ─▶ rv32.c 逐 lane 执行一条 RV32 指令
 *         ─▶ lane_mem.c 取指与 LW/SW，经 gpgpu_user_rw() 访问用户地址
 *     ─▶ dispatch.c 发布 DONE/FAILED，等待 CP 写 ACK
 */
#ifndef HW_GPGPU_COMPUTE_H
#define HW_GPGPU_COMPUTE_H

#include "gpgpu/gpgpu_regs.h"

/* 每个 warp 的 lane 数。 */
#define GPGPU_WARP_SIZE 32
/* 单次派发的工作组数上限。 */
#define GPGPU_MAX_BLOCKS 4096
/* 每工作组的线程数上限。 */
#define GPGPU_MAX_BLOCK_THREADS 1024
/* 单次派发的总线程数上限。 */
#define GPGPU_MAX_TOTAL_THREADS 65536

/**
 * struct GPGPUKernelParams - 一次派发的配置快照
 * @kernel_addr: 代码入口；gpuvm=on 时为 GPUVA，否则为 GPU 物理地址。
 * @kernel_args: 参数块地址，与代码入口同一地址空间；启动时写入 a0。
 * @grid_dim: X/Y/Z 方向工作组数。
 * @block_dim: 工作组 X/Y/Z 方向线程数。
 * @code_size: 代码字节数，限定取指范围。
 * @args_size: 参数块字节数。
 * @shared_mem_size: 每工作组共享内存字节数，本版只接受 0。
 * @vmid: 所属用户 VMID。
 * @source_slot: 来源 HQD 槽，访存故障记到此槽。
 *
 * 提交时复制一份交给任务；之后的寄存器写入不影响正在执行的任务。
 */
typedef struct GPGPUKernelParams {
	uint64_t kernel_addr;
	uint64_t kernel_args;
	uint32_t grid_dim[3];
	uint32_t block_dim[3];
	uint32_t code_size;
	uint32_t args_size;
	uint32_t shared_mem_size;
	uint32_t vmid;
	uint32_t source_slot;
} GPGPUKernelParams;

/**
 * enum GPGPUExecStatus - 一段执行结束时的进度
 * @GPGPU_EXEC_COMPLETE: 整个任务已完成。
 * @GPGPU_EXEC_CONTINUE: 任务尚未完成，需要再调度一段。
 * @GPGPU_EXEC_FAILED: 任务失败，不再推进。
 */
enum GPGPUExecStatus {
	GPGPU_EXEC_COMPLETE,
	GPGPU_EXEC_CONTINUE,
	GPGPU_EXEC_FAILED,
};

/**
 * struct GPGPUExecResult - 一段执行的结果
 * @status: 进度。
 * @error: 失败时的 GpgpuDispatchError；其他情况为 GPGPU_EXEC_ERR_NONE。
 */
typedef struct GPGPUExecResult {
	enum GPGPUExecStatus status;
	uint32_t error;
} GPGPUExecResult;

#endif /* HW_GPGPU_COMPUTE_H */
