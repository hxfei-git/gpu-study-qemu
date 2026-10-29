// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：GPU 地址空间
 *
 * 每个会话在第一次绑定对象时取得一个用户 VMID 和一页通知页：
 *   gpuvm=on   建立三级页表（表页从 VRAM 分配），通知页映射到 GPGPU_VM_NOTIFY_VA；
 *              对象绑定到应用给出的 GPUVA；
 *   gpuvm=off  不建页表，对象的 GPU 地址就是它的 GPU 物理地址。
 *
 * 修改页表后必须同步失效 TLB 并读到 ACK，才能放下旧映射的引用。失效失败时
 * 无法确认设备是否仍在使用旧映射，VM 标记为 failed，保留全部资源。
 * 用户队列存活期间冻结整个地址空间。
 */
#include <linux/slab.h>
#include "gpgpu_drv.h"

/* 页表中间级的移位与索引宽度：L0 由 VA[31:30] 索引 4 项，L1 由 VA[29:21] 索引 512 项。 */
static const unsigned int vm_dir_shift[2] = { 30, 21 };
static const unsigned int vm_dir_mask[2] = { 3, 511 };

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

static int vm_table_alloc(struct gpgpu_device *dev, struct gpgpu_vm *vm, u32 *offset)
{
	struct gpgpu_vm_table *table = kzalloc(sizeof(*table), GFP_KERNEL);
	int ret;

	if (!table)
		return -ENOMEM;
	INIT_LIST_HEAD(&table->blocks);
	ret = gpgpu_vram_alloc(dev, PAGE_SIZE, &table->blocks, &table->offset);
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
 */
static int vm_leaf(struct gpgpu_device *dev, struct gpgpu_vm *vm, u32 va, bool create,
		   u32 *offset)
{
	u32 table = vm->root;

	for (unsigned int level = 0; level < ARRAY_SIZE(vm_dir_shift); level++) {
		u32 entry_at = table + ((va >> vm_dir_shift[level]) & vm_dir_mask[level]) * 8;
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
	*offset = table + ((va >> PAGE_SHIFT) & 511) * 8;
	return 0;
}

/* 同步失效本 VM 的 TLB；读不到 ACK 时 VM 进入 failed。调用者持 dev->lock。 */
static int vm_invalidate(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	wmb(); /* 经 BAR2 写入的页表项先于失效请求对设备可见。 */
	gpgpu_wreg(dev, GPGPU_REG_VM_INVALIDATE, vm->vmid);
	if (gpgpu_rreg(dev, GPGPU_REG_VM_STATUS) == GPGPU_VM_ACK)
		return 0;
	vm->failed = true;
	return -EIO;
}

/* 写 PTE：target 为 0 表示撤销映射。 */
static int vm_set_ptes(struct gpgpu_device *dev, struct gpgpu_vm *vm, u64 va, u64 size,
		       u64 target, u32 permissions)
{
	for (u64 i = 0; i < size; i += PAGE_SIZE) {
		u32 leaf;
		int ret = vm_leaf(dev, vm, va + i, target != 0, &leaf);

		if (ret == -ENOENT && !target)
			continue;
		if (ret)
			return ret;
		writeq(target ? (target + i) | GPGPU_VM_VALID | permissions : 0, dev->vram + leaf);
	}
	return 0;
}

/* gpuvm=on：建页表根，映射通知页，写根并失效，最后启用上下文。 */
static int vm_enable_translation(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	u32 ctx = GPGPU_REG_VM_CONTEXT(vm->vmid);
	int ret;

	ret = vm_table_alloc(dev, vm, &vm->root);
	if (!ret)
		ret = vm_set_ptes(dev, vm, GPGPU_VM_NOTIFY_VA, PAGE_SIZE, vm->notify.addr,
				  GPGPU_VM_READ | GPGPU_VM_WRITE);
	if (ret)
		return ret;
	gpgpu_wreg64(dev, ctx + GPGPU_VM_ROOT_LO, vm->root);
	ret = vm_invalidate(dev, vm);
	if (ret)
		return ret;
	gpgpu_wreg(dev, ctx + GPGPU_VM_ENABLE, 1);
	return gpgpu_rreg(dev, ctx + GPGPU_VM_ENABLE) == 1 ? 0 : -EIO;
}

/*
 * vm_bind() - 第一次绑定时建立地址空间
 *
 * 1. 取得空闲的用户 VMID；2. 分配通知页；3. gpuvm=on 时启用翻译。
 * 第 2、3 步失败后 VM 进入 failed，资源留到会话清理时回收。
 */
static int vm_bind(struct gpgpu_device *dev, struct gpgpu_vm *vm)
{
	int ret;

	if (vm->vmid)
		return vm->failed ? -EIO : 0;

	/* 1. VMID。 */
	for (vm->vmid = 1; vm->vmid <= GPGPU_VM_USERS; vm->vmid++)
		if (!test_and_set_bit(vm->vmid, &dev->vmids))
			break;
	if (vm->vmid > GPGPU_VM_USERS) {
		vm->vmid = 0;
		return -ENOSPC;
	}

	/* 2. 通知页。 */
	ret = gpgpu_dma_buf_alloc(dev, &vm->notify, PAGE_SIZE, PAGE_SIZE);

	/* 3. 翻译。 */
	if (!ret && dev->gpuvm)
		ret = vm_enable_translation(dev, vm);
	if (ret)
		vm->failed = true;
	return ret;
}

/* 用户 GPUVA 区间：页对齐，不含零页与末尾的通知页。 */
static bool vm_range_ok(u64 va, u64 size)
{
	return va >= PAGE_SIZE && !(va & (PAGE_SIZE - 1)) && size && size <= GPGPU_VM_NOTIFY_VA &&
		va <= GPGPU_VM_NOTIFY_VA - size;
}

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
 * 对象未绑定；权限非空且只含 R/W/X；gpuvm=on 时 GPUVA 合法且不与已有绑定
 * 重叠，gpuvm=off 时地址必须为 0（由驱动返回物理地址）。
 */
static int map_args_valid(struct gpgpu_fpriv *fpriv, struct gpgpu_bo *bo,
			  const struct gpgpu_vm_map *args)
{
	if (!bo || bo->bound || args->reserved || !args->permissions ||
	    (args->permissions & ~GPGPU_VM_PERMS))
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
 * 1. 检查参数；队列存活期间冻结地址空间；
 * 2. 建立地址空间（首次）；
 * 3. 取得绑定引用并记录地址；
 * 4. gpuvm=on 时写 PTE 并失效 TLB。写入或失效失败时 VM 进入 failed，
 *    绑定引用保留到会话清理。
 */
int gpgpu_vm_map(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;
	struct gpgpu_bo *bo = gpgpu_bo_lookup(fpriv, args->handle);
	int ret;

	/* 1. 检查。 */
	if (vm->failed || gpgpu_queue_any_live(fpriv))
		return -EBUSY;
	ret = map_args_valid(fpriv, bo, args);
	if (ret)
		return ret;

	/* 2. 地址空间。 */
	ret = vm_bind(dev, vm);
	if (ret)
		return ret;

	/* 3. 绑定。 */
	bo->gpu_addr = dev->gpuvm ? args->address : bo->phys_addr;
	bo->permissions = args->permissions;
	bo->bound = true;
	drm_gem_object_get(&bo->gem);
	args->address = bo->gpu_addr;
	if (!dev->gpuvm)
		return 0;

	/* 4. 页表。 */
	ret = vm_set_ptes(dev, vm, bo->gpu_addr, bo->mapped_size, bo->phys_addr,
			  args->permissions);
	if (ret) {
		vm->failed = true;
		return ret;
	}
	return vm_invalidate(dev, vm);
}

/**
 * gpgpu_vm_unmap() - UNMAP：按 GPU 地址撤销一个绑定
 * @fpriv: 会话；调用者持 dev->lock。
 * @args: ioctl 参数；handle 与 permissions 必须为 0。
 *
 * 按地址查找，句柄已关闭的对象也能解绑。gpuvm=on 时先清 PTE 并失效，
 * 读到 ACK 后才放下绑定引用；句柄已关闭的对象同时放下列表引用。
 */
int gpgpu_vm_unmap(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_bo *bo;
	int ret;

	if (fpriv->vm.failed || gpgpu_queue_any_live(fpriv) || dev->dma_uncertain)
		return -EBUSY;
	if (args->handle || args->permissions || args->reserved)
		return -EINVAL;
	list_for_each_entry(bo, &fpriv->bos, node) {
		if (!bo->bound || bo->gpu_addr != args->address)
			continue;
		if (bo->users)
			return -EBUSY;
		if (dev->gpuvm) {
			vm_set_ptes(dev, &fpriv->vm, bo->gpu_addr, bo->mapped_size, 0, 0);
			ret = vm_invalidate(dev, &fpriv->vm);
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

/* 停用上下文并失效，之后设备不会再经本 VM 翻译。 */
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
 * gpgpu_vm_cleanup() - 会话结束时拆除地址空间
 * @fpriv: 会话；调用者持 dev->lock。
 *
 * 1. 设备仍在运行时：没有存活队列和不确定的 DMA，先消费旧 IH，再停用上下文；
 *    设备已停止时直接回收；
 * 2. 释放事件（通知槽在通知页内）；
 * 3. 放下全部绑定引用；
 * 4. 归还页表页、通知页与 VMID。
 *
 * Return: 0，或设备可能仍在访问时的 -EBUSY / -EIO（资源全部保留）。
 */
int gpgpu_vm_cleanup(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_vm *vm = &fpriv->vm;
	struct gpgpu_vm_table *table, *next;
	struct gpgpu_bo *bo;

	if (!vm->vmid)
		return 0;

	/* 1. 停止使用。 */
	if (!dev->stopped) {
		if (gpgpu_queue_any_live(fpriv) || dev->dma_uncertain)
			return -EBUSY;
		gpgpu_ih_drain(dev);
		if (dev->gpuvm && vm_disable_translation(dev, vm))
			return -EIO;
	}

	/* 2. 事件。 */
	gpgpu_event_release_all(fpriv);

	/* 3. 绑定。 */
	list_for_each_entry(bo, &fpriv->bos, node) {
		if (!bo->bound)
			continue;
		bo->bound = false;
		bo->gpu_addr = 0;
		drm_gem_object_put(&bo->gem);
	}

	/* 4. 存储。 */
	list_for_each_entry_safe(table, next, &vm->tables, node) {
		gpgpu_vram_free(dev, &table->blocks);
		list_del(&table->node);
		kfree(table);
	}
	gpgpu_dma_buf_free(dev, &vm->notify);
	clear_bit(vm->vmid, &dev->vmids);
	vm->vmid = 0;
	vm->root = 0;
	return 0;
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
