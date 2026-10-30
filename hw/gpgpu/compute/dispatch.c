// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dispatch：配置寄存器、提交检查、分段推进与结果发布
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/cp/hqd.h"
#include "hw/gpgpu/irq/ih.h"
#include "hw/gpgpu/mem/gpuvm.h"
#include "dispatch.h"
#include "scheduler.h"

/* 相邻两段之间的虚拟时间间隔，单位纳秒。 */
#define DISPATCH_SEGMENT_INTERVAL_NS 1000

/* 地址的 LO/HI 寄存器：返回 kernel_addr 或 kernel_args，*half 为 0 或 4。 */
static uint64_t *config_addr(GPGPUKernelParams *c, hwaddr reg, unsigned int *half)
{
	switch (reg) {
	case GPGPU_REG_KERNEL_ADDR_LO:
	case GPGPU_REG_KERNEL_ADDR_HI:
		*half = reg - GPGPU_REG_KERNEL_ADDR_LO;
		return &c->kernel_addr;
	case GPGPU_REG_KERNEL_ARGS_LO:
	case GPGPU_REG_KERNEL_ARGS_HI:
		*half = reg - GPGPU_REG_KERNEL_ARGS_LO;
		return &c->kernel_args;
	default:
		return NULL;
	}
}

/* 32 位配置寄存器：返回 reg 对应的字段，其他偏移返回 NULL。 */
static uint32_t *config_field(GPGPUKernelParams *c, hwaddr reg)
{
	switch (reg) {
	case GPGPU_REG_GRID_DIM_X:
	case GPGPU_REG_GRID_DIM_Y:
	case GPGPU_REG_GRID_DIM_Z:
		return &c->grid_dim[(reg - GPGPU_REG_GRID_DIM_X) / 4];
	case GPGPU_REG_BLOCK_DIM_X:
	case GPGPU_REG_BLOCK_DIM_Y:
	case GPGPU_REG_BLOCK_DIM_Z:
		return &c->block_dim[(reg - GPGPU_REG_BLOCK_DIM_X) / 4];
	case GPGPU_REG_SHARED_MEM_SIZE:
		return &c->shared_mem_size;
	case GPGPU_REG_KERNEL_CODE_SIZE:
		return &c->code_size;
	case GPGPU_REG_KERNEL_ARGS_SIZE:
		return &c->args_size;
	case GPGPU_REG_DISPATCH_VMID:
		return &c->vmid;
	case GPGPU_REG_DISPATCH_SLOT:
		return &c->source_slot;
	default:
		return NULL;
	}
}

uint32_t gpgpu_dispatch_read(GPGPUState *s, hwaddr reg)
{
	GPGPUDispatchState *d = &s->dispatch;
	unsigned int half;
	uint64_t *addr;
	uint32_t *field;

	switch (reg) {
	case GPGPU_REG_DISPATCH_STATUS:
		return d->status;
	case GPGPU_REG_DISPATCH_ERROR:
		return d->error;
	}

	/* 其余偏移按配置寄存器读回；DISPATCH、ACK 只写，读零。 */
	addr = config_addr(&d->config, reg, &half);
	if (addr)
		return gpgpu_reg64_read(*addr, half);
	field = config_field(&d->config, reg);
	return field ? *field : 0;
}

/*
 * config_valid() - CP 写 DISPATCH=1 时检查已写入的配置
 *
 * 配置寄存器只在 IDLE 时可写，所以此刻的值就是本次任务要用的快照。
 * 这里只检查执行器自己能判断的条件，任一项不满足就返回 false：
 *
 * 1. 设备已使能；第一版不支持工作组共享存储，shared_mem_size 必须为 0；
 * 2. 代码：入口 4 字节对齐，长度为 4 的非零倍数，
 *    [入口, 入口 + 长度) 不越过 32 位 GPU 地址空间；
 * 3. 参数块：地址 8 字节对齐，长度 4 B～64 KiB 且为 4 的倍数，
 *    范围同样不越过 32 位地址空间；
 * 4. 来源槽是已激活的用户槽，且绑定的 VMID 与配置一致；
 *    gpuvm=on 时该 VMID 的上下文已启用，否则执行器无法翻译地址。
 *
 * 网格规模由调度器创建任务时检查。
 */
static bool config_valid(GPGPUState *s, const GPGPUKernelParams *c)
{
	/* 1. 设备未使能，或要求了不支持的共享存储。 */
	if (!gpgpu_enabled(s) || c->shared_mem_size)
		return false;

	/* 2. 代码入口、长度与范围。 */
	if ((c->kernel_addr & 3) || !c->code_size || (c->code_size & 3) ||
	    !gpgpu_range_ok(c->kernel_addr, c->code_size, GPGPU_VM_ADDRESS_LIMIT))
		return false;

	/* 3. 参数块地址、长度与范围。 */
	if ((c->kernel_args & 7) || c->args_size < 4 || c->args_size > GPGPU_KERNEL_ARGS_MAX ||
	    (c->args_size & 3) ||
	    !gpgpu_range_ok(c->kernel_args, c->args_size, GPGPU_VM_ADDRESS_LIMIT))
		return false;

	/* 4. 来源队列绑定的 VMID 与配置一致，gpuvm=on 时上下文可用。 */
	return gpgpu_hqd_binding_valid(s, c->source_slot, c->vmid) &&
		(!gpgpu_gpuvm_enabled(s) || gpgpu_vm_context_enabled(s, c->vmid));
}

static void dispatch_schedule(GPGPUState *s)
{
	timer_mod(s->dispatch.timer,
		  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DISPATCH_SEGMENT_INTERVAL_NS);
}

/*
 * 以终态结束：先释放任务、清除忙标志、写错误码，最后才写 status。
 * CP 轮询 DISPATCH_STATUS，读到 DONE/FAILED 时任务已不再访问内存，
 * DISPATCH_ERROR 也已就绪。
 */
static void dispatch_finish(GPGPUState *s, uint32_t status, uint32_t error)
{
	GPGPUDispatchState *d = &s->dispatch;

	g_clear_pointer(&d->task, gpgpu_task_destroy);
	gpgpu_set_busy(s, false);
	d->error = error;
	d->status = status;
}

/*
 * 定时器回调：让调度器推进一段。未完成则隔 1 微秒虚拟时间
 * 再推进下一段，两段之间设备可以处理 MMIO；完成或失败时发布终态。
 */
static void dispatch_step(void *opaque)
{
	GPGPUState *s = opaque;
	GPGPUExecResult result = gpgpu_task_step(s->dispatch.task);

	switch (result.status) {
	case GPGPU_EXEC_CONTINUE:
		dispatch_schedule(s);
		break;
	case GPGPU_EXEC_COMPLETE:
		dispatch_finish(s, GPGPU_DISPATCH_DONE, GPGPU_EXEC_ERR_NONE);
		break;
	case GPGPU_EXEC_FAILED:
		dispatch_finish(s, GPGPU_DISPATCH_FAILED, result.error);
		break;
	}
}

/*
 * dispatch_submit() - 处理 CP 写入的 DISPATCH=1
 *
 * 调用时状态一定是 IDLE。提交本身不执行指令，只做检查、建任务，
 * 再交给定时器分段推进：
 *
 * 1. IH 已故障时拒绝。任务完成后 CP 要经 IH 通知 Host，通道故障后
 *    无法通知；CP 检查 IH 之后、提交之前通道也可能刚刚出错；
 * 2. 检查配置快照，见 config_valid()；
 * 3. 创建任务，复制配置并检查网格规模；
 * 4. 清错误码，进入 BUSY 并置全局忙，安排第一段。
 *
 * 第 1～3 步失败都不建任务，直接进入 FAILED（INVALID_CONFIG），
 * CP 照常读取结果并写 ACK。
 */
static void dispatch_submit(GPGPUState *s)
{
	GPGPUDispatchState *d = &s->dispatch;

	/* 1. IH 通道已故障；2. 配置不合法。 */
	if (gpgpu_ih_faulted(s) || !config_valid(s, &d->config))
		goto reject;

	/* 3. 网格规模超出上限时 gpgpu_task_create() 返回 NULL。 */
	d->task = gpgpu_task_create(s, &d->config);
	if (!d->task)
		goto reject;

	/* 4. 进入 BUSY 后配置寄存器拒绝写入，任务使用自己的配置副本。 */
	d->error = GPGPU_EXEC_ERR_NONE;
	d->status = GPGPU_DISPATCH_BUSY;
	gpgpu_set_busy(s, true);
	dispatch_schedule(s);
	return;

reject:
	d->error = GPGPU_EXEC_ERR_INVALID_CONFIG;
	d->status = GPGPU_DISPATCH_FAILED;
}

/*
 * gpgpu_dispatch_cp_write() - CP 经本地窗口写 Dispatch 寄存器
 *
 * 写入规则按状态机划分（见 dispatch.h 的状态图）：
 * - ACK：只在 DONE/FAILED 时接受值 1，回到 IDLE 并清除错误码；
 * - 其他寄存器只在 IDLE 时可写，保证运行中的任务和未确认的结果
 *   不被覆盖；DISPATCH 只接受值 1，写入即提交；
 * - STATUS、ERROR 只读，不在配置表中，落到 invalid。
 *
 * 违反规则时寄存器保持原值，锁存 INVALID_CMD。
 */
void gpgpu_dispatch_cp_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUDispatchState *d = &s->dispatch;
	unsigned int half;
	uint64_t *addr;
	uint32_t *field;

	/* ACK：确认终态结果，回到 IDLE。 */
	if (reg == GPGPU_REG_DISPATCH_ACK) {
		if (value != 1 ||
		    (d->status != GPGPU_DISPATCH_DONE && d->status != GPGPU_DISPATCH_FAILED))
			goto invalid;
		d->status = GPGPU_DISPATCH_IDLE;
		d->error = GPGPU_EXEC_ERR_NONE;
		return;
	}

	/* 其余寄存器只在 IDLE 时可写。 */
	if (d->status != GPGPU_DISPATCH_IDLE)
		goto invalid;

	/* DISPATCH=1：以当前配置提交。 */
	if (reg == GPGPU_REG_DISPATCH) {
		if (value != 1)
			goto invalid;
		dispatch_submit(s);
		return;
	}

	/* 配置寄存器：64 位地址的一半，或一个 32 位字段。 */
	addr = config_addr(&d->config, reg, &half);
	if (addr) {
		gpgpu_reg64_write(addr, half, value);
		return;
	}
	field = config_field(&d->config, reg);
	if (!field)
		goto invalid;
	*field = value;
	return;

invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

bool gpgpu_dispatch_busy(GPGPUState *s)
{
	return s->dispatch.status == GPGPU_DISPATCH_BUSY;
}

void gpgpu_dispatch_init(GPGPUState *s)
{
	s->dispatch.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispatch_step, s);
}

void gpgpu_dispatch_reset(GPGPUState *s)
{
	GPGPUDispatchState *d = &s->dispatch;
	QEMUTimer *timer = d->timer;

	timer_del(timer);
	g_clear_pointer(&d->task, gpgpu_task_destroy);
	*d = (GPGPUDispatchState) { .timer = timer };
}

void gpgpu_dispatch_cleanup(GPGPUState *s)
{
	gpgpu_dispatch_reset(s);
	timer_free(s->dispatch.timer);
}
