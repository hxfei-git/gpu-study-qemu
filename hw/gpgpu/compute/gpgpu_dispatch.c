// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu_dispatch.h"
#include "hw/gpgpu/gpgpu.h"
#include "hw/gpgpu/gpgpu_regs.h"
#include "software/gpgpu/include/gpgpu_queue.h"
#include "qemu/bitops.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "hw/gpgpu/gpgpu_internal.h"
#include "hw/gpgpu/cp/gpgpu_hqd.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "hw/gpgpu/memory/gpgpu_vram.h"
#include "hw/gpgpu/interrupt/gpgpu_ih.h"
#include "hw/gpgpu/interrupt/gpgpu_irq.h"
#include "gpgpu_core.h"

/*
 * gpgpu_dispatch_kernel() - 执行 Host 同步派发命令
 *
 * 检查配置后运行整个网格，结束时清 BUSY 并上报完成或错误中断。
 */
static void gpgpu_dispatch_kernel(GPGPUState *s)
{
	const GPGPUKernelParams *p = &s->dispatch.kernel;
	GPGPUExecResult result;

	if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) || (s->global_status & GPGPU_STATUS_BUSY) ||
	    !p->grid_dim[0] || !p->grid_dim[1] || !p->grid_dim[2] || !p->block_dim[0] ||
	    !p->block_dim[1] || !p->block_dim[2] || p->shared_mem_size ||
	    !gpgpu_vram_word_valid(s, p->kernel_addr) ||
	    !gpgpu_vram_word_valid(s, p->kernel_args)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}

	s->global_status |= GPGPU_STATUS_BUSY;
	result = gpgpu_core_exec_kernel(s, p);
	if (result.error_valid)
		s->dispatch.error = result.error;
	s->global_status &= ~GPGPU_STATUS_BUSY;
	s->global_status |= GPGPU_STATUS_READY;

	if (result.status == GPGPU_EXEC_FAILED)
		gpgpu_set_error(s, GPGPU_ERR_KERNEL_FAULT);
	else
		gpgpu_raise_irq(s, GPGPU_IRQ_KERNEL_DONE);
}

/*
 * 按寄存器偏移读回保存值，64 位地址仍拆成高、低两个寄存器。
 */
uint32_t gpgpu_dispatch_read(GPGPUState *s, hwaddr addr)
{
	switch (addr) {
	case GPGPU_REG_DISPATCH_VMID:
		return s->dispatch.kernel.vmid;
	case GPGPU_REG_DISPATCH_SLOT:
		return s->dispatch.kernel.source_slot;
	case GPGPU_REG_KERNEL_ADDR_LO:
		return (uint32_t)s->dispatch.kernel.kernel_addr;
	case GPGPU_REG_KERNEL_ADDR_HI:
		return (uint32_t)(s->dispatch.kernel.kernel_addr >> 32);
	case GPGPU_REG_KERNEL_ARGS_LO:
		return (uint32_t)s->dispatch.kernel.kernel_args;
	case GPGPU_REG_KERNEL_ARGS_HI:
		return (uint32_t)(s->dispatch.kernel.kernel_args >> 32);
	case GPGPU_REG_GRID_DIM_X:
		return s->dispatch.kernel.grid_dim[0];
	case GPGPU_REG_GRID_DIM_Y:
		return s->dispatch.kernel.grid_dim[1];
	case GPGPU_REG_GRID_DIM_Z:
		return s->dispatch.kernel.grid_dim[2];
	case GPGPU_REG_BLOCK_DIM_X:
		return s->dispatch.kernel.block_dim[0];
	case GPGPU_REG_BLOCK_DIM_Y:
		return s->dispatch.kernel.block_dim[1];
	case GPGPU_REG_BLOCK_DIM_Z:
		return s->dispatch.kernel.block_dim[2];
	case GPGPU_REG_SHARED_MEM_SIZE:
		return s->dispatch.kernel.shared_mem_size;
	case GPGPU_REG_KERNEL_CODE_SIZE:
		return s->dispatch.kernel.code_size;
	case GPGPU_REG_KERNEL_ARGS_SIZE:
		return s->dispatch.kernel.args_size;
	case GPGPU_REG_DISPATCH_STATUS:
		return s->dispatch.status;
	case GPGPU_REG_DISPATCH_ERROR:
		return s->dispatch.error;
	default:
		return 0;
	}
}

/*
 * kernel_config_write() - 更新空闲执行器的配置字段
 *
 * 拆分处理 64 位地址和三维规模，调用者负责校验写入权限与忙状态。
 */
static void kernel_config_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	switch (addr) {
	case GPGPU_REG_DISPATCH_VMID:
		s->dispatch.kernel.vmid = val;
		break;
	case GPGPU_REG_DISPATCH_SLOT:
		s->dispatch.kernel.source_slot = val;
		break;
	case GPGPU_REG_KERNEL_ADDR_LO:
		s->dispatch.kernel.kernel_addr =
			deposit64(s->dispatch.kernel.kernel_addr, 0, 32, val);
		break;
	case GPGPU_REG_KERNEL_ADDR_HI:
		s->dispatch.kernel.kernel_addr =
			deposit64(s->dispatch.kernel.kernel_addr, 32, 32, val);
		break;
	case GPGPU_REG_KERNEL_ARGS_LO:
		s->dispatch.kernel.kernel_args =
			deposit64(s->dispatch.kernel.kernel_args, 0, 32, val);
		break;
	case GPGPU_REG_KERNEL_ARGS_HI:
		s->dispatch.kernel.kernel_args =
			deposit64(s->dispatch.kernel.kernel_args, 32, 32, val);
		break;
	case GPGPU_REG_GRID_DIM_X:
		s->dispatch.kernel.grid_dim[0] = val;
		break;
	case GPGPU_REG_GRID_DIM_Y:
		s->dispatch.kernel.grid_dim[1] = val;
		break;
	case GPGPU_REG_GRID_DIM_Z:
		s->dispatch.kernel.grid_dim[2] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_X:
		s->dispatch.kernel.block_dim[0] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_Y:
		s->dispatch.kernel.block_dim[1] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_Z:
		s->dispatch.kernel.block_dim[2] = val;
		break;
	case GPGPU_REG_SHARED_MEM_SIZE:
		s->dispatch.kernel.shared_mem_size = val;
		break;
	case GPGPU_REG_KERNEL_CODE_SIZE:
		s->dispatch.kernel.code_size = val;
		break;
	case GPGPU_REG_KERNEL_ARGS_SIZE:
		s->dispatch.kernel.args_size = val;
		break;
	default:
		break;
	}
}

/*
 * 先排除队列模式和非空闲执行器，再区分同步启动与普通配置写入。
 */
void gpgpu_dispatch_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	if (s->gpuvm || gpgpu_hqd_queue_mode(s) || s->dispatch.status != GPGPU_DISPATCH_IDLE) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	if (addr == GPGPU_REG_DISPATCH)
		gpgpu_dispatch_kernel(s);
	else
		kernel_config_write(s, addr, val);
}

/*
 * dispatch_validate() - 校验队列模式的派发快照
 *
 * 检查代码、参数和队列绑定，网格规模由 Core 创建任务时检查。
 */
static bool dispatch_validate(GPGPUState *s, const GPGPUKernelParams *p)
{
	if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) || p->shared_mem_size || (p->kernel_addr & 3) ||
	    !p->code_size || (p->code_size & 3) ||
	    !(s->gpuvm ? (p->kernel_addr < GPGPU_VM_ADDRESS_LIMIT &&
			  p->code_size <= GPGPU_VM_ADDRESS_LIMIT - p->kernel_addr) :
			 gpgpu_vram_contains(s, p->kernel_addr, p->code_size)) ||
	    (p->kernel_args & 7) || p->args_size < 4 || p->args_size > GPGPU_KERNEL_ARGS_MAX ||
	    (p->args_size & 3) || p->kernel_args > UINT64_MAX - (p->args_size - 1)) {
		return false;
	}
	if (s->gpuvm &&
	    (p->vmid < 1 || p->vmid > GPGPU_VM_USERS || !gpgpu_vm_enabled(s, p->vmid) ||
	     p->source_slot < 2 || p->source_slot >= 4 ||
	     !gpgpu_hqd_binding_valid(s, p->source_slot, p->vmid) ||
	     p->kernel_args >= GPGPU_VM_ADDRESS_LIMIT ||
	     p->args_size > GPGPU_VM_ADDRESS_LIMIT - p->kernel_args))
		return false;

	return true;
}

/*
 * dispatch_step() - 在定时器回调中推进计算任务
 *
 * 未结束则重新调度；结束后释放上下文，保证结果写入先于终态发布。
 */
static void dispatch_step(void *opaque)
{
	GPGPUState *s = opaque;
	GPGPUExecResult result = gpgpu_core_step_task(s->dispatch.task);

	if (result.status == GPGPU_EXEC_CONTINUE) {
		timer_mod(s->dispatch.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
		return;
	}
	if (result.error_valid)
		s->dispatch.error = result.error;
	if (result.status == GPGPU_EXEC_FAILED && !s->dispatch.error)
		s->dispatch.error = GPGPU_EXEC_ERR_INSTRUCTION;
	g_clear_pointer(&s->dispatch.task, gpgpu_core_destroy_task);
	s->global_status &= ~GPGPU_STATUS_BUSY;
	/* 当前段访问已结束，结果写入先于终态可见。 */
	smp_wmb();
	s->dispatch.status =
		result.status == GPGPU_EXEC_FAILED ? GPGPU_DISPATCH_FAILED : GPGPU_DISPATCH_DONE;
}

/*
 * ACK 只接受终态；其他写入只接受空闲态。提交时固定配置并保留 Core 句柄，
 * 定时器回调负责后续推进和终态发布。
 */
void gpgpu_dispatch_cp_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	if (addr == GPGPU_REG_DISPATCH_ACK) {
		if (val != 1 ||
		    (s->dispatch.status != GPGPU_DISPATCH_DONE &&
		     s->dispatch.status != GPGPU_DISPATCH_FAILED)) {
			goto invalid;
		}
		s->dispatch.status = GPGPU_DISPATCH_IDLE;
		s->dispatch.error = 0;
		return;
	}
	if (s->dispatch.status != GPGPU_DISPATCH_IDLE || addr == GPGPU_REG_DISPATCH_STATUS ||
	    addr == GPGPU_REG_DISPATCH_ERROR) {
		goto invalid;
	}
	if (addr == GPGPU_REG_DISPATCH) {
		if (val != 1)
			goto invalid;
		s->dispatch.error = 0;
		/*
		 * 在 BQL 保护下重新检查 IH 故障，防止固件检查后到本次提交前
		 * 通道已经故障，却仍启动新任务。
		 */
		if (gpgpu_ih_faulted(s) || !dispatch_validate(s, &s->dispatch.kernel)) {
			s->dispatch.error = GPGPU_EXEC_ERR_INVALID_CONFIG;
			s->dispatch.status = GPGPU_DISPATCH_FAILED;
			return;
		}
		s->dispatch.task = gpgpu_core_create_task(s, &s->dispatch.kernel,
							  s->gpuvm ? GPGPU_EXEC_GPUVM_QUEUE :
								     GPGPU_EXEC_PHYSICAL_QUEUE);
		if (!s->dispatch.task) {
			s->dispatch.error = GPGPU_EXEC_ERR_INVALID_CONFIG;
			s->dispatch.status = GPGPU_DISPATCH_FAILED;
			return;
		}
		gpgpu_hqd_enter_queue_mode(s);
		s->dispatch.status = GPGPU_DISPATCH_BUSY;
		s->global_status |= GPGPU_STATUS_BUSY;
		timer_mod(s->dispatch.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
		return;
	}
	if ((addr < GPGPU_REG_KERNEL_ADDR_LO || addr > GPGPU_REG_KERNEL_ARGS_SIZE) &&
	    !(s->gpuvm && (addr == GPGPU_REG_DISPATCH_VMID || addr == GPGPU_REG_DISPATCH_SLOT)))
		goto invalid;
	kernel_config_write(s, addr, val);
	return;
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

/*
 * 使用原有虚拟时钟调度间隔；初始化时不启动定时器。
 */
void gpgpu_dispatch_init(GPGPUState *s)
{
	s->dispatch.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispatch_step, s);
}

/*
 * 定时器必须先取消，随后才能释放 Core 句柄并清空配置。
 */
void gpgpu_dispatch_reset(GPGPUState *s)
{
	if (s->dispatch.timer)
		timer_del(s->dispatch.timer);
	g_clear_pointer(&s->dispatch.task, gpgpu_core_destroy_task);
	memset(&s->dispatch.kernel, 0, sizeof(s->dispatch.kernel));
	s->dispatch.status = GPGPU_DISPATCH_IDLE;
	s->dispatch.error = 0;
}

/*
 * 复用复位顺序停止当前任务，最后释放定时器。
 */
void gpgpu_dispatch_cleanup(GPGPUState *s)
{
	gpgpu_dispatch_reset(s);
	timer_free(s->dispatch.timer);
}
