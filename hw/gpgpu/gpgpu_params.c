/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/remote/vfio-user-obj.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

/*
 * gpgpu_param_read() - 读取 RV32 只读 Host 参数窗口
 *
 * 将窗口偏移转换为完整 Host 地址，通过设备 DMA 读取一个小端 32 位值。
 */
bool gpgpu_param_read(GPGPUState *s, uint32_t addr, uint32_t *value)
{
	const GPGPUKernelParams *params = &s->task->params;
	uint32_t offset = addr - GPGPU_KERNEL_ARGS_WINDOW;
	uint8_t data[4];

	if ((offset & 3) || params->args_size < 4 || offset > params->args_size - 4 ||
	    params->kernel_args > UINT64_MAX - offset - 3) {
		s->dispatch_error = GPGPU_EXEC_ERR_ARG_RANGE;
		if (s->gpuvm)
			gpgpu_vm_fail(s, params->vmid, params->source_slot,
				      params->kernel_args + offset, GPGPU_VM_READ,
				      GPGPU_VM_PHASE_ARGS, GPGPU_VM_FAULT_RANGE);
		return false;
	}
	if (s->gpuvm) {
		if (!gpgpu_vm_access(s, params->vmid, params->source_slot,
				     params->kernel_args + offset, data, sizeof(data),
				     GPGPU_VM_READ, GPGPU_VM_PHASE_ARGS)) {
			s->dispatch_error = GPGPU_EXEC_ERR_ARG_READ;
			return false;
		}
		*value = ldl_le_p(data);
		return true;
	}
	if (vfu_object_dma_rw(PCI_DEVICE(s), params->kernel_args + offset, data, sizeof(data),
			      false) != MEMTX_OK) {
		s->dispatch_error = GPGPU_EXEC_ERR_ARG_READ;
		return false;
	}
	*value = ldl_le_p(data);
	return true;
}
