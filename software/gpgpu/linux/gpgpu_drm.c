// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：DRM 入口
 *
 * 每次打开 render node 创建一个会话（gpgpu_fpriv）。每个 ioctl 都经
 * gpgpu_ioctl_enter() 统一检查接口版本与调用进程，再按需串行化：
 *
 *   ioctl ─▶ 检查版本与进程 ─▶ 取 vm_serial（EVENT_WAIT 除外）与 dev->lock
 *         ─▶ 具体处理函数 ─▶ 释放锁
 *
 * 文件关闭时，若会话的队列、VM 或 DMA 仍可能被设备访问，会话保留为 orphan，
 * 等设备停止后由 gpgpu_device_stop() 回收。
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include "gpgpu_drv.h"

/* GPUVA 位数与每个 VM 的队列数，供 INFO 报告。 */
#define GPGPU_GPUVA_BITS 32
#define GPGPU_QUEUES_PER_VM 1

/* ---- 会话 ---- */

static int gpgpu_open(struct drm_device *drm, struct drm_file *file)
{
	struct gpgpu_device *dev = drm->dev_private;
	struct gpgpu_fpriv *fpriv;

	if (!drm_is_render_client(file))
		return -EOPNOTSUPP;
	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;
	fpriv->dev = dev;
	fpriv->file = file;
	fpriv->owner = task_tgid_nr(current);
	fpriv->next_queue = 1;
	fpriv->next_event = 1;
	INIT_LIST_HEAD(&fpriv->bos);
	INIT_LIST_HEAD(&fpriv->queues);
	INIT_LIST_HEAD(&fpriv->events);
	INIT_LIST_HEAD(&fpriv->vm.tables);
	atomic_set(&fpriv->db_maps, 0);

	mutex_lock(&dev->lock);
	if (!dev->present || dev->fatal) {
		mutex_unlock(&dev->lock);
		kfree(fpriv);
		return -ENODEV;
	}
	list_add_tail(&fpriv->node, &dev->fprivs);
	gpgpu_device_get(dev);
	mutex_unlock(&dev->lock);
	file->driver_priv = fpriv;
	return 0;
}

/**
 * gpgpu_fpriv_destroy() - 回收会话全部对象并放下设备引用
 * @fpriv: 会话；调用者持 dev->lock，并已确认设备不再访问其对象。
 *
 * 按事件、队列、地址空间、对象的顺序释放，最后归还 Doorbell 页。
 */
void gpgpu_fpriv_destroy(struct gpgpu_fpriv *fpriv)
{
	struct gpgpu_device *dev = fpriv->dev;
	struct gpgpu_queue *q, *next;

	gpgpu_event_release_all(fpriv);
	list_for_each_entry_safe(q, next, &fpriv->queues, node)
		gpgpu_queue_free(q);
	gpgpu_vm_cleanup(fpriv);
	gpgpu_bo_release_all(fpriv);
	if (fpriv->db_page)
		clear_bit(fpriv->db_page, &dev->db_pages);
	list_del(&fpriv->node);
	kfree(fpriv);
	gpgpu_device_put(dev);
}

/*
 * 文件关闭：仍有未卸载队列、DMA 结果未知或 VM 无法安全拆除时，设备可能
 * 仍在访问会话对象，保留为 orphan；否则立即回收。
 */
static void gpgpu_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct gpgpu_fpriv *fpriv = file->driver_priv;
	struct gpgpu_device *dev = fpriv->dev;
	bool retain;

	mutex_lock(&dev->lock);
	fpriv->file = NULL;
	retain = dev->dma_uncertain || gpgpu_queue_any_live(fpriv) || gpgpu_vm_cleanup(fpriv);
	if (retain && !dev->stopped)
		fpriv->orphan = true;
	else
		gpgpu_fpriv_destroy(fpriv);
	mutex_unlock(&dev->lock);
}

/* ---- ioctl ---- */

/*
 * gpgpu_ioctl_enter() - 每个 ioctl 的公共入口检查与加锁
 * @serial: 是否取 vm_serial；事件等待不取，避免阻塞其他线程提交。
 *
 * 每个参数结构体首字段都是 version。成功返回时已持有相应的锁。
 */
static int gpgpu_ioctl_enter(struct gpgpu_fpriv *fpriv, u32 version, bool serial)
{
	struct gpgpu_device *dev = fpriv->dev;

	if (version != GPGPU_DRM_VERSION)
		return -EPROTONOSUPPORT;
	if (task_tgid_nr(current) != fpriv->owner)
		return -EOPNOTSUPP;
	if (serial)
		mutex_lock(&dev->vm_serial);
	mutex_lock(&dev->lock);
	if (dev->present)
		return 0;
	mutex_unlock(&dev->lock);
	if (serial)
		mutex_unlock(&dev->vm_serial);
	return -ENODEV;
}

static void gpgpu_ioctl_exit(struct gpgpu_fpriv *fpriv, bool serial)
{
	mutex_unlock(&fpriv->dev->lock);
	if (serial)
		mutex_unlock(&fpriv->dev->vm_serial);
}

/*
 * GPGPU_IOCTL_HANDLER() - 生成一个带公共检查的 ioctl 处理函数
 * @name: 生成的函数名。
 * @type: 参数结构体类型。
 * @serial: 是否串行化。
 * @body: 具体处理，形如 handler(fpriv, args)。
 */
#define GPGPU_IOCTL_HANDLER(name, type, serial, body)                                     \
	static int name(struct drm_device *drm, void *data, struct drm_file *file)        \
	{                                                                                 \
		struct gpgpu_fpriv *fpriv = file->driver_priv;                            \
		type *args = data;                                                        \
		int ret = gpgpu_ioctl_enter(fpriv, args->version, serial);                \
										  \
		if (ret)                                                                  \
			return ret;                                                       \
		ret = body;                                                               \
		gpgpu_ioctl_exit(fpriv, serial);                                          \
		return ret;                                                               \
	}

/* INFO：GPUVM 开启时报告一个可分配区间 [PAGE_SIZE, 通知页)，关闭时为零个。 */
static int gpgpu_info(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_info *args)
{
	struct gpgpu_device *dev = fpriv->dev;

	if (args->flags)
		return -EINVAL;
	*args = (struct gpgpu_vm_info) {
		.version = GPGPU_DRM_VERSION,
		.address_bits = GPGPU_GPUVA_BITS,
		.page_size = PAGE_SIZE,
		.user_vmids = GPGPU_VM_USERS,
		.queues_per_vm = GPGPU_QUEUES_PER_VM,
		.range_count = dev->gpuvm ? 1 : 0,
		.bindings = hweight_long(dev->vmids),
		.ranges = { { dev->gpuvm ? PAGE_SIZE : 0, dev->gpuvm ? GPGPU_VM_NOTIFY_VA : 0 } },
	};
	return 0;
}

/* QUEUE_CREATE：先清除空闲槽上的旧故障，新队列才不会读到别人的故障。 */
static int gpgpu_queue_create_ioctl_body(struct gpgpu_fpriv *fpriv,
					 struct gpgpu_queue_create *args)
{
	gpgpu_vm_clear_free_faults(fpriv->dev);
	return gpgpu_queue_create(fpriv, args);
}

/* FAULT：返回队列首次 GPUVM 故障；队列对象被回收前都可查询。 */
static int gpgpu_fault(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_fault *args)
{
	struct gpgpu_queue *q = gpgpu_queue_lookup(fpriv, args->queue_id);

	if (!q)
		return -ENOENT;
	gpgpu_vm_save_fault(q);
	*args = q->fault;
	args->version = GPGPU_DRM_VERSION;
	args->queue_id = q->id;
	return 0;
}

GPGPU_IOCTL_HANDLER(gpgpu_info_ioctl, struct gpgpu_vm_info, true, gpgpu_info(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_bo_create_ioctl, struct gpgpu_bo_create, true,
		    gpgpu_bo_create(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_map_ioctl, struct gpgpu_vm_map, true, gpgpu_vm_map(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_unmap_ioctl, struct gpgpu_vm_map, true, gpgpu_vm_unmap(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_copy_ioctl, struct gpgpu_memory_copy, true,
		    gpgpu_bo_copy(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_queue_create_ioctl, struct gpgpu_queue_create, true,
		    gpgpu_queue_create_ioctl_body(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_queue_query_ioctl, struct gpgpu_queue_state, true,
		    gpgpu_queue_query(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_queue_destroy_ioctl, struct gpgpu_queue_state, true,
		    gpgpu_queue_destroy(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_event_create_ioctl, struct gpgpu_event_request, true,
		    gpgpu_event_create(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_event_query_ioctl, struct gpgpu_event_request, true,
		    gpgpu_event_query(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_event_wait_ioctl, struct gpgpu_event_request, false,
		    gpgpu_event_wait(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_event_destroy_ioctl, struct gpgpu_event_request, true,
		    gpgpu_event_destroy(fpriv, args))
GPGPU_IOCTL_HANDLER(gpgpu_fault_ioctl, struct gpgpu_vm_fault, true, gpgpu_fault(fpriv, args))

#define GPGPU_IOCTL(ioctl, handler) \
	{ .cmd = ioctl, .func = handler, .flags = DRM_RENDER_ALLOW, .name = #ioctl }

static const struct drm_ioctl_desc gpgpu_ioctls[] = {
	GPGPU_IOCTL(GPGPU_DRM_INFO, gpgpu_info_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_BO_CREATE, gpgpu_bo_create_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_MAP, gpgpu_map_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_UNMAP, gpgpu_unmap_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_COPY, gpgpu_copy_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_QUEUE_CREATE, gpgpu_queue_create_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_QUEUE_QUERY, gpgpu_queue_query_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_QUEUE_DESTROY, gpgpu_queue_destroy_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_EVENT_CREATE, gpgpu_event_create_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_EVENT_QUERY, gpgpu_event_query_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_EVENT_WAIT, gpgpu_event_wait_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_EVENT_DESTROY, gpgpu_event_destroy_ioctl),
	GPGPU_IOCTL(GPGPU_DRM_FAULT, gpgpu_fault_ioctl),
};

/* ---- 文件操作 ---- */

/* 外部存储不进入固定存储池，拒绝 DMA-BUF 导入。 */
static struct drm_gem_object *gpgpu_gem_import(struct drm_device *drm, struct dma_buf *buf)
{
	return ERR_PTR(-EOPNOTSUPP);
}

/* 只允许创建会话的进程映射；Doorbell 令牌单独处理，其余交给 GEM。 */
static int gpgpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct gpgpu_fpriv *fpriv = file->driver_priv;

	if (task_tgid_nr(current) != fpriv->owner)
		return -EOPNOTSUPP;
	if (!READ_ONCE(fpriv->dev->present))
		return -ENODEV;
	if (vma->vm_pgoff == GPGPU_DOORBELL_MMAP_PAGE)
		return gpgpu_doorbell_mmap(fpriv, vma);
	return drm_gem_mmap(filp, vma);
}

static const struct file_operations gpgpu_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.mmap = gpgpu_mmap,
};

static const struct drm_driver gpgpu_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.open = gpgpu_open,
	.postclose = gpgpu_postclose,
	.gem_prime_import = gpgpu_gem_import,
	.fops = &gpgpu_fops,
	.ioctls = gpgpu_ioctls,
	.num_ioctls = ARRAY_SIZE(gpgpu_ioctls),
	.name = "gpgpu",
	.desc = "GPGPU 固定存储队列",
	.major = GPGPU_DRM_VERSION,
	.minor = 0,
};

/* 设备页表固定使用 4 KiB 页，客体页大小必须一致。 */
int gpgpu_drm_register(struct gpgpu_device *dev)
{
	struct drm_device *drm;
	int ret;

	if (PAGE_SIZE != GPGPU_VM_PAGE_SIZE)
		return -EOPNOTSUPP;
	drm = drm_dev_alloc(&gpgpu_drm_driver, &dev->pdev->dev);
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
void gpgpu_drm_unregister(struct gpgpu_device *dev)
{
	if (!dev->drm)
		return;
	drm_dev_unregister(dev->drm);
	drm_dev_put(dev->drm);
}
