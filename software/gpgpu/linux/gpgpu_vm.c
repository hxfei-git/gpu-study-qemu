// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：GPU 地址空间
 *
 * 每个会话在第一次绑定对象时建立地址空间：
 *   gpuvm=on   建立四级页表（表页从 VRAM 分配），对象绑定到应用给出的
 *              GPUVA。Host 页的叶子项写 IOVA 并置 SYSTEM，VRAM 页写 VRAM 偏移；
 *              不可缓存对象的叶子项另置 UNCACHED，设备访问这些页时不进入
 *              dcache；
 *   gpuvm=off  不建页表，对象的 GPU 地址就是它的 GPU 物理地址。
 *
 * 通知页由用户态创建：BO_CREATE 带 GPGPU_BO_EVENT_PAGE 时登记为会话的
 * 通知页，应用再像普通对象一样把它 MAP 到 GPU 地址。登记期间通知页持有
 * 一个 users 计数，不能解绑，会话清理时撤销登记。
 *
 * 用户 VMID 只在队列驻留期间占用：
 *
 *   建队列   gpgpu_vm_attach()：取空闲 VMID → 写根 → 失效整个 VMID → 启用
 *   卸载后   gpgpu_vm_detach()：停用 → 失效整个 VMID → 清根 → 归还 VMID
 *
 * TLB 项按 VMID 标记，同一个编号会先后分给不同会话，取得和归还时都失效
 * 整个 VMID，上一个使用者的翻译不会留给下一个。没有驻留队列时 TLB 中
 * 没有本 VM 的翻译，修改页表只写表项，不发失效。
 *
 * 修改页表后必须同步失效 TLB 并读到 ACK，才能放下旧映射的引用。失效失败时
 * 无法确认设备是否仍在使用旧映射，VM 标记为 failed，保留全部资源。
 *
 * 队列存活期间也可以绑定和解绑对象，此时 VM 占有 VMID，按对象范围失效。
 * Ring 与进度对象由 users 计数保护；其他对象有没有任务在用，驱动不知道，
 * 由应用在解绑前确认。
 */
#include <linux/slab.h>
#include "gpgpu_drv.h"

/**
 * struct gpgpu_vm_table - 一页驱动私有页表
 * @node: 所属 VM 的 tables 链表节点。
 * @blocks: 持有的 buddy 块。
 * @offset: VRAM 字节偏移，页对齐。
 *
 * 表页在 VM 寿命内不释放，部分撤销绑定时不拆除共享的目录。
 */
struct gpgpu_vm_table {
	struct list_head node;
	struct list_head blocks;
	u64 offset;
};

/* 表页始终锁内 BAR2 清零，再挂入 VM；不能失锁等待 DMA。 */
static int vm_table_alloc(struct gpgpu_device *dev, struct gpgpu_vm *vm, u32 *offset)
{
	struct gpgpu_vm_table *table = kzalloc(sizeof(*table), GFP_KERNEL);
	int ret;

	if (!table)
		return -ENOMEM;

	INIT_LIST_HEAD(&table->blocks);
	ret = gpgpu_vram_alloc(dev, PAGE_SIZE, &table->blocks, &table->offset, false);
	if (ret) {
		kfree(table);
		return ret;
	}

	list_add_tail(&table->node, &vm->tables);
	*offset = table->offset;
	return 0;
}

/*
 * vm_leaf() - 取得 va 的叶子项在 VRAM 中的偏移
 * @create: 中间级缺失时是否分配新表页；为 false 时缺失返回 -ENOENT。
 *
 * 经 BAR2 读写页表：从根开始依次用 VA[47:39]、VA[38:30]、VA[29:21] 索引
 * L0、L1、L2，中间项有效就进入下一级表页，缺失时按 @create 分配新表页
 * （已清零）并写入只含 VALID 的中间项。驱动的表页都在 VRAM，中间项不带
 * SYSTEM，地址域就是 BAR2 偏移。最后用 VA[20:12] 算出 L3 叶子项的偏移。
 */
static int vm_leaf(struct gpgpu_device *dev, struct gpgpu_vm *vm, u64 va, bool create,
		   u32 *offset)
{
	u32 table = vm->root;

	for (unsigned int level = 0; level < GPGPU_VM_LEVELS - 1; level++) {
		u32 index = (va >> GPGPU_VM_LEVEL_SHIFT(level)) & GPGPU_VM_INDEX_MASK;
		u32 entry_at = table + index * 8;
		u64 entry = readq(dev->vram + entry_at);
		int ret;

		if (entry & GPGPU_VM_VALID) {
			table = entry & GPGPU_VM_ADDR_MASK;
			continue;
		}
		if (!create)
			return -ENOENT;
		ret = vm_table_alloc(dev, vm, &table);
		if (ret)
			return ret;
		writeq((u64)table | GPGPU_VM_VALID, dev->vram + entry_at);
	}

	/* L3 由 VA[20:12] 索引 512 项。 */
	*offset = table + ((va >> GPGPU_VM_PAGE_SHIFT) & GPGPU_VM_INDEX_MASK) * 8;
	return 0;
}

/*
 * 写一次 VM_INV_REQ 并核对 ACK 恰为本 VMID 的位；设备同步完成，读到的
 * 就是本次结果。不符时无法确认旧翻译已清除，VM 进入 failed。
 * 调用者持 dev->lock。
 */
static int vm_invalidate_req(struct gpgpu_device *dev, struct gpgpu_vm *vm, u32 req)
{
	wmb(); /* 经 BAR2 写入的页表项先于失效请求对设备可见。 */
	gpgpu_wreg(dev, GPGPU_REG_VM_INV_REQ, req);
	if (gpgpu_rreg(dev, GPGPU_REG_VM_INV_ACK) == GPGPU_VM_INV_VMID(vm->vmid))
		return 0;
	vm->failed = true;
	return -EIO;
}

/* 失效本 VM 的全部 TLB 项，用于取得和归还 VMID；调用时 VM 占有 VMID。 */
static int vm_invalidate(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	return vm_invalidate_req(dev, vm, GPGPU_VM_INV_VMID(vm->vmid));
}

/*
 * 只失效 [va, va + size) 覆盖的页，用于绑定和解绑一个对象，不影响同一 VM
 * 其他对象的翻译。例：va = 0x0000_ffff_8a40_0000、size = 0x3000 时 VPN
 * 范围为 0xf_fff8_a400～0xf_fff8_a402，经两对 LO/HI 寄存器写入。size 非零。
 * 没有驻留队列时 VM 不占 VMID，TLB 中也没有它的翻译，直接返回；下次取得
 * VMID 时会失效整个 VMID。
 */
static int vm_invalidate_range(struct gpgpu_device *dev, struct gpgpu_vm *vm, u64 va, u64 size)
{
	if (!vm->vmid)
		return 0;

	gpgpu_wreg64(dev, GPGPU_REG_VM_INV_VPN_START_LO, va >> GPGPU_VM_PAGE_SHIFT);
	gpgpu_wreg64(dev, GPGPU_REG_VM_INV_VPN_END_LO, (va + size - 1) >> GPGPU_VM_PAGE_SHIFT);
	return vm_invalidate_req(dev, vm, GPGPU_VM_INV_VMID(vm->vmid) | GPGPU_VM_INV_RANGE);
}

/*
 * vm_set_ptes() - 按叶表范围写入或撤销一段 PTE
 *
 * MAP/UNMAP 持 dev->lock 调用，表页不会在函数内失锁。page 为地址域起点：
 * Host 对象是 IOVA，flags 带 SYSTEM；VRAM 对象是 VRAM 偏移。flags=0 撤销，
 * 不用 page=0 判断，因为 IOVA 0 也合法。
 *
 * 一张叶表覆盖 span，表内相邻 PTE 相差 sizeof(u64) 字节。例如从
 * VA=0x1f_f000 修改两页时，先写本表最后一项，再重新定位下一张叶表。
 *
 * 1. 每段只沿目录找到一次叶子位置，缺失的撤销段直接跳过；
 * 2. 在同一叶表内逐页写入“物理页 | VALID | flags”或零，再进入下一段。
 *
 * 函数不失效 TLB，交由调用者在写完后执行；失败前的写入不回退。
 *
 * Return: 0 表示成功，否则为表页分配或查找错误。
 */
static int vm_set_ptes(struct gpgpu_device *dev, struct gpgpu_vm *vm, u64 va, u64 size,
		       u64 page, u64 flags)
{
	const u64 span = 1ULL << GPGPU_VM_LEVEL_SHIFT(GPGPU_VM_LEVELS - 2);
	u64 done = 0;

	while (done < size) {
		u64 addr = va + done;
		u64 chunk = min(size - done, span - (addr & (span - 1)));
		u32 leaf;
		int ret;

		/* 1. 目录查找只做一次，后续不跨越这张叶表的末尾。 */
		ret = vm_leaf(dev, vm, addr, flags != 0, &leaf);
		if (ret && (ret != -ENOENT || flags))
			return ret;

		/* 2. 相邻页只前进 PTE 偏移，不重复读取三级目录。 */
		if (!ret) {
			for (u64 i = 0; i < chunk; i += PAGE_SIZE) {
				u64 value = flags ? (page + done + i) | GPGPU_VM_VALID | flags : 0;

				writeq(value, dev->vram + leaf + (i >> PAGE_SHIFT) * sizeof(value));
			}
		}
		done += chunk;
	}
	return 0;
}

/*
 * vm_init() - 第一次绑定时建立地址空间
 *
 * 会话第一次 MAP 时调用，之后直接返回（VM 已失败时返回 -EIO）。这里不取
 * VMID：VMID 到建队列时才由 gpgpu_vm_attach() 取得，页表根也在那时写进
 * 上下文寄存器。gpuvm=on 时分配根表页；gpuvm=off 时设备直接使用物理地址，
 * 不需要页表。
 *
 * 根表分配失败时 VM 保持未建立，不进入 failed：此时还没有绑定，根也
 * 没有交给设备，下次 MAP 重新分配。
 */
static int vm_init(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	int ret;

	if (vm->ready)
		return vm->failed ? -EIO : 0;
	if (dev->gpuvm) {
		ret = vm_table_alloc(dev, vm, &vm->root);
		if (ret)
			return ret;
	}
	vm->ready = true;
	return 0;
}

/*
 * 为 [va, va + size) 建好全部中间级表页，不写叶子项。一张 L3 表覆盖
 * 一个 L2 项的 2 MiB，所以从 va 向下对齐到这个边界，再逐段取叶子位置。
 * 例：va = 0x1f_f000、size = 0x2000 时依次准备 0x0 与 0x20_0000 两段，
 * 跨界的第二页也有叶表。失败时已建的空表页留在 tables 中，叶子项
 * 不变。
 */
static int vm_prepare_tables(struct gpgpu_device *dev, struct gpgpu_vm *vm, u64 va, u64 size)
{
	const u64 span = 1ULL << GPGPU_VM_LEVEL_SHIFT(GPGPU_VM_LEVELS - 2);

	for (u64 addr = ALIGN_DOWN(va, span); addr < va + size; addr += span) {
		u32 leaf;
		int ret = vm_leaf(dev, vm, addr, true, &leaf);

		if (ret)
			return ret;
	}
	return 0;
}

/* 用户 GPUVA 区间：页对齐，不含零页，整段低于 GPUVA 上限 2^48。 */
static bool vm_range_ok(u64 va, u64 size)
{
	return va >= PAGE_SIZE && !(va & (PAGE_SIZE - 1)) && size &&
		size <= GPGPU_VM_VA_LIMIT && va <= GPGPU_VM_VA_LIMIT - size;
}

/* [va, va + size) 是否与会话中已绑定的对象重叠。 */
static bool vm_overlaps(struct gpgpu_fpriv *fpriv, u64 va, u64 size)
{
	struct gpgpu_bo *bo;

	list_for_each_entry(bo, &fpriv->bos, node)
		if (bo->bound && va < bo->gpu_addr + bo->mapped_size && bo->gpu_addr < va + size)
			return true;
	return false;
}

/*
 * map_args_valid() - 检查 MAP 参数
 *
 * 对象未绑定；权限非空且只含 R/W/X，通知页还必须可写（CP 经用户域
 * 写通知槽）；gpuvm=on 时 GPUVA 合法且不与已有绑定重叠，gpuvm=off 时
 * 地址必须为 0（由驱动返回物理地址）。
 */
static int map_args_valid(struct gpgpu_fpriv *fpriv, struct gpgpu_bo *bo,
			  const struct gpgpu_vm_map *args)
{
	if (!bo || bo->bound || args->reserved || !args->permissions ||
	    (args->permissions & ~GPGPU_VM_PERMS))
		return -EINVAL;
	if (bo == fpriv->vm.notify && !(args->permissions & GPGPU_VM_WRITE))
		return -EINVAL;
	if (!fpriv->dev->gpuvm)
		return args->address ? -EOPNOTSUPP : 0;
	if (!vm_range_ok(args->address, bo->mapped_size))
		return -EINVAL;
	return vm_overlaps(fpriv, args->address, bo->mapped_size) ? -EEXIST : 0;
}

/**
 * gpgpu_vm_map() - MAP：把整个对象绑定到 GPU 地址
 * @fpriv: 会话；调用者持 dev->lock。
 * @args: ioctl 参数；输出最终 GPU 地址。
 *
 * 1. VM 已失败时返回 -EBUSY；再检查对象、权限和 GPUVA 区间；
 * 2. 第一次绑定时建立地址空间（gpuvm=on 时为根表），不取 VMID；
 * 3. gpuvm=on 时建好这段 GPUVA 的全部中间级表页。第 2、3 步分配失败时
 *    返回 -ENOSPC 或 -ENOMEM，对象保持未绑定，VM 仍可使用，调用者释放
 *    空间后可以重试；已建的空表页留在 VM 中，会话清理时归还；
 * 4. 记录绑定地址并取得绑定引用：此后即使句柄关闭，对象和存储
 *    也保留到 UNMAP 或会话清理。gpuvm=off 时 GPU 地址就是物理地址，
 *    到此结束；
 * 5. 写叶子 PTE。队列驻留时 VM 占有 VMID，再按范围同步失效 TLB；
 *    不占 VMID 时由下次取得 VMID 时的整体失效清除旧翻译。失效未确认时
 *    返回 -EIO，VM 进入 failed，无法确认设备看到的映射，绑定引用保留到
 *    会话清理。
 */
int gpgpu_vm_map(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;
	struct gpgpu_bo *bo = gpgpu_bo_lookup(fpriv, args->handle);
	u64 flags;
	int ret;

	/* 1. 先检查 VM 状态，再检查参数。 */
	if (vm->failed)
		return -EBUSY;
	ret = map_args_valid(fpriv, bo, args);
	if (ret)
		return ret;

	/* 2. 首次绑定时建立地址空间，gpuvm=on 时分配根表。 */
	ret = vm_init(dev, vm);
	if (ret)
		return ret;

	/* 3. 绑定前分配全部表页，失败时不留下这个对象的任何叶子项。 */
	if (dev->gpuvm) {
		ret = vm_prepare_tables(dev, vm, args->address, bo->mapped_size);
		if (ret)
			return ret;
	}

	/* 4. 绑定引用让对象在句柄关闭后继续存活。 */
	bo->gpu_addr = dev->gpuvm ? args->address : bo->phys_addr;
	bo->permissions = args->permissions;
	bo->bound = true;
	drm_gem_object_get(&bo->gem);
	args->address = bo->gpu_addr;

	if (!dev->gpuvm)
		return 0;

	/*
	 * 5. 写叶子 PTE，再失效 TLB，设备才会看到新映射。Host 对象写
	 *    IOVA 并置 SYSTEM，VRAM 对象写 VRAM 偏移；bo->phys_addr 仍是
	 *    GPU 物理地址。不可缓存对象另置 UNCACHED。中间级已在第 3 步
	 *    建好，vm_set_ptes() 不再分配表页，不会失败。
	 */
	flags = args->permissions | (bo->uncached ? GPGPU_VM_UNCACHED : 0);
	if (bo->kind == GPGPU_MEMORY_HOST)
		vm_set_ptes(dev, vm, bo->gpu_addr, bo->mapped_size, bo->host.dma,
			    GPGPU_VM_SYSTEM | flags);
	else
		vm_set_ptes(dev, vm, bo->gpu_addr, bo->mapped_size, bo->phys_addr, flags);
	return vm_invalidate_range(dev, vm, bo->gpu_addr, bo->mapped_size);
}

/**
 * gpgpu_vm_unmap() - UNMAP：按 GPU 地址撤销一个绑定
 * @fpriv: 会话；调用者持 dev->lock。
 * @args: ioctl 参数；handle 与 permissions 必须为 0。
 *
 * 按地址查找，句柄已关闭的对象也能解绑。队列的 Ring 与进度对象
 * （users 非零）返回 -EBUSY。gpuvm=on 时先清 PTE；VM 占有 VMID 时再失效
 * 并读到 ACK，之后才放下绑定引用；句柄已关闭的对象同时放下列表引用。
 */
int gpgpu_vm_unmap(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *bo;
	int ret;

	if (fpriv->vm.failed || dev->dma_uncertain)
		return -EBUSY;
	if (args->handle || args->permissions || args->reserved)
		return -EINVAL;

	list_for_each_entry(bo, &fpriv->bos, node) {
		if (!bo->bound || bo->gpu_addr != args->address)
			continue;
		if (bo->users)
			return -EBUSY;

		/* 撤销时不分配表页，vm_set_ptes() 不会失败。 */
		if (dev->gpuvm) {
			vm_set_ptes(dev, &fpriv->vm, bo->gpu_addr, bo->mapped_size, 0, 0);
			ret = vm_invalidate_range(dev, &fpriv->vm, bo->gpu_addr, bo->mapped_size);
			if (ret)
				return ret;
		}

		bo->bound = false;
		bo->gpu_addr = 0;
		drm_gem_object_put(&bo->gem);
		if (!bo->handle)
			gpgpu_bo_drop(bo);
		return 0;
	}
	return -ENOENT;
}

/*
 * 停用上下文并失效 TLB，读回 ENABLE 为 0 且收到 ACK 后清零根地址。
 * 之后设备不会再经本 VM 翻译。
 */
static int vm_disable_translation(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	u32 ctx = GPGPU_REG_VM_CONTEXT(vm->vmid);

	gpgpu_wreg(dev, ctx + GPGPU_VM_ENABLE, 0);
	if (gpgpu_rreg(dev, ctx + GPGPU_VM_ENABLE) || vm_invalidate(dev, vm))
		return -EIO;
	gpgpu_wreg64(dev, ctx + GPGPU_VM_ROOT_LO, 0);
	return 0;
}

/**
 * gpgpu_vm_attach() - 为即将驻留的队列取得 VMID
 * @fpriv: 会话；调用者持 vm_serial 与 dev->lock，VM 已建立、未失败且没有
 *         存活队列。
 *
 * 建队列时在写 MQD 之前调用，MQD 中写入这里取得的编号：
 *
 * 1. 从 VMID 1、2 中取一个空闲的。没有空闲编号时返回 -ENOSPC，不改任何
 *    状态；
 * 2. gpuvm=on 时把根写进上下文寄存器，失效整个 VMID，再启用上下文。TLB 项
 *    按 VMID 标记，这个编号上一次可能属于别的会话，必须先清掉它留下的
 *    翻译。设备启用时检查根页合法，读回 ENABLE 确认已接受。gpuvm=off 时
 *    VMID 只标识队列来源，不配置上下文。
 *
 * 第 2 步的结果未知时 VM 进入 failed，VMID 保留到会话清理。
 *
 * Return: 0、-ENOSPC 或 -EIO。
 */
int gpgpu_vm_attach(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;
	u32 ctx;
	int ret;

	/* 1. 原子地占用第一个空闲的用户 VMID。 */
	for (vm->vmid = 1; vm->vmid <= GPGPU_VM_USERS; vm->vmid++)
		if (!test_and_set_bit(vm->vmid, &dev->vmids))
			break;
	if (vm->vmid > GPGPU_VM_USERS) {
		vm->vmid = 0;
		return -ENOSPC;
	}
	if (!dev->gpuvm)
		return 0;

	/* 2. 写根 → 失效整个 VMID → 启用并读回。 */
	ctx = GPGPU_REG_VM_CONTEXT(vm->vmid);
	gpgpu_wreg64(dev, ctx + GPGPU_VM_ROOT_LO, vm->root);
	ret = vm_invalidate(dev, vm);
	if (!ret) {
		gpgpu_wreg(dev, ctx + GPGPU_VM_ENABLE, 1);
		if (gpgpu_rreg(dev, ctx + GPGPU_VM_ENABLE) != 1)
			ret = -EIO;
	}
	if (ret)
		vm->failed = true;
	return ret;
}

/**
 * gpgpu_vm_detach() - 队列卸载后归还 VMID
 * @fpriv: 会话；调用者持 dev->lock，并已确认 CP 不再持有本 VM 的队列。
 *
 * gpuvm=on 时先停用上下文、失效整个 VMID，再清零根地址：这个编号随后
 * 可能交给别的会话，设备不会再经本 VM 翻译，TLB 中也不留本 VM 的翻译。
 * 停用或失效的结果未知时 VM 进入 failed，VMID 保留到会话清理时再试。
 * 没有占用 VMID 时直接返回。
 */
void gpgpu_vm_detach(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;

	if (!vm->vmid)
		return;
	if (dev->gpuvm && vm_disable_translation(dev, vm)) {
		vm->failed = true;
		return;
	}
	clear_bit(vm->vmid, &dev->vmids);
	vm->vmid = 0;
}

/**
 * gpgpu_vm_cleanup() - 会话结束时拆除地址空间
 * @fpriv: 会话；调用者持 dev->lock。
 *
 * 文件关闭时调用（gpgpu_postclose()、gpgpu_fpriv_destroy()）。必须先确认
 * 设备不会再经本 VM 访问内存，才能放下对象，归还页表页和 VMID。
 *
 * 1. 设备仍在运行时：有存活队列或 DMA 结果未知则返回 -EBUSY；否则先
 *    消费旧 IH，让通知和故障都处理完。VMID 通常已在队列卸载时归还；
 *    上次归还失败而仍占有 VMID 时，gpuvm=on 再停用上下文并失效 TLB。
 *    设备已停止时它不会再访问内存，直接回收；
 * 2. 释放全部事件，再撤销通知页登记：事件的通知槽都在通知页内；
 * 3. 放下全部绑定引用，没有其他引用的对象随之释放；
 * 4. 归还页表页与仍占有的 VMID。
 *
 * 还没有 MAP 过的会话没有地址空间，只撤销可能存在的通知页登记。
 *
 * Return: 0，或设备可能仍在访问时的 -EBUSY / -EIO（资源全部保留）。
 */
int gpgpu_vm_cleanup(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;
	struct gpgpu_vm_table *table, *next;
	struct gpgpu_bo *bo;

	if (!vm->ready) {
		gpgpu_vm_set_event_page(fpriv, NULL);
		return 0;
	}

	/* 1. 设备仍在运行时，先确认它不会再经本 VM 访问内存。 */
	if (!dev->stopped) {
		if (gpgpu_queue_any_live(fpriv) || dev->dma_uncertain)
			return -EBUSY;
		gpgpu_ih_drain(dev);
		if (vm->vmid && dev->gpuvm && vm_disable_translation(dev, vm))
			return -EIO;
	}

	/* 2. 事件的通知槽在通知页内，先释放事件再撤销登记。 */
	gpgpu_event_release_all(fpriv);
	gpgpu_vm_set_event_page(fpriv, NULL);

	/* 3. 放下每个已绑定对象的绑定引用。 */
	list_for_each_entry(bo, &fpriv->bos, node) {
		if (!bo->bound)
			continue;
		bo->bound = false;
		bo->gpu_addr = 0;
		drm_gem_object_put(&bo->gem);
	}

	/* 4. 归还页表页与 VMID。 */
	list_for_each_entry_safe(table, next, &vm->tables, node) {
		gpgpu_vram_free(dev, &table->blocks);
		list_del(&table->node);
		kfree(table);
	}
	if (vm->vmid)
		clear_bit(vm->vmid, &dev->vmids);
	vm->vmid = 0;
	vm->root = 0;
	vm->ready = false;
	return 0;
}

/**
 * gpgpu_vm_set_event_page() - 登记或撤销会话的通知页
 * @fpriv: 会话；调用者持 dev->lock。
 * @bo: 要登记的 Host 对象；NULL 表示撤销当前登记。
 *
 * 登记期间通知页持有一个 users 计数：UNMAP 返回 -EBUSY，关闭句柄也不放下
 * 对象。撤销前调用者须确认已没有事件使用其中的通知槽。
 */
void gpgpu_vm_set_event_page(struct gpgpu_fpriv *fpriv, struct gpgpu_bo *bo)
{
	struct gpgpu_vm *vm = &fpriv->vm;

	if (vm->notify)
		vm->notify->users--;
	vm->notify = bo;
	if (bo)
		bo->users++;
}

/* 找到 q 当前所在的用户槽；不在任何槽上返回 -1。 */
static int vm_queue_slot(struct gpgpu_device *dev, struct gpgpu_queue *q)
{
	for (u32 slot = GPGPU_HQD_USER_FIRST; slot < GPGPU_HQD_SLOTS; slot++)
		if (gpgpu_hqd_mqd(dev, slot) == q->mqd.addr)
			return slot;
	return -1;
}

/**
 * gpgpu_vm_save_fault() - 保存队列所在槽的首次 GPUVM 故障
 * @q: 队列；调用者持 dev->lock。
 *
 * 槽被复用前把故障复制到队列对象，卸载后仍能查询。已保存过则不再更新。
 */
void gpgpu_vm_save_fault(struct gpgpu_queue *q)
{
	struct gpgpu_device *dev = q->fpriv->dev;
	u32 base;
	int slot;

	if (!dev->present || q->fault.reason)
		return;
	slot = vm_queue_slot(dev, q);
	if (slot < 0)
		return;

	base = GPGPU_REG_VM_FAULT(slot);
	q->fault = (struct gpgpu_vm_fault) {
		.version = GPGPU_DRM_VERSION,
		.queue_id = q->id,
		.reason = gpgpu_rreg(dev, base + GPGPU_VM_FAULT_REASON),
		.vmid = gpgpu_rreg(dev, base + GPGPU_VM_FAULT_VMID),
		.address = gpgpu_rreg(dev, base + GPGPU_VM_FAULT_ADDR_LO) |
			   (u64)gpgpu_rreg(dev, base + GPGPU_VM_FAULT_ADDR_HI) << 32,
		.access = gpgpu_rreg(dev, base + GPGPU_VM_FAULT_ACCESS),
		.phase = gpgpu_rreg(dev, base + GPGPU_VM_FAULT_PHASE),
	};
}

/*
 * gpgpu_vm_clear_free_faults() - 建队列前清除空闲用户槽上的旧故障
 *
 * 先消费旧 IH，确保旧队列的故障已保存；只清既未激活也未绑定 MQD 的槽。
 */
void gpgpu_vm_clear_free_faults(struct gpgpu_device *dev)
{
	gpgpu_ih_drain(dev);
	for (u32 slot = GPGPU_HQD_USER_FIRST; slot < GPGPU_HQD_SLOTS; slot++)
		if (!gpgpu_hqd_read(dev, slot, GPGPU_HQD_ACTIVE) && !gpgpu_hqd_mqd(dev, slot))
			gpgpu_wreg(dev, GPGPU_REG_VM_FAULT(slot) + GPGPU_VM_FAULT_CLEAR, 1);
}
