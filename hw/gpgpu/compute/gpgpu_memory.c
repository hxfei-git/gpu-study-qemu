// SPDX-License-Identifier: GPL-2.0-or-later
/* RV32 的取指和数据访问共用用户地址空间。 */
#include "qemu/osdep.h"
#include "gpgpu_memory.h"
#include "gpgpu_compute_internal.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "qemu/bswap.h"

/* 代码范围由派发快照约束，翻译及物理访问由统一用户入口完成。 */
bool gpgpu_memory_fetch(GPGPUExecContext *ctx, uint32_t address, uint32_t *instruction)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t data[4];

	if ((address & 3) || p->code_size < 4 || address < p->kernel_addr ||
	    (uint64_t)address - p->kernel_addr > p->code_size - 4) {
		gpgpu_vm_fail(ctx->device, p->vmid, p->source_slot, address, GPGPU_VM_EXEC,
			      GPGPU_VM_PHASE_FETCH, GPGPU_VM_FAULT_RANGE);
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	}
	if (!gpgpu_user_access(ctx->device, p->vmid, p->source_slot, address, data,
			       sizeof(data), GPGPU_VM_EXEC, GPGPU_VM_PHASE_FETCH))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	*instruction = ldl_le_p(data);
	return true;
}

/* 参数通过 a0 中的真实地址读取，与整数和浮点数组使用同一入口。 */
bool gpgpu_memory_load_word(GPGPUExecContext *ctx, uint32_t address, uint32_t *value)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t data[4];

	if (!gpgpu_user_access(ctx->device, p->vmid, p->source_slot, address, data,
			       sizeof(data), GPGPU_VM_READ, GPGPU_VM_PHASE_DATA))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_MEMORY);
	*value = ldl_le_p(data);
	return true;
}

/* 不对参数地址或旧控制页地址作特殊处理；页权限由 GPUVM 检查。 */
bool gpgpu_memory_store_word(GPGPUExecContext *ctx, uint32_t address, uint32_t value)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t data[4];

	stl_le_p(data, value);
	if (!gpgpu_user_access(ctx->device, p->vmid, p->source_slot, address, data,
			       sizeof(data), GPGPU_VM_WRITE, GPGPU_VM_PHASE_DATA))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_MEMORY);
	return true;
}
