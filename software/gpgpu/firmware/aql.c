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
 * AQL 包里的 grid_size 是各维的总线程数，执行器要的是各维工作组数，
 * 所以这里用 grid_size / workgroup_size 换算，并在换算时检查上限：
 *
 * 1. Header 去掉 barrier 位后必须等于 GPGPU_AQL_HEADER（Kernel Dispatch、
 *    acquire/release 均为 SYSTEM），setup 中的维度数为 1～3；
 * 2. 保留字段为 0；本版本不支持私有段和工作组共享段，
 *    两者大小必须为 0；
 * 3. 逐维检查：工作组尺寸和总线程数都非零，总线程数能被
 *    工作组尺寸整除，超出维度数的维必须两项都为 1。
 *    把本维乘进三个累计值前，先与“上限 / 已有累计值”比较，
 *    保证每组线程数、工作组数和总线程数都不超限，乘法也不会溢出。
 *
 * Return: 1 表示合法，并已填好 t->block 和 t->grid；0 时调用者记 PACKET 错误。
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

	/* 1. 包类型、fence scope 与维度数。 */
	if ((header & ~GPGPU_AQL_BARRIER) != GPGPU_AQL_HEADER || dims < 1 || dims > 3)
		return 0;

	/* 2. 保留字段与两种段大小都必须为 0。 */
	if (p->reserved0 || p->reserved2 || p->private_segment_size || p->group_segment_size)
		return 0;

	/* 3. 逐维整除、换算工作组数，并在累乘前检查上限。 */
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
 * 包中的 kernel_object 指向运行时生成的 16 字节描述对象。地址按 8 字节
 * 对齐时，按队列的访问域把它复制到 t->desc，再检查：
 *
 * - ABI 版本与固件一致；
 * - 代码长度非零，入口和长度按 4 字节对齐，代码不越过 32 位地址空间；
 * - 参数长度在 4 字节到 64 KiB 之间且为 4 的倍数；包中的参数地址
 *   按 8 字节对齐，整段参数块在 32 位地址空间内。
 *
 * 这些是提交执行器前的静态检查，运行中的取指和参数读取越界仍由
 * 执行器检查。
 *
 * Return: 1 表示合法；0 时调用者记 DESCRIPTOR 错误。
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

/*
 * 读取并检查完成 Signal，取出完成时要用的事件编号和通知槽地址。
 * Signal 按 8 字节对齐；完成值必须是 Host 提交前写入的 1；事件编号
 * 非零，保留字段为 0。
 *
 * 完成值是 Host 与 CP 共享的动态值，要用单条 64 位 load 读到完整值，
 * 所以各字段逐个标量读取，不用 cp_read_object() 整体复制。
 * 通知槽地址的范围由调用者检查。
 *
 * Return: 1 表示合法；0 时调用者记 SIGNAL 错误。
 */
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
 * 多个 Host 线程可能先预留编号再填包，W 已经越过 R 并不代表 R 处的包已
 * 发布。Host 最后才用一次 32 位写发布 Header，所以 CP 先单独读 Header：
 * 仍是 INVALID 就返回 0，下一轮再看，不跳过这个包。
 * Header 有效后，依次读包体，检查形状、描述对象、Signal 和通知槽地址，
 * 每项失败都记对应的队列错误并停止该队列，不推进 R。
 *
 * Return: 1 表示包可执行；0 表示没有已发布的包，或已按错误停止队列。
 */
static int aql_fetch(u32 slot, AqlTask *t)
{
	if (!cp_next_packet(slot, &t->addr, sizeof(t->packet)))
		return 0;

	/*
	 * 1. 先单独读 Header 和 setup 所在的 32 位字，
	 *    类型为 INVALID 时还未发布，不读包体。
	 *    fence 保证读到有效 Header 后，才读 Host 写好的包体。
	 */
	if (!cp_queue_read32(slot, GPGPU_QUEUE_ERROR_RING, t->addr, &t->packet.full_header)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RING);
		return 0;
	}
	if ((t->packet.full_header & 0xff) == GPGPU_AQL_INVALID)
		return 0;
	cp_fence();

	/* 2. 读剩余 60 字节包体，再逐项检查；错误编号指出哪一项出错。 */
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

/*
 * 第 3 步：把已检查的包写入执行器的分发寄存器。
 * 调用者已确认 DISPATCH_STATUS 为空闲，执行器此时不会读这些寄存器。
 * VMID 和 slot 决定执行器访存时的地址翻译和故障归属；代码与参数地址、
 * 长度供执行器做运行时范围检查；网格写入各维工作组数和工作组尺寸。
 * 本版本不支持工作组共享存储，大小固定写 0。
 * 写入顺序由调用者的 fence 保证。
 */
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
 * 第 4 步：提交并等待执行器给出终态。
 *
 * 写 DISPATCH=1 启动执行器，然后轮询 DISPATCH_STATUS 直到不再为 BUSY；
 * 执行器只有在结果写回和相关访存都结束后才给出 DONE 或失败。
 * fence 保证先看到终态，再读 DISPATCH_ERROR。
 * 最后写 ACK，执行器清除结果并回到空闲，下一次派发才能提交；
 * ACK 不会中止执行，因为此时执行已经结束。
 *
 * Return: 状态为 DONE 且没有细分错误时为 1；0 时调用者记 EXECUTOR 错误。
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
 * aql_complete() - 第 5 步：任务成功后按序发布完成
 *
 * Host 等待方先轮询 Signal，超时后阻塞在事件上，由驱动根据 IH 记录和
 * 通知槽唤醒；Host 提交方根据 R 复用 Ring 槽。所以顺序必须是：
 *
 * 1. Signal：fence 保证执行器写回的结果先于 Signal 可见；再读一次确认
 *    完成值仍为 1，然后用单条 64 位 store 写 0；
 * 2. 通知槽：fence 后写入零扩展的事件编号，驱动消费 IH 记录时用它核对；
 * 3. IH：fence 后提交 SIGNAL_NOTIFY，驱动据此唤醒等待者。提交失败时
 *    cp_ih_submit() 已置 cp_stopped，直接返回；
 * 4. Header 写回 INVALID，Host 复用该槽时才能从“未发布”状态开始填包；
 * 5. R+1 并写回共享进度，Host 读到新 R 后才会复用该槽。
 *
 * 任一步失败停止该队列，之后的步骤不执行，已写入的内容不回退。
 */
static void aql_complete(u32 slot, const AqlTask *t)
{
	u64 sig = t->packet.completion_signal;
	u64 value;

	/* 1. 结果先可见，再把 Signal 从 1 改为 0。 */
	cp_fence();
	if (!cp_queue_read64(slot, GPGPU_QUEUE_ERROR_SIGNAL, sig, &value) || value != 1 ||
	    !cp_queue_write64(slot, GPGPU_QUEUE_ERROR_SIGNAL, sig, 0)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_SIGNAL);
		return;
	}

	/* 2. Signal 可见后再写通知槽。 */
	cp_fence();
	if (!cp_queue_write64(slot, GPGPU_QUEUE_ERROR_MAILBOX, t->mailbox, t->event_id)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_MAILBOX);
		return;
	}

	/* 3. 通知槽可见后再发 IH 记录。 */
	cp_fence();
	if (!cp_ih_submit(slot, GPGPU_IH_SIGNAL_NOTIFY, t->event_id))
		return;

	/* 4. 把 Header 恢复为 INVALID，供 Host 下一次复用该槽。 */
	if (!cp_queue_write32(slot, GPGPU_QUEUE_ERROR_RETIRE, t->addr, GPGPU_AQL_INVALID)) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_RETIRE);
		return;
	}

	/* 5. 推进 R 并写回共享进度。 */
	cp_retire(slot);
}

/*
 * cp_aql() - 处理用户槽中的一个派发包
 *
 * 一次调用同步走完文件头的 5 步：取包并检查，配置执行器，等待整个
 * Dispatch 结束，再发布完成。CP 在等待期间不处理其他队列。
 *
 * Return: 本轮提交过执行器或调度已停止时为 1，供主循环轮换两个用户槽的
 * 优先顺序；没有可执行的包（含提交前即出错）时为 0。
 */
int cp_aql(u32 slot)
{
	AqlTask t;

	/* 1. 2. 没有已发布的包或检查失败时返回 0，失败已记错误。 */
	if (!aql_fetch(slot, &t))
		return 0;

	/*
	 * 3. 执行器须空闲，否则说明上一次结果没有确认，记 EXECUTOR 错误。
	 *    配置写完后 fence，再确认 IH 仍正常：IH 已故障时
	 *    完成通知送不到 Host，不再提交。
	 */
	if (cp_reg_read(GPGPU_REG_DISPATCH_STATUS) != GPGPU_DISPATCH_IDLE) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 0;
	}
	aql_configure(slot, &t);
	cp_fence();
	if (cp_ih_faulted())
		return 1;

	/* 4. 提交并等待终态；执行失败记 EXECUTOR 错误，不发布成功 Signal。 */
	if (!aql_run()) {
		cp_queue_error(slot, GPGPU_QUEUE_ERROR_EXECUTOR);
		return 1;
	}

	/* IH 可能在执行期间故障；执行器已结束，停止成功收尾。 */
	if (cp_ih_faulted())
		return 1;

	/* 5. 执行成功，按序发布 Signal、通知槽、IH、Header 和 R。 */
	aql_complete(slot, &t);
	return 1;
}
