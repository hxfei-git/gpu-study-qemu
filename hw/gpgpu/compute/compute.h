/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 计算执行器：Dispatch 与调度器之间共享的类型
 *
 * 一次派发的流向：
 *
 *   CP 写 0x300 寄存器组 ─▶ dispatch.c 检查并锁存 GPGPUKernelParams
 *     ─▶ scheduler.c 创建任务，按 warp 分段推进（定时器驱动）
 *       ─▶ rv64.c 逐 lane 执行一条 RV64 指令
 *         ─▶ lane_mem.c 取指与 LW/SW，经 gpgpu_user_rw() 访问用户地址
 *     ─▶ dispatch.c 发布 DONE/FAILED，等待 CP 写 ACK
 */
#ifndef HW_GPGPU_COMPUTE_H
#define HW_GPGPU_COMPUTE_H

#include "gpgpu/gpgpu_regs.h"
#include "gpgpu/gpgpu_queue_abi.h"

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

/**
 * struct GPGPUExecProfile - 一次派发的可选剖析计数
 * @lane_steps: 尝试执行的逻辑 lane 指令数，含触发失败的指令。
 * @fetches: 实际调用取指路径的次数，含失败请求。
 * @decodes: 成功取指后拆解指令字段的次数。
 * @pc_groups: 各 warp 轮内不同 PC 的组数，含取指失败的组。
 * @rounds: 实际推进的 warp 轮数，含失败轮。
 * @warps: 初始化过的 warp 数。
 * @data_reads: 数据读取请求数，含失败请求，不含取指与页表读取。
 * @data_writes: 数据写入请求数，含失败请求。
 * @read_bytes: 数据读取请求的总字节数，含失败请求。
 * @write_bytes: 数据写入请求的总字节数，含失败请求。
 * @tlb_hits: 各执行段内 TLB 命中增量之和。
 * @tlb_misses: 各执行段内 TLB 未命中增量之和。
 * @dcache_hits: 各执行段内 dcache 命中增量之和。
 * @dcache_misses: 各执行段内 dcache 未命中增量之和。
 * @dcache_writebacks: 各执行段内 dcache 回写增量之和。
 * @tensor_instructions: 尝试执行的合法编码张量指令数，含访存失败。
 * @tensor_macs: 张量指令已完成局部计算的逻辑 FMA 数，不含反量化和 bias。
 * @tensor_native_tiles: 安全 SIMD 完成计算的矩阵块数。
 * @tensor_soft_tiles: SoftFloat 完成计算的矩阵块数，含未启用 SIMD 的情况。
 * @tensor_layout_elements: 布局指令的局部元素数，不是请求数。
 *
 * 段内的 32 位寄存器增量按回绕计算后累加为 64 位；不计段间 CP 轮询。
 * 统计只在 profile_dispatch 开启时更新，不增加硬件寄存器或 UAPI。
 */
typedef struct GPGPUExecProfile {
	uint64_t lane_steps;
	uint64_t fetches;
	uint64_t decodes;
	uint64_t pc_groups;
	uint64_t rounds;
	uint64_t warps;
	uint64_t data_reads;
	uint64_t data_writes;
	uint64_t read_bytes;
	uint64_t write_bytes;
	uint64_t tlb_hits;
	uint64_t tlb_misses;
	uint64_t dcache_hits;
	uint64_t dcache_misses;
	uint64_t dcache_writebacks;
	uint64_t tensor_instructions;
	uint64_t tensor_macs;
	uint64_t tensor_native_tiles;
	uint64_t tensor_soft_tiles;
	uint64_t tensor_layout_elements;
} GPGPUExecProfile;

#endif /* HW_GPGPU_COMPUTE_H */
