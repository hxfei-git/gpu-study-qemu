// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "gpgpu_memory.h"
#include "gpgpu_compute_internal.h"
#include "hw/gpgpu/memory/gpgpu_host.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "hw/gpgpu/memory/gpgpu_vram.h"
#include "qemu/bswap.h"
#include "software/gpgpu/include/gpgpu_queue.h"
#include "software/gpgpu/include/gpgpu_vm.h"

/*
 * RV32 的参数窗口只提供偏移，完整参数基址保存在任务快照中。先检查整个字
 * 是否落在参数块内，再读取 Host DMA 地址或翻译 GPUVA。
 */
static bool memory_read_param(GPGPUExecContext *ctx, uint32_t addr, uint32_t *value)
{
	const GPGPUKernelParams *params = &ctx->params;
	GPGPUState *s = ctx->device;
	uint32_t offset = addr - GPGPU_KERNEL_ARGS_WINDOW;
	uint8_t data[4];

	if ((offset & 3) || params->args_size < 4 || offset > params->args_size - 4 ||
	    params->kernel_args > UINT64_MAX - offset - 3) {
		if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE)
			gpgpu_vm_fail(s, params->vmid, params->source_slot,
				      params->kernel_args + offset, GPGPU_VM_READ,
				      GPGPU_VM_PHASE_ARGS, GPGPU_VM_FAULT_RANGE);
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_ARG_RANGE);
	}
	if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE) {
		if (!gpgpu_vm_access(s, params->vmid, params->source_slot,
				     params->kernel_args + offset, data, sizeof(data),
				     GPGPU_VM_READ, GPGPU_VM_PHASE_ARGS))
			return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_ARG_READ);
	} else if (!gpgpu_host_read(s, params->kernel_args + offset, data, sizeof(data))) {
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_ARG_READ);
	}
	*value = ldl_le_p(data);
	return true;
}

/* 取指范围与 VM 权限检查按原顺序执行，故障仍归属提交时的 HQD 槽。 */
bool gpgpu_memory_fetch(GPGPUExecContext *ctx, uint32_t address, uint32_t *instruction)
{
	const GPGPUKernelParams *p = &ctx->params;
	GPGPUState *s = ctx->device;

	if (ctx->mode != GPGPU_EXEC_SYNC &&
	    (address < p->kernel_addr || (uint64_t)address - p->kernel_addr > p->code_size - 4)) {
		if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE)
			gpgpu_vm_fail(s, p->vmid, p->source_slot, address, GPGPU_VM_EXEC,
				      GPGPU_VM_PHASE_FETCH, GPGPU_VM_FAULT_RANGE);
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	}
	if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE) {
		uint8_t data[4];

		if (!gpgpu_vm_access(s, p->vmid, p->source_slot, address, data, sizeof(data),
				     GPGPU_VM_EXEC, GPGPU_VM_PHASE_FETCH))
			return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
		*instruction = ldl_le_p(data);
		return true;
	}
	if (!gpgpu_vram_read_word(s, address, instruction))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	return true;
}

/* 控制页中的坐标由当前 warp 和 lane 计算，不使用 BAR0 的教学读回寄存器。 */
bool gpgpu_memory_load_word(GPGPUExecContext *ctx, const GPGPUWarp *warp, unsigned int lane_id,
			    uint32_t addr, uint32_t *value)
{
	const GPGPUKernelParams *params = &ctx->params;
	GPGPUState *s = ctx->device;
	uint32_t linear_thread = warp->thread_id_base + lane_id;

	if (ctx->mode != GPGPU_EXEC_SYNC && addr >= GPGPU_KERNEL_ARGS_WINDOW &&
	    addr - GPGPU_KERNEL_ARGS_WINDOW < GPGPU_KERNEL_ARGS_MAX)
		return memory_read_param(ctx, addr, value);
	if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE &&
	    !(addr >= GPGPU_VM_CTRL_VA && addr < GPGPU_VM_CTRL_VA + 4096)) {
		uint8_t data[4];

		if (!gpgpu_vm_access(s, params->vmid, params->source_slot, addr, data, sizeof(data),
				     GPGPU_VM_READ, GPGPU_VM_PHASE_DATA))
			return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_VRAM);
		*value = ldl_le_p(data);
		return true;
	}
	if (ctx->mode != GPGPU_EXEC_GPUVM_QUEUE && gpgpu_vram_read_word(s, addr, value))
		return true;

	switch (addr) {
	case GPGPU_CORE_CTRL_THREAD_ID_X:
		*value = linear_thread % params->block_dim[0];
		break;
	case GPGPU_CORE_CTRL_THREAD_ID_Y:
		*value = linear_thread / params->block_dim[0] % params->block_dim[1];
		break;
	case GPGPU_CORE_CTRL_THREAD_ID_Z:
		*value = linear_thread / params->block_dim[0] / params->block_dim[1];
		break;
	case GPGPU_CORE_CTRL_BLOCK_ID_X:
	case GPGPU_CORE_CTRL_BLOCK_ID_Y:
	case GPGPU_CORE_CTRL_BLOCK_ID_Z:
		*value = warp->block_id[(addr - GPGPU_CORE_CTRL_BLOCK_ID_X) / 4];
		break;
	case GPGPU_CORE_CTRL_BLOCK_DIM_X:
	case GPGPU_CORE_CTRL_BLOCK_DIM_Y:
	case GPGPU_CORE_CTRL_BLOCK_DIM_Z:
		*value = params->block_dim[(addr - GPGPU_CORE_CTRL_BLOCK_DIM_X) / 4];
		break;
	case GPGPU_CORE_CTRL_GRID_DIM_X:
	case GPGPU_CORE_CTRL_GRID_DIM_Y:
	case GPGPU_CORE_CTRL_GRID_DIM_Z:
		*value = params->grid_dim[(addr - GPGPU_CORE_CTRL_GRID_DIM_X) / 4];
		break;
	default:
		if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE)
			gpgpu_vm_fail(s, params->vmid, params->source_slot, addr, GPGPU_VM_READ,
				      GPGPU_VM_PHASE_DATA, GPGPU_VM_FAULT_RANGE);
		/* 同步非法读只返回失败，保留原有细分错误值。 */
		return gpgpu_exec_fail(ctx, ctx->mode == GPGPU_EXEC_SYNC ? 0 : GPGPU_EXEC_ERR_VRAM);
	}
	return true;
}

/* 写路径保留两种存储方式：普通 VRAM 标量写和 GPUVM 已翻译访问。 */
bool gpgpu_memory_store_word(GPGPUExecContext *ctx, uint32_t addr, uint32_t value)
{
	const GPGPUKernelParams *p = &ctx->params;
	GPGPUState *s = ctx->device;

	if (ctx->mode == GPGPU_EXEC_GPUVM_QUEUE) {
		uint8_t data[4];

		stl_le_p(data, value);
		if ((addr >= GPGPU_VM_ARGS_VA && addr < GPGPU_VM_ARGS_END) ||
		    (addr >= GPGPU_VM_CTRL_VA && addr < GPGPU_VM_CTRL_VA + 4096)) {
			gpgpu_vm_fail(s, p->vmid, p->source_slot, addr, GPGPU_VM_WRITE,
				      GPGPU_VM_PHASE_DATA, GPGPU_VM_FAULT_PERMISSION);
			return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_ARG_WRITE);
		}
		if (!gpgpu_vm_access(s, p->vmid, p->source_slot, addr, data, sizeof(data),
				     GPGPU_VM_WRITE, GPGPU_VM_PHASE_DATA))
			return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_VRAM);
		return true;
	}
	if (!gpgpu_vram_word_valid(s, addr)) {
		uint32_t error = addr >= GPGPU_KERNEL_ARGS_WINDOW &&
				addr - GPGPU_KERNEL_ARGS_WINDOW < GPGPU_KERNEL_ARGS_MAX ?
			GPGPU_EXEC_ERR_ARG_WRITE :
			GPGPU_EXEC_ERR_VRAM;

		return gpgpu_exec_fail(ctx, error);
	}
	gpgpu_vram_store(s, addr, value, sizeof(value));
	return true;
}
