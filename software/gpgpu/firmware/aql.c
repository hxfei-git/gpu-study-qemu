/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件：用户 AQL 派发
 *
 * 处理一个派发包的完整顺序：
 *
 *   1. 读 Header，INVALID 表示尚未发布，等下一轮
 *   2. 读包体，检查包、Kernel 描述对象与 Signal
 *   3. 把配置写入执行器并提交
 *   4. 等执行器结束并确认结果
 *   5. 按序发布完成：Signal 1→0、通知槽、IH 记录、Header 置 INVALID、R+1
 *
 * 第 2 步之前的错误不触及执行器；第 3 步之后出错时，执行器已经结束，已写入
 * 的数据保留，不补写成功 Signal。
 */
#include "cp.h"

/* 每工作组线程数、工作组数与总线程数的上限。 */
#define AQL_MAX_BLOCK_THREADS 1024
#define AQL_MAX_BLOCKS 4096
#define AQL_MAX_TOTAL_THREADS 65536

/**
 * struct AqlTask - 一个已通过检查的派发
 * @packet: 包的副本。
 * @desc: Kernel 描述对象的副本。
 * @addr: 包在 Ring 中的地址。
 * @grid: 各维工作组数（由总线程数除以工作组尺寸）。
 * @block: 各维工作组尺寸。
 * @event_id: Signal 中的事件编号。
 * @mailbox: Signal 中的通知槽地址。
 */
typedef struct AqlTask {
	GpgpuAqlPacket packet;
	GpgpuKernelDescriptor desc;
	u64 addr;
	u32 grid[3];
	u32 block[3];
	u32 event_id;
	u64 mailbox;
} AqlTask;

/*
 * aql_shape_valid() - 检查 Header 与网格尺寸，算出各维工作组数
 *
 * 1. Header 必须是 GPGPU_AQL_HEADER（允许 barrier 位），维度数 1～3；
 * 2. 保留字段、私有段与共享段都为 0；
 * 3. 每维：尺寸非零且能被工作组尺寸整除；未使用的维度为 1；
 *    累计线程数、工作组数和总线程数不超过上限（先除后乘，乘积不会溢出）。
 */
static int aql_shape_valid(AqlTask *t)
{
	const GpgpuAqlPacket *p = &t->packet;
	const u32 sizes[3] = { p->grid_size_x, p->grid_size_y, p->grid_size_z };
	u32 dims = p->full_header >> GPGPU_AQL_SETUP_SHIFT;
	u32 header = p->full_header & 0xffff;
	u32 threads = 1, groups = 1, total = 1;

	t->block[0] = p->workgroup_size_x;
	t->block[1] = p->workgroup_size_y;
	t->block[2] = p->workgroup_size_z;

	/* 1. Header。 */
	if ((header & ~GPGPU_AQL_BARRIER) != GPGPU_AQL_HEADER || dims < 1 || dims > 3)
		return 0;

	/* 2. 保留与段大小。 */
	if (p->reserved0 || p->reserved2 || p->private_segment_size || p->group_segment_size)
		return 0;

	/* 3. 各维。 */
	for (u32 i = 0; i < 3; i++) {
		if (!t->block[i] || !sizes[i] || sizes[i] % t->block[i] ||
		    (i >= dims && (sizes[i] != 1 || t->block[i] != 1)))
			return 0;
		t->grid[i] = sizes[i] / t->block[i];
		if (t->block[i] > AQL_MAX_BLOCK_THREADS / threads ||
		    t->grid[i] > AQL_MAX_BLOCKS / groups || sizes[i] > AQL_MAX_TOTAL_THREADS / total)
			return 0;
		threads *= t->block[i];
		groups *= t->grid[i];
		total *= sizes[i];
	}
	return 1;
}

/*
 * aql_read_descriptor() - 读取并检查 Kernel 描述对象
 *
 * ABI 版本一致；代码入口与长度按 4 字节对齐且范围在 32 位空间内；
 * 参数长度 4 字节到 64 KiB、按 4 字节对齐，参数地址按 8 字节对齐。
 */
static int aql_read_descriptor(u32 slot, AqlTask *t)
{
	const GpgpuKernelDescriptor *d = &t->desc;

	if (!cp_range_ok(t->packet.kernel_object, sizeof(*d), 8) ||
	    !cp_queue_read_object(slot, GPGPU_QUEUE_ERROR_DESCRIPTOR, t->packet.kernel_object,
				  &t->desc, sizeof(t->desc)))
		return 0;
	return d->kernel_abi_version == GPGPU_KERNEL_ABI_VERSION && d->code_size &&
		!(d->code_size & 3) && !(d->code_entry & 3) &&
		d->code_size <= GPGPU_VM_ADDRESS_LIMIT - d->code_entry && d->kernarg_size >= 4 &&
		d->kernarg_size <= GPGPU_KERNEL_ARGS_MAX && !(d->kernarg_size & 3) &&
		cp_range_ok(t->packet.kernarg_address, d->kernarg_size, 8);
}

/* 读取并检查 Signal：完成值为 1，事件编号非零，保留字段为 0；取得通知槽地址。 */
static int aql_read_signal(u32 slot, AqlTask *t)
{
	const u32 phase = GPGPU_QUEUE_ERROR_SIGNAL;
	u64 sig = t->packet.completion_signal;
	u32 reserved;
	u64 value;

	return cp_range_ok(sig, sizeof(GpgpuSignal), 8) &&
		cp_queue_read64(slot, phase, sig, &value) && value == 1 &&
		cp_queue_read32(slot, phase, sig + __builtin_offsetof(GpgpuSignal, event_id),
				&t->event_id) &&
		t->event_id &&
		cp_queue_read32(slot, phase, sig + __builtin_offsetof(GpgpuSignal, reserved),
				&reserved) &&
		!reserved &&
		cp_queue_read64(slot, phase,
				sig + __builtin_offsetof(GpgpuSignal, event_mailbox_ptr),
				&t->mailbox);
}

/*
 * aql_fetch() - 第 1、2 步：取包并检查
 *
 * Return: 1 表示包可执行；0 表示没有已发布的包，或已按错误停止队列。
 */
static int aql_fetch(u32 slot, AqlTask *t)
{
	if (!cp_next_packet(slot, &t->addr, sizeof(t->packet)))
		return 0;

	/* 1. Header：先单独读取，未发布时不读包体。 */
	if (!cp_queue_read32(slot, GPGPU_QUEUE_ERROR_RING, t->addr, &t->packet.full_header)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	if ((t->packet.full_header & 0xff) == GPGPU_AQL_INVALID)
		return 0;
	cp_fence();

	/* 2. 包体、描述对象与 Signal。 */
	if (!cp_queue_read_object(slot, GPGPU_QUEUE_ERROR_RING, t->addr + 4,
				  (char *)&t->packet + 4, sizeof(t->packet) - 4)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	if (!aql_shape_valid(t)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_PACKET);
		return 0;
	}
	if (!aql_read_descriptor(slot, t)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_DESCRIPTOR);
		return 0;
	}
	if (!aql_read_signal(slot, t)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SIGNAL);
		return 0;
	}
	if (!cp_range_ok(t->mailbox, 8, 8)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_MAILBOX);
		return 0;
	}
	return 1;
}

/* 第 3 步：写入执行器配置。调用者已确认执行器空闲。 */
static void aql_configure(u32 slot, const AqlTask *t)
{
	cp_reg_write(GPGPU_REG_DISPATCH_VMID, cp_slots[slot].vmid);
	cp_reg_write(GPGPU_REG_DISPATCH_SLOT, slot);
	cp_reg_write(GPGPU_REG_KERNEL_ADDR_LO, t->desc.code_entry);
	cp_reg_write(GPGPU_REG_KERNEL_ADDR_HI, 0);
	cp_reg_write(GPGPU_REG_KERNEL_CODE_SIZE, t->desc.code_size);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_LO, (u32)t->packet.kernarg_address);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_HI, t->packet.kernarg_address >> 32);
	cp_reg_write(GPGPU_REG_KERNEL_ARGS_SIZE, t->desc.kernarg_size);
	for (u32 i = 0; i < 3; i++) {
		cp_reg_write(GPGPU_REG_GRID_DIM_X + 4 * i, t->grid[i]);
		cp_reg_write(GPGPU_REG_BLOCK_DIM_X + 4 * i, t->block[i]);
	}
	cp_reg_write(GPGPU_REG_SHARED_MEM_SIZE, 0);
}

/*
 * 第 4 步：提交并等待终态，读出错误后写 ACK 让执行器回到空闲。
 *
 * Return: 执行器成功且没有细分错误时为 1。
 */
static int aql_run(void)
{
	u32 status, error;

	cp_reg_write(GPGPU_REG_DISPATCH, 1);
	do {
		status = cp_reg_read(GPGPU_REG_DISPATCH_STATUS);
	} while (status == GPGPU_DISPATCH_BUSY);
	cp_fence();
	error = cp_reg_read(GPGPU_REG_DISPATCH_ERROR);
	cp_reg_write(GPGPU_REG_DISPATCH_ACK, 1);
	return status == GPGPU_DISPATCH_DONE && !error;
}

/*
 * aql_complete() - 第 5 步：按序发布完成
 *
 * 1. Signal：再次确认仍为 1，写为 0；
 * 2. 通知槽：写入事件编号；
 * 3. IH：SIGNAL_NOTIFY，失败时调度已停止；
 * 4. Header 写回 INVALID，Host 才能复用该槽；
 * 5. R+1 并写回共享进度。
 * 每一步都在上一步可见之后进行；任一步失败停止该队列，之后的步骤不执行。
 */
static void aql_complete(u32 slot, const AqlTask *t)
{
	u64 sig = t->packet.completion_signal;
	u64 value;

	/* 1. Signal。 */
	cp_fence();
	if (!cp_queue_read64(slot, GPGPU_QUEUE_ERROR_SIGNAL, sig, &value) || value != 1 ||
	    !cp_queue_write64(slot, GPGPU_QUEUE_ERROR_SIGNAL, sig, 0)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SIGNAL);
		return;
	}

	/* 2. 通知槽。 */
	cp_fence();
	if (!cp_queue_write64(slot, GPGPU_QUEUE_ERROR_MAILBOX, t->mailbox, t->event_id)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_MAILBOX);
		return;
	}

	/* 3. IH。 */
	cp_fence();
	if (!cp_ih_submit(slot, GPGPU_IH_SIGNAL_NOTIFY, t->event_id))
		return;

	/* 4. Header。 */
	if (!cp_queue_write32(slot, GPGPU_QUEUE_ERROR_RETIRE, t->addr, GPGPU_AQL_INVALID)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RETIRE);
		return;
	}

	/* 5. R。 */
	cp_retire(slot);
}

/*
 * cp_aql() - 处理用户槽中的一个派发包
 *
 * Return: 本轮提交过执行器或调度已停止时为 1，供主循环轮换两个用户槽的
 * 优先顺序；没有可执行的包（含提交前即出错）时为 0。
 */
int cp_aql(u32 slot)
{
	AqlTask t;

	/* 1. 2. 取包与检查。 */
	if (!aql_fetch(slot, &t))
		return 0;

	/* 3. 配置。 */
	if (cp_reg_read(GPGPU_REG_DISPATCH_STATUS) != GPGPU_DISPATCH_IDLE) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 0;
	}
	aql_configure(slot, &t);
	cp_fence();
	if (cp_ih_faulted())
		return 1;

	/* 4. 执行。 */
	if (!aql_run()) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 1;
	}

	/* IH 可能在执行期间故障；执行器已结束，停止成功收尾。 */
	if (cp_ih_faulted())
		return 1;

	/* 5. 完成。 */
	aql_complete(slot, &t);
	return 1;
}
