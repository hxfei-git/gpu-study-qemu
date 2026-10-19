/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

/*
 * gpgpu_dispatch_kernel() - 执行 Host 同步派发命令
 *
 * 检查配置后运行整个网格，结束时清 BUSY 并上报完成或错误中断。
 */
static void gpgpu_dispatch_kernel(GPGPUState *s)
{
	int ret;

	if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) || (s->global_status & GPGPU_STATUS_BUSY) ||
	    !s->kernel.grid_dim[0] || !s->kernel.grid_dim[1] || !s->kernel.grid_dim[2] ||
	    !s->kernel.block_dim[0] || !s->kernel.block_dim[1] || !s->kernel.block_dim[2] ||
	    s->kernel.shared_mem_size || !gpgpu_vram_word_valid(s, s->kernel.kernel_addr) ||
	    !gpgpu_vram_word_valid(s, s->kernel.kernel_args)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}

	s->global_status |= GPGPU_STATUS_BUSY;
	ret = gpgpu_core_exec_kernel(s);
	s->global_status &= ~GPGPU_STATUS_BUSY;
	s->global_status |= GPGPU_STATUS_READY;

	if (ret < 0)
		gpgpu_set_error(s, GPGPU_ERR_KERNEL_FAULT);
	else
		gpgpu_raise_irq(s, GPGPU_IRQ_KERNEL_DONE);
}

/*
 * gpgpu_kernel_read() - 读回派发配置和执行结果寄存器
 *
 * 按 32 位偏移返回保存值，地址高低位分开读回。
 */
uint32_t gpgpu_kernel_read(GPGPUState *s, hwaddr addr)
{
	switch (addr) {
	case GPGPU_REG_KERNEL_ADDR_LO:
		return (uint32_t)s->kernel.kernel_addr;
	case GPGPU_REG_KERNEL_ADDR_HI:
		return (uint32_t)(s->kernel.kernel_addr >> 32);
	case GPGPU_REG_KERNEL_ARGS_LO:
		return (uint32_t)s->kernel.kernel_args;
	case GPGPU_REG_KERNEL_ARGS_HI:
		return (uint32_t)(s->kernel.kernel_args >> 32);
	case GPGPU_REG_GRID_DIM_X:
		return s->kernel.grid_dim[0];
	case GPGPU_REG_GRID_DIM_Y:
		return s->kernel.grid_dim[1];
	case GPGPU_REG_GRID_DIM_Z:
		return s->kernel.grid_dim[2];
	case GPGPU_REG_BLOCK_DIM_X:
		return s->kernel.block_dim[0];
	case GPGPU_REG_BLOCK_DIM_Y:
		return s->kernel.block_dim[1];
	case GPGPU_REG_BLOCK_DIM_Z:
		return s->kernel.block_dim[2];
	case GPGPU_REG_SHARED_MEM_SIZE:
		return s->kernel.shared_mem_size;
	case GPGPU_REG_KERNEL_CODE_SIZE:
		return s->kernel.code_size;
	case GPGPU_REG_KERNEL_ARGS_SIZE:
		return s->kernel.args_size;
	case GPGPU_REG_DISPATCH_STATUS:
		return s->dispatch_status;
	case GPGPU_REG_DISPATCH_ERROR:
		return s->dispatch_error;
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
	case GPGPU_REG_KERNEL_ADDR_LO:
		s->kernel.kernel_addr = deposit64(s->kernel.kernel_addr, 0, 32, val);
		break;
	case GPGPU_REG_KERNEL_ADDR_HI:
		s->kernel.kernel_addr = deposit64(s->kernel.kernel_addr, 32, 32, val);
		break;
	case GPGPU_REG_KERNEL_ARGS_LO:
		s->kernel.kernel_args = deposit64(s->kernel.kernel_args, 0, 32, val);
		break;
	case GPGPU_REG_KERNEL_ARGS_HI:
		s->kernel.kernel_args = deposit64(s->kernel.kernel_args, 32, 32, val);
		break;
	case GPGPU_REG_GRID_DIM_X:
		s->kernel.grid_dim[0] = val;
		break;
	case GPGPU_REG_GRID_DIM_Y:
		s->kernel.grid_dim[1] = val;
		break;
	case GPGPU_REG_GRID_DIM_Z:
		s->kernel.grid_dim[2] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_X:
		s->kernel.block_dim[0] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_Y:
		s->kernel.block_dim[1] = val;
		break;
	case GPGPU_REG_BLOCK_DIM_Z:
		s->kernel.block_dim[2] = val;
		break;
	case GPGPU_REG_SHARED_MEM_SIZE:
		s->kernel.shared_mem_size = val;
		break;
	case GPGPU_REG_KERNEL_CODE_SIZE:
		s->kernel.code_size = val;
		break;
	case GPGPU_REG_KERNEL_ARGS_SIZE:
		s->kernel.args_size = val;
		break;
	default:
		break;
	}
}

/*
 * gpgpu_kernel_write() - 处理 Host 的同步派发寄存器写入
 *
 * 拒绝队列模式或非空闲执行器，DISPATCH 写入转入同步计算。
 */
void gpgpu_kernel_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	if (s->queue_mode || s->dispatch_status != GPGPU_DISPATCH_IDLE) {
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
 * 检查代码、参数和网格边界，用先除后乘防溢出，并计算工作组及 warp
 * 数。
 */
static bool dispatch_validate(GPGPUState *s, GPGPUDispatchTask *task)
{
	const GPGPUKernelParams *p = &task->params;

	if (!(s->global_ctrl & GPGPU_CTRL_ENABLE) || p->shared_mem_size || (p->kernel_addr & 3) ||
	    !p->code_size || (p->code_size & 3) ||
	    !gpgpu_vram_contains(s, p->kernel_addr, p->code_size) || (p->kernel_args & 7) ||
	    p->args_size < 4 || p->args_size > GPGPU_KERNEL_ARGS_MAX || (p->args_size & 3) ||
	    p->kernel_args > UINT64_MAX - (p->args_size - 1)) {
		return false;
	}
	task->block_count = task->threads_per_block = 1;
	for (unsigned int i = 0; i < 3; i++) {
		if (!p->grid_dim[i] || !p->block_dim[i] ||
		    p->grid_dim[i] > GPGPU_MAX_BLOCKS / task->block_count ||
		    p->block_dim[i] > GPGPU_MAX_BLOCK_THREADS / task->threads_per_block) {
			return false;
		}
		task->block_count *= p->grid_dim[i];
		task->threads_per_block *= p->block_dim[i];
	}
	if (task->block_count > GPGPU_MAX_TOTAL_THREADS / task->threads_per_block)
		return false;
	task->warp_count = DIV_ROUND_UP(task->threads_per_block, GPGPU_WARP_SIZE);
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
	int result = gpgpu_core_step_task(s);

	if (result > 0) {
		timer_mod(s->dispatch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
		return;
	}
	if (result < 0 && !s->dispatch_error)
		s->dispatch_error = GPGPU_EXEC_ERR_INSTRUCTION;
	g_clear_pointer(&s->task, g_free);
	s->global_status &= ~GPGPU_STATUS_BUSY;
	/* 当前段访问已结束，结果写入先于终态可见。 */
	smp_wmb();
	s->dispatch_status = result < 0 ? GPGPU_DISPATCH_FAILED : GPGPU_DISPATCH_DONE;
}

/*
 * gpgpu_kernel_cp_write() - 处理 CP 的配置、提交与结果确认
 *
 * 仅空闲状态接受新任务，提交时固定配置；终态收到 ACK 才回到空闲。
 */
void gpgpu_kernel_cp_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	if (addr == GPGPU_REG_DISPATCH_ACK) {
		if (val != 1 ||
		    (s->dispatch_status != GPGPU_DISPATCH_DONE &&
		     s->dispatch_status != GPGPU_DISPATCH_FAILED)) {
			goto invalid;
		}
		s->dispatch_status = GPGPU_DISPATCH_IDLE;
		s->dispatch_error = 0;
		return;
	}
	if (s->dispatch_status != GPGPU_DISPATCH_IDLE || addr == GPGPU_REG_DISPATCH_STATUS ||
	    addr == GPGPU_REG_DISPATCH_ERROR) {
		goto invalid;
	}
	if (addr == GPGPU_REG_DISPATCH) {
		if (val != 1)
			goto invalid;
		s->task = g_new0(GPGPUDispatchTask, 1);
		s->task->params = s->kernel;
		s->dispatch_error = 0;
		/*
		 * 与 IH 故障发布共用 BQL，
		 * 关闭固件检查到提交之间的竞态。
		 */
		if ((s->ih.status & GPGPU_IH_FAULT) || !dispatch_validate(s, s->task)) {
			g_clear_pointer(&s->task, g_free);
			s->dispatch_error = GPGPU_EXEC_ERR_INVALID_CONFIG;
			s->dispatch_status = GPGPU_DISPATCH_FAILED;
			return;
		}
		s->queue_mode = true;
		s->dispatch_status = GPGPU_DISPATCH_BUSY;
		s->global_status |= GPGPU_STATUS_BUSY;
		timer_mod(s->dispatch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
		return;
	}
	if (addr < GPGPU_REG_KERNEL_ADDR_LO || addr > GPGPU_REG_KERNEL_ARGS_SIZE)
		goto invalid;
	kernel_config_write(s, addr, val);
	return;
invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

/*
 * gpgpu_dispatch_init() - 创建分段执行定时器
 *
 * 使用 QEMU 虚拟时钟调度 dispatch_step。
 */
void gpgpu_dispatch_init(GPGPUState *s)
{
	s->dispatch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispatch_step, s);
}

/*
 * gpgpu_dispatch_reset() - 取消分段执行并清理任务
 *
 * 先撤销定时器回调，再释放上下文并恢复 IDLE 状态。
 */
void gpgpu_dispatch_reset(GPGPUState *s)
{
	if (s->dispatch_timer)
		timer_del(s->dispatch_timer);
	g_clear_pointer(&s->task, g_free);
	s->dispatch_status = GPGPU_DISPATCH_IDLE;
	s->dispatch_error = 0;
}

/*
 * gpgpu_dispatch_cleanup() - 释放执行器生命周期资源
 *
 * 先执行任务复位，再释放定时器。
 */
void gpgpu_dispatch_cleanup(GPGPUState *s)
{
	gpgpu_dispatch_reset(s);
	timer_free(s->dispatch_timer);
}
