// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lane 访存：取指与 LW/SW
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/gpgpu/mem/gpuvm.h"
#include "compute_internal.h"
#include "lane_mem.h"

/* pc 必须 4 字节对齐，且整条指令位于本次派发的代码范围内。 */
static bool fetch_in_code_range(const GPGPUKernelParams *p, uint32_t pc)
{
	return !(pc & 3) && pc >= p->kernel_addr && p->code_size >= 4 &&
		(uint64_t)pc - p->kernel_addr <= p->code_size - 4;
}

bool gpgpu_lane_fetch(GPGPUExecContext *ctx, uint32_t pc, uint32_t *insn)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t buf[4];

	if (!fetch_in_code_range(p, pc)) {
		gpgpu_vm_record_fault(ctx->device, p->vmid, p->source_slot, pc, GPGPU_VM_EXEC,
				      GPGPU_VM_PHASE_FETCH, GPGPU_VM_FAULT_RANGE);
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	}
	if (!gpgpu_user_rw(ctx->device, p->vmid, p->source_slot, pc, buf, sizeof(buf),
			   GPGPU_VM_EXEC, GPGPU_VM_PHASE_FETCH))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_CODE_RANGE);
	*insn = ldl_le_p(buf);
	return true;
}

bool gpgpu_lane_load(GPGPUExecContext *ctx, uint32_t addr, uint32_t *value)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t buf[4];

	if (!gpgpu_user_rw(ctx->device, p->vmid, p->source_slot, addr, buf, sizeof(buf),
			   GPGPU_VM_READ, GPGPU_VM_PHASE_DATA))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_MEMORY);
	*value = ldl_le_p(buf);
	return true;
}

bool gpgpu_lane_store(GPGPUExecContext *ctx, uint32_t addr, uint32_t value)
{
	const GPGPUKernelParams *p = &ctx->params;
	uint8_t buf[4];

	stl_le_p(buf, value);
	if (!gpgpu_user_rw(ctx->device, p->vmid, p->source_slot, addr, buf, sizeof(buf),
			   GPGPU_VM_WRITE, GPGPU_VM_PHASE_DATA))
		return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_MEMORY);
	return true;
}
