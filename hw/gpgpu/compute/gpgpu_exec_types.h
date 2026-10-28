/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPGPU_EXEC_TYPES_H
#define HW_GPGPU_EXEC_TYPES_H

/* 一个 warp 固定包含 32 个 lane。 */
#define GPGPU_WARP_SIZE 32
/* 单次派发允许的工作组、组内线程和总线程数上限。 */
#define GPGPU_MAX_BLOCKS 4096
#define GPGPU_MAX_BLOCK_THREADS 1024
#define GPGPU_MAX_TOTAL_THREADS 65536

/**
 * struct GPGPUKernelParams - 提交时保存的派发配置
 * @kernel_addr: 代码入口；GPUVM 开启时为 GPUVA，关闭时为 GPU 物理地址。
 * @kernel_args: 参数基址；与代码入口使用相同的地址模式。
 * @grid_dim: X/Y/Z 方向的工作组数。
 * @block_dim: 工作组 X/Y/Z 方向的线程数。
 * @code_size: 代码范围的字节数，用于取指边界检查。
 * @args_size: 参数块字节数，用于派发前检查地址范围。
 * @shared_mem_size: 每工作组共享内存字节数，当前仅接受零。
 * @vmid: 用户队列所属地址空间编号，关闭翻译时仍用于归属。
 * @source_slot: GPUVM 访问故障归属的 HQD 槽号。
 *
 * GPUVM 队列的 kernel_args 同样为 GPUVA。参数类型不含可变执行进度；
 * 后续寄存器写入不会改变任务持有的这份快照。
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
 * enum GPGPUExecStatus - 本次执行调用结束时的进度
 * @GPGPU_EXEC_COMPLETE: 本次请求的执行范围已经完成。
 * @GPGPU_EXEC_CONTINUE: 队列任务尚未完成，Dispatch 需要再次调度。
 * @GPGPU_EXEC_FAILED: 执行失败，后续不再推进任务。
 */
typedef enum GPGPUExecStatus {
	GPGPU_EXEC_COMPLETE,
	GPGPU_EXEC_CONTINUE,
	GPGPU_EXEC_FAILED,
} GPGPUExecStatus;

/**
 * struct GPGPUExecResult - Core 返回给 Dispatch 的进度及错误
 * @status: 完成、待续或失败。
 * @error: 已有 GPGPU_EXEC_ERR_* 编码，error_valid 为真时有效。
 * @error_valid: 本次失败是否产生细分错误；为假时不覆盖原细分错误寄存器。
 *
 * 队列任务在提交时清错，并由 Dispatch
 * 为没有细分错误的失败补入默认指令错误。
 */
typedef struct GPGPUExecResult {
	GPGPUExecStatus status;
	uint32_t error;
	bool error_valid;
} GPGPUExecResult;

#endif /* HW_GPGPU_EXEC_TYPES_H */
