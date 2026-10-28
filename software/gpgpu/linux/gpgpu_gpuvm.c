// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "gpgpu_queue_drv.h"

/* 正常、错误或结果未知的驻留队列都冻结映射。 */
bool gpgpu_vm_queue_live(struct gpgpu_qclient *c)
{
	struct gpgpu_user_queue *q;

	list_for_each_entry(q, &c->queues, node) {
		if (!q->dead)
			return true;
	}
	return false;
}

#if IS_REACHABLE(CONFIG_DRM) && IS_REACHABLE(CONFIG_GPU_BUDDY)
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>

/**
 * struct gpgpu_table - 驱动私有表页，与用户 BO 分开登记
 * @node: 所属 VM 的表页链表节点。
 * @blocks: 此页持有的 buddy 分配块。
 * @offset: 4 KiB 对齐的 VRAM 字节偏移。
 */
struct gpgpu_table {
	struct list_head node;
	struct list_head blocks;
	u32 offset;
};

/* 存储最后释放可能来自 VMA 或句柄关闭，独立持有设备引用。 */
static void bo_free(struct drm_gem_object *obj)
{
	struct gpgpu_qmemory *m = container_of(obj, struct gpgpu_qmemory, gem);
	struct gpgpu_qdev *dev = m->dev;
	struct drm_device *drm = obj->dev;

	mutex_lock(&dev->lock);
	drm_gem_object_release(obj);
	gpgpu_memory_storage_free(m);
	mutex_unlock(&dev->lock);
	drm_dev_put(drm);
	gpgpu_qdev_put(dev);
}

/* 调用方持锁；移除列表引用后，剩余 GEM 引用自行决定存储寿命。 */
void gpgpu_gem_drop(struct gpgpu_qmemory *m)
{
	struct gpgpu_qdev *dev = m->dev;

	list_del_init(&m->node);
	m->client = NULL;
	mutex_unlock(&dev->lock);
	drm_gem_object_put(&m->gem);
	mutex_lock(&dev->lock);
}

/* 句柄关闭不撤销 GPU 映射；已映射对象由映射及列表引用继续保活。 */
static void bo_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct gpgpu_qmemory *m = container_of(obj, struct gpgpu_qmemory, gem);
	struct gpgpu_qdev *dev = m->dev;

	mutex_lock(&dev->lock);
	m->id = 0;
	if (m->client && !m->bound && !m->users)
		gpgpu_gem_drop(m);
	mutex_unlock(&dev->lock);
}

/* 首版不共享对象，显式阻止 GEM 核心的默认导出回退。 */
static struct dma_buf *bo_export(struct drm_gem_object *obj, int flags)
{
	return ERR_PTR(-EOPNOTSUPP);
}

/* 外部存储不进入固定存储池，拒绝 DMA-BUF 导入。 */
static struct drm_gem_object *bo_import(struct drm_device *drm, struct dma_buf *buf)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct vm_operations_struct bo_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* GEM 核心先检查文件授权并取得 VMA 引用，本回调只建立一致性映射。 */
static int bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct gpgpu_qmemory *m = container_of(obj, struct gpgpu_qmemory, gem);
	unsigned long length = vma->vm_end - vma->vm_start;
	int ret;

	if (m->kind != GPGPU_MEMORY_HOST || length != m->mapped_size ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	vma->vm_pgoff = 0;
	vm_flags_clear(vma, VM_PFNMAP | VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	ret = dma_mmap_coherent(&m->dev->pdev->dev, vma, m->cpu, m->dma, m->size);
	if (!ret)
		vma->vm_ops = &bo_vm_ops;
	return ret;
}

static const struct drm_gem_object_funcs bo_funcs = {
	.free = bo_free,
	.close = bo_close,
	.export = bo_export,
	.mmap = bo_mmap,
};

/* 创建引用先保留在会话列表，GEM handle 另持有一个引用。 */
static int bo_create(struct gpgpu_qclient *c, struct gpgpu_bo_create *r)
{
	struct gpgpu_bo_create req = {
		.kind = r->kind,
		.size = r->size,
		.alignment = r->alignment,
	};
	struct gpgpu_qmemory *m;
	u32 handle;
	int ret;

	if (r->reserved || c->vm_failed || gpgpu_vm_queue_live(c))
		return -EBUSY;
	ret = gpgpu_memory_alloc(c, &req);
	if (ret)
		return ret;
	m = gpgpu_memory_find(c, req.handle);
	m->gem.funcs = &bo_funcs;
	drm_gem_private_object_init(c->dev->drm, &m->gem, m->mapped_size);
	/* private GEM 不自动持有 drm_device，orphan 必须保护其 mmap 管理器。 */
	drm_dev_get(c->dev->drm);
	m->gem_ready = true;
	kref_get(&c->dev->refs);
	ret = drm_gem_create_mmap_offset(&m->gem);
	if (ret)
		goto fail;
	/* GEM 句柄失败可能调用 close；避免持设备锁进入其回调。 */
	drm_gem_object_get(&m->gem);
	mutex_unlock(&c->dev->lock);
	ret = drm_gem_handle_create(c->drm_file, &m->gem, &handle);
	mutex_lock(&c->dev->lock);
	/* 其他线程可能已关闭刚创建的句柄；先核对列表引用，再放临时引用。 */
	if (!ret && !m->client)
		ret = -ENOENT;
	if (ret) {
		if (m->client)
			gpgpu_gem_drop(m);
		mutex_unlock(&c->dev->lock);
		drm_gem_object_put(&m->gem);
		mutex_lock(&c->dev->lock);
		return ret;
	}
	drm_gem_object_put(&m->gem);
	m->id = handle;
	r->handle = handle;
	r->mapped_size = m->mapped_size;
	r->mmap_offset =
		m->kind == GPGPU_MEMORY_HOST ? drm_vma_node_offset_addr(&m->gem.vma_node) : 0;
	return 0;
fail:
	if (m->client)
		gpgpu_gem_drop(m);
	return ret;
}

/* 分配清零表页，表页寿命覆盖整个 VM，避免部分撤销时拆除共享目录。 */
static int table_alloc(struct gpgpu_qclient *c, u32 *offset)
{
	struct gpgpu_table *table = kzalloc(sizeof(*table), GFP_KERNEL);
	struct gpu_buddy_block *block;
	int ret;

	if (!table)
		return -ENOMEM;
	INIT_LIST_HEAD(&table->blocks);
	ret = gpu_buddy_alloc_blocks(&c->dev->buddy, PAGE_SIZE, GPGPU_VRAM_USABLE_SIZE, PAGE_SIZE,
				     PAGE_SIZE, &table->blocks, GPU_BUDDY_CONTIGUOUS_ALLOCATION);
	if (ret) {
		kfree(table);
		return ret;
	}
	block = list_first_entry(&table->blocks, struct gpu_buddy_block, link);
	table->offset = gpu_buddy_block_offset(block);
	memset_io(c->dev->vram + table->offset, 0, PAGE_SIZE);
	list_add_tail(&table->node, &c->tables);
	*offset = table->offset;
	return 0;
}

/* 查找叶项地址；创建时仅从驱动自己的页表路径分配下级页。 */
static int vm_leaf(struct gpgpu_qclient *c, u32 va, bool create, u32 *offset)
{
	u32 table = c->root;
	unsigned int shifts[] = { 30, 21 };

	for (unsigned int i = 0; i < 2; i++) {
		u32 entry_addr = table + ((va >> shifts[i]) & (i ? 511 : 3)) * 8;
		u64 entry = readq(c->dev->vram + entry_addr);
		int ret;

		if (!(entry & GPGPU_VM_VALID)) {
			if (!create)
				return -ENOENT;
			ret = table_alloc(c, &table);
			if (ret)
				return ret;
			writeq((u64)table | GPGPU_VM_VALID, c->dev->vram + entry_addr);
		} else {
			table = entry & GPGPU_VM_ADDR_MASK;
		}
	}
	*offset = table + ((va >> 12) & 511) * 8;
	return 0;
}

/* 失效请求由设备锁串行，必须读到本次 ACK 才允许解除映射引用。 */
static int vm_invalidate(struct gpgpu_qclient *c)
{
	u32 status;

	wmb();
	writel(c->vmid, c->dev->regs + GPGPU_REG_VM_INVALIDATE);
	status = readl(c->dev->regs + GPGPU_REG_VM_STATUS);
	if (status != GPGPU_VM_ACK) {
		c->vm_failed = true;
		return -EIO;
	}
	return 0;
}

/* 首次绑定取得队列来源编号，并为两种模式建立内部通知页。 */
static int vm_bind(struct gpgpu_qclient *c)
{
	struct gpgpu_qdev *dev = c->dev;
	u32 leaf, base;
	int ret;

	if (c->vmid)
		return c->vm_failed ? -EIO : 0;
	for (c->vmid = 1; c->vmid <= GPGPU_VM_USERS; c->vmid++)
		if (!test_and_set_bit(c->vmid, &dev->vmids))
			break;
	if (c->vmid > GPGPU_VM_USERS) {
		c->vmid = 0;
		return -ENOSPC;
	}
	c->notify_cpu = dma_alloc_coherent(&dev->pdev->dev, PAGE_SIZE, &c->notify_dma, GFP_KERNEL);
	if (!c->notify_cpu) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(c->notify_cpu, 0, PAGE_SIZE);
	if ((c->notify_dma & (PAGE_SIZE - 1)) ||
	    !gpgpu_dma_address(dev, c->notify_dma, PAGE_SIZE, &c->notify_address)) {
		ret = -ERANGE;
		goto fail;
	}
	if (!dev->gpuvm)
		return 0;
	ret = table_alloc(c, &c->root);
	if (ret)
		goto fail;
	ret = vm_leaf(c, GPGPU_VM_NOTIFY_VA, true, &leaf);
	if (ret)
		goto fail;
	writeq(c->notify_address | GPGPU_VM_VALID | GPGPU_VM_READ | GPGPU_VM_WRITE,
	       dev->vram + leaf);
	base = GPGPU_REG_VM_CONTEXT(c->vmid);
	gpgpu_qwrite64(dev, base + GPGPU_VM_ROOT_LO, c->root);
	ret = vm_invalidate(c);
	if (ret)
		goto fail;
	writel(1, dev->regs + base + GPGPU_VM_ENABLE);
	if (readl(dev->regs + base + GPGPU_VM_ENABLE) != 1) {
		ret = -EIO;
		goto fail;
	}
	return 0;
fail:
	c->vm_failed = true;
	return ret;
}

/* 普通 GPUVA 仅排除零页和内部通知末页。 */
static bool vm_range(u64 address, u64 size)
{
	return address >= PAGE_SIZE && !(address & (PAGE_SIZE - 1)) && size &&
		size <= GPGPU_VM_NOTIFY_VA && address <= GPGPU_VM_NOTIFY_VA - size;
}

/* 两种模式都取得绑定引用；写表或失效失败继续保留到安全清理。 */
static int vm_map(struct gpgpu_qclient *c, struct gpgpu_vm_map *r)
{
	struct gpgpu_qmemory *m = gpgpu_memory_find(c, r->handle), *other;
	u64 target;
	int ret;

	if (c->vm_failed || gpgpu_vm_queue_live(c))
		return -EBUSY;
	if (!m || !m->gem_ready || m->bound || r->reserved || !r->permissions ||
	    (r->permissions & ~GPGPU_VM_PERMS))
		return -EINVAL;
	if (!c->dev->gpuvm && r->address)
		return -EOPNOTSUPP;
	if (c->dev->gpuvm && !vm_range(r->address, m->mapped_size))
		return -EINVAL;
	if (c->dev->gpuvm) {
		list_for_each_entry(other, &c->memory, node) {
			if (other->bound && r->address < other->gpu_address + other->mapped_size &&
			    other->gpu_address < r->address + m->mapped_size)
				return -EEXIST;
		}
	}
	target = m->physical_address;
	if ((target & (PAGE_SIZE - 1)) || target >= (1ULL << 32) ||
	    m->mapped_size > (1ULL << 32) - target)
		return -ERANGE;
	ret = vm_bind(c);
	if (ret)
		return ret;
	m->gpu_address = c->dev->gpuvm ? r->address : target;
	m->permissions = r->permissions;
	m->bound = true;
	drm_gem_object_get(&m->gem);
	r->address = m->gpu_address;
	if (!c->dev->gpuvm)
		return 0;
	for (u64 i = 0; i < m->mapped_size; i += PAGE_SIZE) {
		u32 leaf;

		ret = vm_leaf(c, r->address + i, true, &leaf);
		if (ret) {
			c->vm_failed = true;
			return ret;
		}
		writeq((target + i) | GPGPU_VM_VALID | r->permissions, c->dev->vram + leaf);
	}
	return vm_invalidate(c);
}

/* 地址定位独立绑定，允许 GEM handle 已关闭后再安全解绑。 */
static int vm_unmap(struct gpgpu_qclient *c, struct gpgpu_vm_map *r)
{
	struct gpgpu_qmemory *m;
	int ret;

	if (c->vm_failed || gpgpu_vm_queue_live(c) || c->dev->dma_uncertain)
		return -EBUSY;
	if (r->handle || r->permissions || r->reserved)
		return -EINVAL;
	list_for_each_entry(m, &c->memory, node) {
		if (!m->bound || m->gpu_address != r->address)
			continue;
		if (m->users)
			return -EBUSY;
		if (c->dev->gpuvm) {
			for (u64 i = 0; i < m->mapped_size; i += PAGE_SIZE) {
				u32 leaf;

				if (!vm_leaf(c, r->address + i, false, &leaf))
					writeq(0, c->dev->vram + leaf);
			}
			ret = vm_invalidate(c);
			if (ret)
				return ret;
		}
		m->bound = false;
		m->gpu_address = 0;
		drm_gem_object_put(&m->gem); /* 列表引用仍在。 */
		if (!m->id)
			gpgpu_gem_drop(m);
		return 0;
	}
	return -ENOENT;
}

/* 仅在队列、DMA 与旧通知都结束后，撤销绑定并释放内部存储。 */
int gpgpu_vm_cleanup(struct gpgpu_qclient *c)
{
	struct gpgpu_qdev *dev = c->dev;
	struct gpgpu_qmemory *m;
	struct gpgpu_table *table, *next;
	u32 base;

	if (!c->vmid)
		return 0;
	if (!dev->stopped) {
		if (gpgpu_vm_queue_live(c) || dev->dma_uncertain)
			return -EBUSY;
		gpgpu_ih_drain_locked(dev);
		if (dev->gpuvm) {
			base = GPGPU_REG_VM_CONTEXT(c->vmid);
			writel(0, dev->regs + base + GPGPU_VM_ENABLE);
			if (readl(dev->regs + base + GPGPU_VM_ENABLE) || vm_invalidate(c))
				return -EIO;
			gpgpu_qwrite64(dev, base + GPGPU_VM_ROOT_LO, 0);
		}
	}
	gpgpu_event_release_all(c);
	list_for_each_entry(m, &c->memory, node) {
		if (m->bound) {
			m->bound = false;
			m->gpu_address = 0;
			drm_gem_object_put(&m->gem);
		}
	}
	list_for_each_entry_safe(table, next, &c->tables, node) {
		gpu_buddy_free_list(&dev->buddy, &table->blocks, 0);
		list_del(&table->node);
		kfree(table);
	}
	if (c->notify_cpu)
		dma_free_coherent(&dev->pdev->dev, PAGE_SIZE, c->notify_cpu, c->notify_dma);
	c->notify_cpu = NULL;
	c->notify_address = 0;
	clear_bit(c->vmid, &dev->vmids);
	c->vmid = c->root = 0;
	return 0;
}

/* 按源 MQD 找到硬件槽，保存首次诊断供卸载后查询。 */
void gpgpu_vm_save_fault(struct gpgpu_user_queue *q)
{
	struct gpgpu_qdev *dev = q->client->dev;

	if (!dev->present || q->fault.reason)
		return;
	for (u32 slot = 2; slot < 4; slot++) {
		u64 mqd;
		u32 base = GPGPU_REG_VM_FAULT(slot), reason;

		writel(slot, dev->regs + GPGPU_REG_HQD_SELECT);
		mqd = readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_LO);
		mqd |= (u64)readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_HI) << 32;
		if (mqd != q->mqd_address)
			continue;
		reason = readl(dev->regs + base + GPGPU_VM_FAULT_REASON);
		q->fault = (struct gpgpu_vm_fault) {
			.version = GPGPU_DRM_VERSION,
			.queue_id = q->id,
			.reason = reason,
			.vmid = readl(dev->regs + base + GPGPU_VM_FAULT_VMID),
			.address = readl(dev->regs + base + GPGPU_VM_FAULT_ADDR_LO) |
				((u64)readl(dev->regs + base + GPGPU_VM_FAULT_ADDR_HI) << 32),
			.access = readl(dev->regs + base + GPGPU_VM_FAULT_ACCESS),
			.phase = readl(dev->regs + base + GPGPU_VM_FAULT_PHASE),
		};
		break;
	}
}

/* 新槽使用前旧 IH 已经排空，且只清未激活的槽，保留其他队列故障。 */
static void vm_clear_free_faults(struct gpgpu_qdev *dev)
{
	gpgpu_ih_drain_locked(dev);
	for (u32 slot = 2; slot < 4; slot++) {
		writel(slot, dev->regs + GPGPU_REG_HQD_SELECT);
		if (!readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ACTIVE) &&
		    !readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_LO) &&
		    !readl(dev->regs + GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_HI))
			writel(1, dev->regs + GPGPU_REG_VM_FAULT(slot) + GPGPU_VM_FAULT_CLEAR);
	}
}

/* DRM 已复制固定结构；沿原有队列/事件操作处理，版本和归属在此统一校验。 */
static int vm_ioctl(struct drm_device *drm, void *data, struct drm_file *file, unsigned int cmd)
{
	struct gpgpu_qclient *c = file->driver_priv;
	struct gpgpu_qdev *dev = c->dev;
	bool serial = cmd != GPGPU_DRM_EVENT_WAIT;
	int ret = 0;

	if (*(u32 *)data != GPGPU_DRM_VERSION)
		return -EPROTONOSUPPORT;
	if (task_tgid_nr(current) != c->owner)
		return -EOPNOTSUPP;
	if (serial)
		mutex_lock(&dev->vm_serial);
	mutex_lock(&dev->lock);
	if (!dev->present) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case GPGPU_DRM_INFO: {
		struct gpgpu_vm_info *r = data;

		if (r->flags) {
			ret = -EINVAL;
			break;
		}
		*r = (struct gpgpu_vm_info) {
			.version = GPGPU_DRM_VERSION,
			.address_bits = 32,
			.page_size = PAGE_SIZE,
			.user_vmids = 2,
			.queues_per_vm = 1,
			.range_count = dev->gpuvm ? 1 : 0,
			.bindings = hweight_long(dev->vmids),
			.ranges = { { dev->gpuvm ? PAGE_SIZE : 0,
				      dev->gpuvm ? GPGPU_VM_NOTIFY_VA : 0 } },
		};
		break;
	}
	case GPGPU_DRM_BO_CREATE:
		ret = bo_create(c, data);
		break;
	case GPGPU_DRM_MAP:
		ret = vm_map(c, data);
		break;
	case GPGPU_DRM_UNMAP:
		ret = vm_unmap(c, data);
		break;
	case GPGPU_DRM_COPY:
		ret = gpgpu_memory_copy(c, data);
		break;
	case GPGPU_DRM_QUEUE_CREATE:
		vm_clear_free_faults(dev);
		ret = gpgpu_queue_create(c, data);
		break;
	case GPGPU_DRM_QUEUE_QUERY:
	case GPGPU_DRM_QUEUE_DESTROY:
		ret = gpgpu_queue_operation(c, cmd, data);
		break;
	case GPGPU_DRM_FAULT: {
		struct gpgpu_vm_fault *r = data;
		struct gpgpu_user_queue *q = gpgpu_queue_find(c, r->queue_id);

		if (!q) {
			ret = -ENOENT;
			break;
		}
		gpgpu_vm_save_fault(q);
		*r = q->fault;
		r->version = GPGPU_DRM_VERSION;
		r->queue_id = q->id;
		break;
	}
	case GPGPU_DRM_EVENT_CREATE:
	case GPGPU_DRM_EVENT_QUERY:
	case GPGPU_DRM_EVENT_WAIT:
	case GPGPU_DRM_EVENT_DESTROY:
		ret = gpgpu_event_operation(c, cmd, data);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&dev->lock);
	if (serial)
		mutex_unlock(&dev->vm_serial);
	return ret;
}

#define VM_HANDLER(name, command)                                            \
	static int name(struct drm_device *d, void *arg, struct drm_file *f) \
	{                                                                    \
		return vm_ioctl(d, arg, f, command);                         \
	}
VM_HANDLER(vm_info_ioctl, GPGPU_DRM_INFO)
VM_HANDLER(bo_create_ioctl, GPGPU_DRM_BO_CREATE)
VM_HANDLER(vm_map_ioctl, GPGPU_DRM_MAP)
VM_HANDLER(vm_unmap_ioctl, GPGPU_DRM_UNMAP)
VM_HANDLER(vm_copy_ioctl, GPGPU_DRM_COPY)
VM_HANDLER(vm_queue_create_ioctl, GPGPU_DRM_QUEUE_CREATE)
VM_HANDLER(vm_queue_query_ioctl, GPGPU_DRM_QUEUE_QUERY)
VM_HANDLER(vm_queue_destroy_ioctl, GPGPU_DRM_QUEUE_DESTROY)
VM_HANDLER(vm_event_create_ioctl, GPGPU_DRM_EVENT_CREATE)
VM_HANDLER(vm_event_query_ioctl, GPGPU_DRM_EVENT_QUERY)
VM_HANDLER(vm_event_wait_ioctl, GPGPU_DRM_EVENT_WAIT)
VM_HANDLER(vm_event_destroy_ioctl, GPGPU_DRM_EVENT_DESTROY)
VM_HANDLER(vm_fault_ioctl, GPGPU_DRM_FAULT)
#define VM_IOCTL(command, handler)                                                           \
	{                                                                                    \
		.cmd = command, .func = handler, .flags = DRM_RENDER_ALLOW, .name = #command \
	}
static const struct drm_ioctl_desc vm_ioctls[] = {
	VM_IOCTL(GPGPU_DRM_INFO, vm_info_ioctl),
	VM_IOCTL(GPGPU_DRM_BO_CREATE, bo_create_ioctl),
	VM_IOCTL(GPGPU_DRM_MAP, vm_map_ioctl),
	VM_IOCTL(GPGPU_DRM_UNMAP, vm_unmap_ioctl),
	VM_IOCTL(GPGPU_DRM_COPY, vm_copy_ioctl),
	VM_IOCTL(GPGPU_DRM_QUEUE_CREATE, vm_queue_create_ioctl),
	VM_IOCTL(GPGPU_DRM_QUEUE_QUERY, vm_queue_query_ioctl),
	VM_IOCTL(GPGPU_DRM_QUEUE_DESTROY, vm_queue_destroy_ioctl),
	VM_IOCTL(GPGPU_DRM_EVENT_CREATE, vm_event_create_ioctl),
	VM_IOCTL(GPGPU_DRM_EVENT_QUERY, vm_event_query_ioctl),
	VM_IOCTL(GPGPU_DRM_EVENT_WAIT, vm_event_wait_ioctl),
	VM_IOCTL(GPGPU_DRM_EVENT_DESTROY, vm_event_destroy_ioctl),
	VM_IOCTL(GPGPU_DRM_FAULT, vm_fault_ioctl),
};

/* 使用已有会话初始化，不让 DRM file->private_data 被旧入口覆盖。 */
static int vm_open(struct drm_device *drm, struct drm_file *file)
{
	struct file temporary = {};
	struct gpgpu_qclient *c;
	int ret;

	if (!drm_is_render_client(file))
		return -EOPNOTSUPP;
	ret = gpgpu_qopen(drm->dev_private, &temporary);
	if (ret)
		return ret;
	c = temporary.private_data;
	c->drm_file = file;
	file->driver_priv = c;
	return 0;
}

/* GEM 核心已关闭句柄；存活 GPU 引用由已有 orphan 路径保留。 */
static void vm_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct file temporary = { .private_data = file->driver_priv };

	gpgpu_qrelease(NULL, &temporary);
}

/* 所有 CPU 映射均拒绝跨进程使用；Doorbell 令牌不能落入 GEM offset 区间。 */
static int gpuvm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_file *drm_file = file->private_data;
	struct gpgpu_qclient *c = drm_file->driver_priv;
	struct file temporary = { .private_data = c };

	if (task_tgid_nr(current) != c->owner)
		return -EOPNOTSUPP;
	if (!READ_ONCE(c->dev->present))
		return -ENODEV;
	if (vma->vm_pgoff == GPGPU_VM_DB_MMAP_PAGE)
		return gpgpu_qmmap(&temporary, vma);
	return drm_gem_mmap(file, vma);
}

static const struct file_operations vm_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.mmap = gpuvm_mmap,
};
static const struct drm_driver vm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.open = vm_open,
	.postclose = vm_postclose,
	.gem_prime_import = bo_import,
	.fops = &vm_fops,
	.ioctls = vm_ioctls,
	.num_ioctls = ARRAY_SIZE(vm_ioctls),
	.name = "gpgpu",
	.desc = "GPGPU 固定存储队列",
	.major = 2,
	.minor = 0,
};

/* 仅 4 KiB 客体页可匹配设备固定页表，注册后才开放用户连接。 */
int gpgpu_drm_register(struct gpgpu_qdev *dev)
{
	struct drm_device *drm;
	int ret;

	if (PAGE_SIZE != GPGPU_VM_PAGE_SIZE)
		return -EOPNOTSUPP;
	drm = drm_dev_alloc(&vm_driver, &dev->pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);
	drm->dev_private = dev;
	dev->drm = drm;
	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev->drm = NULL;
		drm_dev_put(drm);
	}
	return ret;
}

/* 先撤销用户入口；PCI remove 随后停止 CP 并回收保留对象。 */
void gpgpu_drm_unregister(struct gpgpu_qdev *dev)
{
	if (dev->drm) {
		drm_dev_unregister(dev->drm);
		drm_dev_put(dev->drm);
	}
}
#else
int gpgpu_drm_register(struct gpgpu_qdev *dev)
{
	return -EOPNOTSUPP;
}
void gpgpu_drm_unregister(struct gpgpu_qdev *dev)
{
}
void gpgpu_gem_drop(struct gpgpu_qmemory *m)
{
}
int gpgpu_vm_cleanup(struct gpgpu_qclient *c)
{
	return 0;
}
void gpgpu_vm_save_fault(struct gpgpu_user_queue *q)
{
}
#endif
