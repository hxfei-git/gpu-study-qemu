// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "gpgpu_regs.h"
#include "gpgpu_uapi.h"
#include "gpgpu_queue_drv.h"

static bool queue_mode;
module_param(queue_mode, bool, 0444);
MODULE_PARM_DESC(queue_mode,
		 "启用 RV64 CP 队列模式；"
		 "默认使用第一阶段同步模式");

/* gen_pool 的最小分配单元为 2^8 字节。 */
/* 同步模式 gen_pool 的最小分配粒度，以 2 的幂表示。 */
#define GPGPU_VRAM_ALLOC_ORDER 8
/* 同步模式 VRAM 分配对齐的字节数。 */
#define GPGPU_VRAM_ALLOC_SIZE (1U << GPGPU_VRAM_ALLOC_ORDER)
/* 同步模式单次计算或 DMA 请求的等待上限，单位毫秒。 */
#define GPGPU_REQUEST_TIMEOUT_MS 5000

/**
 * struct gpgpu_device - PCI 驱动共用状态及同步模式会话控制
 * @pdev: 绑定的 PCI 设备。
 * @queue: 队列模式上下文；同步模式为 NULL。
 * @vector_count: 已申请的 MSI-X 向量数。
 * @misc: 向用户暴露 /dev/gpgpu0 的字符设备。
 * @regs: BAR0：控制寄存器
 * @doorbell: BAR4 Doorbell 映射，由队列模式使用。
 * @lock: 保护会话、请求和移除过程
 * @irq_lock: 保护 IRQ 处理与请求准备
 * @kernel_done: 计算完成或错误中断唤醒的完成量。
 * @dma_done: DMA 完成或错误中断唤醒的完成量。
 * @refs: probe 和会话各持有一个引用
 * @irqs: 按向量编号累计的中断事件次数。
 * @vram_size: BAR2 的总字节数。
 * @present: 持锁检查，移除后禁用 BAR
 * @opened: 会话到 release 时才结束
 * @failed: 本次请求错误，由 IRQ 设置
 */
struct gpgpu_device {
	struct pci_dev *pdev;
	struct gpgpu_qdev *queue;
	unsigned int vector_count;
	struct miscdevice misc;
	void __iomem *regs;
	void __iomem *doorbell;
	struct mutex lock;
	spinlock_t irq_lock;
	/* IRQ 调用 complete() 唤醒等待者，包括错误中断。 */
	struct completion kernel_done;
	struct completion dma_done;
	struct kref refs;
	atomic64_t irqs[4];
	u32 vram_size;
	bool present;
	bool opened;
	bool failed;
};

/**
 * struct gpgpu_allocation - 同步会话拥有的一段 VRAM 分配记录
 * @node: 会话 allocations 链表节点。
 * @address: VRAM 字节偏移，不是 CPU 指针
 * @size: 申请长度，用于范围检查
 * @extent: 对齐长度，用于归还分配池
 */
struct gpgpu_allocation {
	struct list_head node;
	u32 address;
	u32 size;
	u32 extent;
};

/**
 * struct gpgpu_client - 同步模式的独占会话及 DMA 暂存区
 * @gpu: 所属设备，会话存活期间持有设备引用。
 * @pool: 管理 VRAM 偏移的分配记录
 * @allocations: 本会话拥有的 VRAM 分配记录。
 * @staging: 暂存缓冲区的内核虚拟地址
 * @staging_dma: 同一缓冲区的设备可用 DMA 地址
 */
struct gpgpu_client {
	struct gpgpu_device *gpu;
	struct gen_pool *pool;
	struct list_head allocations;
	void *staging;
	dma_addr_t staging_dma;
};

/*
 * gpgpu_destroy() - 释放同步设备最后一个引用
 *
 * 先放下 PCI 引用，再回收驱动私有状态。
 */
static void gpgpu_destroy(struct kref *refs)
{
	struct gpgpu_device *gpu = container_of(refs, struct gpgpu_device, refs);

	pci_dev_put(gpu->pdev);
	kfree(gpu);
}

/*
 * gpgpu_irq() - 处理 PCI 中断并选择当前模式的路径
 *
 * 队列模式转交独立处理器；同步模式在中断锁下累计事件并唤醒请求。
 */
static irqreturn_t gpgpu_irq(int irq, void *data)
{
	struct gpgpu_device *gpu = data;
	unsigned long flags;
	u32 pending;

	if (gpu->queue)
		return gpgpu_qdev_irq(gpu->queue);
	/* 多个 MSI-X 向量可同时到达；每位只处理一次。 */
	spin_lock_irqsave(&gpu->irq_lock, flags);
	pending = readl(gpu->regs + GPGPU_REG_IRQ_STATUS) & GPGPU_IRQ_ALL;
	if (!pending) {
		spin_unlock_irqrestore(&gpu->irq_lock, flags);
		return IRQ_NONE;
	}
	writel(pending, gpu->regs + GPGPU_REG_IRQ_ACK);
	for (unsigned int i = 0; i < GPGPU_IRQ_VECTOR_COUNT; i++)
		if (pending & BIT(i))
			atomic64_inc(&gpu->irqs[i]);
	if (pending & GPGPU_IRQ_ERROR) {
		/* 让当前等待者及时返回错误，不必等到超时。 */
		WRITE_ONCE(gpu->failed, true);
		complete(&gpu->kernel_done);
		complete(&gpu->dma_done);
	}
	if (pending & GPGPU_IRQ_KERNEL)
		complete(&gpu->kernel_done);
	if (pending & GPGPU_IRQ_DMA)
		complete(&gpu->dma_done);
	spin_unlock_irqrestore(&gpu->irq_lock, flags);
	return IRQ_HANDLED;
}

/*
 * gpgpu_reset() - 复位同步模式的设备寄存器
 *
 * 先关闭中断，提交全局复位后重新使能设备和同步中断。
 */
static void gpgpu_reset(struct gpgpu_device *gpu)
{
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	writel(GPGPU_CTRL_RESET, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	writel(GPGPU_CTRL_ENABLE, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	writel(GPGPU_IRQ_ALL, gpu->regs + GPGPU_REG_IRQ_ENABLE);
}

/*
 * gpgpu_prepare_request() - 准备一次同步请求的完成状态
 *
 * 调用方持设备锁；在中断锁下清旧事件与错误，重置 completion
 * 后才能提交。
 */
static void gpgpu_prepare_request(struct gpgpu_device *gpu, struct completion *done)
{
	unsigned long flags;

	spin_lock_irqsave(&gpu->irq_lock, flags);
	writel(GPGPU_IRQ_ALL, gpu->regs + GPGPU_REG_IRQ_ACK);
	writel(~0U, gpu->regs + GPGPU_REG_ERROR_STATUS);
	WRITE_ONCE(gpu->failed, false);
	reinit_completion(done);
	spin_unlock_irqrestore(&gpu->irq_lock, flags);
}

/*
 * gpgpu_wait() - 等待同步请求结束并检查错误
 *
 * 完成中断也可能代表失败；超时按同步模式规则复位并返回 -ETIMEDOUT。
 */
static int gpgpu_wait(struct gpgpu_device *gpu, struct completion *done)
{
	unsigned long timeout = msecs_to_jiffies(GPGPU_REQUEST_TIMEOUT_MS);

	if (!wait_for_completion_timeout(done, timeout)) {
		gpgpu_reset(gpu);
		return -ETIMEDOUT;
	}
	/* 错误也会唤醒等待者，需检查请求是否成功。 */
	if (READ_ONCE(gpu->failed) || readl(gpu->regs + GPGPU_REG_ERROR_STATUS))
		return -EIO;
	return 0;
}

/*
 * gpgpu_owns_range() - 检查地址范围是否属于当前会话的一次分配
 *
 * 只接受申请长度内的非空范围，不将对齐产生的尾部空间视为用户内存。
 */
static bool gpgpu_owns_range(struct gpgpu_client *client, u32 address, u32 size)
{
	struct gpgpu_allocation *allocation;

	if (!size)
		return false;
	list_for_each_entry(allocation, &client->allocations, node) {
		/* 用差值比较，避免 address + size 溢出。 */
		if (address >= allocation->address && size <= allocation->size &&
		    address - allocation->address <= allocation->size - size) {
			return true;
		}
	}
	return false;
}

/*
 * gpgpu_open() - 建立同步会话或转交队列打开入口
 *
 * 同步模式仅允许一个会话，为其创建 VRAM 分配池和 DMA 暂存区。
 */
static int gpgpu_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct gpgpu_device *gpu = container_of(misc, struct gpgpu_device, misc);
	struct gpgpu_client *client;
	int ret;

	if (queue_mode)
		return gpgpu_qopen(gpu->queue, file);
	mutex_lock(&gpu->lock);
	if (!gpu->present || gpu->opened) {
		ret = gpu->present ? -EBUSY : -ENODEV;
		goto unlock;
	}
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client) {
		ret = -ENOMEM;
		goto unlock;
	}
	client->pool = gen_pool_create(GPGPU_VRAM_ALLOC_ORDER, -1);
	if (!client->pool) {
		ret = -ENOMEM;
		goto free_client;
	}
	/* 保留首个页，使地址 0 可以表示分配失败。 */
	ret = gen_pool_add(client->pool, PAGE_SIZE, gpu->vram_size - PAGE_SIZE, -1);
	if (ret)
		goto free_pool;
	client->staging = dma_alloc_coherent(&gpu->pdev->dev, GPGPU_STAGING_SIZE,
					     &client->staging_dma, GFP_KERNEL);
	if (!client->staging) {
		ret = -ENOMEM;
		goto free_pool;
	}
	client->gpu = gpu;
	memset(client->staging, 0, GPGPU_STAGING_SIZE);
	INIT_LIST_HEAD(&client->allocations);
	kref_get(&gpu->refs);
	gpu->opened = true;
	gpgpu_reset(gpu);
	file->private_data = client;
	mutex_unlock(&gpu->lock);
	return nonseekable_open(inode, file);

free_pool:
	gen_pool_destroy(client->pool);
free_client:
	kfree(client);
unlock:
	mutex_unlock(&gpu->lock);
	return ret;
}

/*
 * gpgpu_release() - 释放文件对应的会话资源
 *
 * 按模式分流；同步路径回收分配和暂存区，VMA
 * 持有的文件引用会延迟调用。
 */
static int gpgpu_release(struct inode *inode, struct file *file)
{
	if (queue_mode)
		return gpgpu_qrelease(inode, file);
	struct gpgpu_client *client = file->private_data;
	struct gpgpu_device *gpu = client->gpu;
	struct gpgpu_allocation *allocation, *next;

	mutex_lock(&gpu->lock);
	if (gpu->present)
		gpgpu_reset(gpu);
	list_for_each_entry_safe(allocation, next, &client->allocations, node) {
		gen_pool_free(client->pool, allocation->address, allocation->extent);
		list_del(&allocation->node);
		kfree(allocation);
	}
	gen_pool_destroy(client->pool);
	dma_free_coherent(&gpu->pdev->dev, GPGPU_STAGING_SIZE, client->staging,
			  client->staging_dma);
	gpu->opened = false;
	mutex_unlock(&gpu->lock);
	kfree(client);
	kref_put(&gpu->refs, gpgpu_destroy);
	return 0;
}

/*
 * gpgpu_mmap() - 映射当前会话允许的共享存储
 *
 * 队列模式按令牌分流；同步模式只允许映射完整暂存区且不可执行。
 */
static int gpgpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (queue_mode)
		return gpgpu_qmmap(file, vma);
	struct gpgpu_client *client = file->private_data;
	struct gpgpu_device *gpu = client->gpu;
	int ret;

	/* 只映射整个 DMA 暂存缓冲区，要求共享且不可执行。 */
	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != GPGPU_STAGING_SIZE ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC)) {
		return -EINVAL;
	}
	mutex_lock(&gpu->lock);
	if (!gpu->present) {
		ret = -ENODEV;
	} else {
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		vm_flags_clear(vma, VM_MAYEXEC);
		ret = dma_mmap_coherent(&gpu->pdev->dev, vma, client->staging, client->staging_dma,
					GPGPU_STAGING_SIZE);
	}
	/* VMA 持有 file 引用：munmap 前 release 不会释放 DMA 内存。 */
	mutex_unlock(&gpu->lock);
	return ret;
}

/*
 * gpgpu_allocate() - 为同步会话申请 VRAM
 *
 * 按 gen_pool 粒度分配并保存原始长度，回报用户失败时回滚分配。
 */
static int gpgpu_allocate(struct gpgpu_client *client, void __user *arg)
{
	struct gpgpu_alloc request;
	struct gpgpu_allocation *allocation;

	if (copy_from_user(&request, arg, sizeof(request)))
		return -EFAULT;
	if (!request.size || request.size > client->gpu->vram_size - PAGE_SIZE)
		return -EINVAL;
	allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
	if (!allocation)
		return -ENOMEM;
	allocation->size = request.size;
	allocation->extent = ALIGN(request.size, GPGPU_VRAM_ALLOC_SIZE);
	allocation->address = gen_pool_alloc(client->pool, allocation->extent);
	if (!allocation->address) {
		kfree(allocation);
		return -ENOMEM;
	}
	request.address = allocation->address;
	if (copy_to_user(arg, &request, sizeof(request))) {
		gen_pool_free(client->pool, allocation->address, allocation->extent);
		kfree(allocation);
		return -EFAULT;
	}
	list_add(&allocation->node, &client->allocations);
	return 0;
}

/*
 * gpgpu_free() - 按起始偏移释放同步 VRAM 分配
 *
 * 只在当前会话记录中匹配，内部偏移或未知地址返回错误。
 */
static int gpgpu_free(struct gpgpu_client *client, void __user *arg)
{
	struct gpgpu_allocation *allocation;
	u32 address;

	if (copy_from_user(&address, arg, sizeof(address)))
		return -EFAULT;
	list_for_each_entry(allocation, &client->allocations, node) {
		if (allocation->address == address) {
			gen_pool_free(client->pool, address, allocation->extent);
			list_del(&allocation->node);
			kfree(allocation);
			return 0;
		}
	}
	return -EINVAL;
}

/*
 * gpgpu_write_address() - 写入低、高两个 32 位地址寄存器
 *
 * reg 指向低位配置字段，调用者在启动命令前完成整对写入。
 */
static void gpgpu_write_address(struct gpgpu_device *gpu, u32 reg, u64 value)
{
	/* reg 指向低 32 位，相邻寄存器保存高 32 位。 */
	writel(lower_32_bits(value), gpu->regs + reg);
	writel(upper_32_bits(value), gpu->regs + reg + 4);
}

/*
 * gpgpu_copy() - 执行同步暂存区与 VRAM 之间的 DMA
 *
 * 校验分配范围，准备 completion 后写描述符，等待结束并同步设备写回。
 */
static int gpgpu_copy(struct gpgpu_client *client, void __user *arg)
{
	struct gpgpu_device *gpu = client->gpu;
	struct gpgpu_copy descriptor;
	bool d2h;
	int ret;

	if (copy_from_user(&descriptor, arg, sizeof(descriptor)))
		return -EFAULT;
	if (descriptor.reserved || descriptor.direction > GPGPU_COPY_D2H ||
	    descriptor.size > GPGPU_STAGING_SIZE ||
	    !gpgpu_owns_range(client, descriptor.address, descriptor.size)) {
		return -EINVAL;
	}
	d2h = descriptor.direction == GPGPU_COPY_D2H;
	gpgpu_prepare_request(gpu, &gpu->dma_done);
	/* H2D：先让暂存区写入有序，再通知设备读取。 */
	dma_wmb();
	/* H2D 为暂存区 DMA 地址 → VRAM 偏移，D2H 交换两端。 */
	gpgpu_write_address(gpu, GPGPU_REG_DMA_SRC_LO,
			    d2h ? descriptor.address : client->staging_dma);
	gpgpu_write_address(gpu, GPGPU_REG_DMA_DST_LO,
			    d2h ? client->staging_dma : descriptor.address);
	writel(descriptor.size, gpu->regs + GPGPU_REG_DMA_SIZE);
	writel(GPGPU_DMA_START | GPGPU_DMA_IRQ | (d2h ? GPGPU_DMA_D2H : 0),
	       gpu->regs + GPGPU_REG_DMA_CTRL);
	ret = gpgpu_wait(gpu, &gpu->dma_done);
	/* D2H：完成后再读取设备写回的暂存区。 */
	dma_rmb();
	return ret;
}

/*
 * gpgpu_validate_launch() - 校验同步计算请求
 *
 * 检查代码及参数归属、对齐与非零维度，并在乘法前限制派发规模。
 */
static int gpgpu_validate_launch(struct gpgpu_client *client, const struct gpgpu_launch *request)
{
	u32 blocks = 1;
	u32 threads_per_block = 1;

	if (request->shared_size)
		return -EOPNOTSUPP;
	/* 代码和参数须按 4 字节对齐，且属于当前会话。 */
	if (request->reserved || (request->code & 3) || (request->args & 3) ||
	    (request->code_size & 3) || (request->args_size & 3) ||
	    !gpgpu_owns_range(client, request->code, request->code_size) ||
	    !gpgpu_owns_range(client, request->args, request->args_size)) {
		return -EINVAL;
	}
	for (unsigned int i = 0; i < 3; i++) {
		if (!request->grid[i] || !request->block[i])
			return -EINVAL;
		/* 在相乘前检查上限，同时避免维度乘积溢出。 */
		if (request->grid[i] > GPGPU_MAX_BLOCKS / blocks ||
		    request->block[i] > GPGPU_MAX_BLOCK_THREADS / threads_per_block) {
			return -E2BIG;
		}
		blocks *= request->grid[i];
		threads_per_block *= request->block[i];
	}
	if (blocks > GPGPU_MAX_THREADS / threads_per_block)
		return -E2BIG;
	return 0;
}

/*
 * gpgpu_launch() - 提交同步 Kernel 并等待全部线程完成
 *
 * 先校验请求与准备中断状态，再填地址、维度，最后写 DISPATCH。
 */
static int gpgpu_launch(struct gpgpu_client *client, void __user *arg)
{
	struct gpgpu_device *gpu = client->gpu;
	struct gpgpu_launch request;
	int ret;

	if (copy_from_user(&request, arg, sizeof(request)))
		return -EFAULT;
	ret = gpgpu_validate_launch(client, &request);
	if (ret)
		return ret;

	gpgpu_prepare_request(gpu, &gpu->kernel_done);
	/* 代码和参数已由 COPY 上传，此处提交地址和维度。 */
	gpgpu_write_address(gpu, GPGPU_REG_KERNEL_ADDR_LO, request.code);
	gpgpu_write_address(gpu, GPGPU_REG_KERNEL_ARGS_LO, request.args);
	for (unsigned int i = 0; i < 3; i++) {
		writel(request.grid[i], gpu->regs + GPGPU_REG_GRID_DIM_X + 4 * i);
		writel(request.block[i], gpu->regs + GPGPU_REG_BLOCK_DIM_X + 4 * i);
	}
	writel(0, gpu->regs + GPGPU_REG_SHARED_MEM_SIZE);
	/* 写 DISPATCH 启动计算，等待所有线程完成。 */
	writel(1, gpu->regs + GPGPU_REG_DISPATCH);
	return gpgpu_wait(gpu, &gpu->kernel_done);
}

/*
 * gpgpu_ioctl() - 分发同步或队列接口请求
 *
 * 队列模式交给队列入口；同步模式持设备锁检查在场状态后执行命令。
 */
static long gpgpu_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	if (queue_mode)
		return gpgpu_qioctl(file, command, argument);
	struct gpgpu_client *client = file->private_data;
	struct gpgpu_device *gpu = client->gpu;
	void __user *arg = (void __user *)argument;
	struct gpgpu_info info;
	struct gpgpu_stats stats;
	long ret = 0;

	mutex_lock(&gpu->lock);
	if (!gpu->present) {
		ret = -ENODEV;
		goto unlock;
	}
	switch (command) {
	case GPGPU_IOC_INFO:
		info.abi_version = GPGPU_ABI_VERSION;
		info.vram_size = gpu->vram_size;
		info.staging_size = GPGPU_STAGING_SIZE;
		info.caps = readl(gpu->regs + GPGPU_REG_DEV_CAPS);
		ret = copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
		break;
	case GPGPU_IOC_ALLOC:
		ret = gpgpu_allocate(client, arg);
		break;
	case GPGPU_IOC_FREE:
		ret = gpgpu_free(client, arg);
		break;
	case GPGPU_IOC_COPY:
		ret = gpgpu_copy(client, arg);
		break;
	case GPGPU_IOC_LAUNCH:
		ret = gpgpu_launch(client, arg);
		break;
	case GPGPU_IOC_RESET:
		gpgpu_reset(gpu);
		break;
	case GPGPU_IOC_STATS:
		stats.kernel_irqs = atomic64_read(&gpu->irqs[GPGPU_VECTOR_KERNEL]);
		stats.dma_irqs = atomic64_read(&gpu->irqs[GPGPU_VECTOR_DMA]);
		stats.error_irqs = atomic64_read(&gpu->irqs[GPGPU_VECTOR_ERROR]);
		ret = copy_to_user(arg, &stats, sizeof(stats)) ? -EFAULT : 0;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
unlock:
	mutex_unlock(&gpu->lock);
	return ret;
}

static const struct file_operations gpgpu_fops = {
	.owner = THIS_MODULE,
	.open = gpgpu_open,
	.release = gpgpu_release,
	.unlocked_ioctl = gpgpu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.mmap = gpgpu_mmap,
};

/*
 * gpgpu_probe() - 绑定 PCI 设备并注册用户入口
 *
 * 申请 BAR 和 DMA 能力，按模式初始化中断及上下文，失败按逆序回收。
 */
static int gpgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct gpgpu_device *gpu;
	int ret;
	int requested = 0;

	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	gpu->pdev = pci_dev_get(pdev);
	kref_init(&gpu->refs);
	mutex_init(&gpu->lock);
	spin_lock_init(&gpu->irq_lock);
	init_completion(&gpu->kernel_done);
	init_completion(&gpu->dma_done);
	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto free_gpu;
	ret = pci_request_regions(pdev, "gpgpu_pci");
	if (ret)
		goto disable;
	/* 64 位 BAR 各占两槽：BAR0 控制、BAR2 显存、BAR4 门铃。 */
	for (unsigned int bar = 0; bar <= 4; bar += 2) {
		if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM) ||
		    !pci_resource_len(pdev, bar)) {
			ret = -ENODEV;
			goto regions;
		}
	}
	if (pci_resource_len(pdev, 0) < SZ_1M) {
		ret = -ENODEV;
		goto regions;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto regions;
	pci_set_master(pdev);
	gpu->regs = pci_iomap(pdev, 0, 0);
	gpu->doorbell = pci_iomap(pdev, 4, 0);
	if (!gpu->regs || !gpu->doorbell) {
		ret = -ENOMEM;
		goto unmap;
	}
	gpu->vram_size = readl(gpu->regs + GPGPU_REG_VRAM_SIZE_LO);
	if (readl(gpu->regs + GPGPU_REG_DEV_ID) != GPGPU_DEV_ID_VALUE ||
	    readl(gpu->regs + GPGPU_REG_VRAM_SIZE_HI) || gpu->vram_size <= PAGE_SIZE ||
	    gpu->vram_size >= 0x80000000U || gpu->vram_size > pci_resource_len(pdev, 2)) {
		ret = -ENODEV;
		goto unmap;
	}
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	gpu->vector_count = queue_mode ? 4 : GPGPU_IRQ_VECTOR_COUNT;
	ret = pci_alloc_irq_vectors(pdev, gpu->vector_count, gpu->vector_count, PCI_IRQ_MSIX);
	if (ret < 0)
		goto unmap;
	for (; requested < gpu->vector_count; requested++) {
		ret = request_irq(pci_irq_vector(pdev, requested), gpgpu_irq, 0, "gpgpu_pci", gpu);
		if (ret)
			goto irqs;
	}
	gpu->misc.minor = MISC_DYNAMIC_MINOR;
	gpu->misc.name = "gpgpu0";
	gpu->misc.fops = &gpgpu_fops;
	gpu->misc.parent = &pdev->dev;
	gpu->misc.mode = 0600;
	gpu->present = true;
	pci_set_drvdata(pdev, gpu);
	gpgpu_reset(gpu);
	if (queue_mode) {
		gpu->queue = gpgpu_qdev_create(pdev, gpu->regs, gpu->doorbell);
		if (IS_ERR(gpu->queue)) {
			ret = PTR_ERR(gpu->queue);
			gpu->queue = NULL;
			goto irqs;
		}
		ret = gpgpu_qdev_start(gpu->queue);
		if (ret)
			goto irqs;
	}
	ret = misc_register(&gpu->misc);
	if (ret) {
		writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
		goto irqs;
	}
	dev_info(&pdev->dev, "gpgpu_pci: /dev/gpgpu0, VRAM %u, MSI-X %u\n", gpu->vram_size,
		 gpu->vector_count);
	return 0;

irqs:
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	if (gpu->queue)
		gpgpu_qdev_stop(gpu->queue);
	while (requested)
		free_irq(pci_irq_vector(pdev, --requested), gpu);
	pci_free_irq_vectors(pdev);
	if (gpu->queue) {
		gpgpu_qdev_put(gpu->queue);
		gpu->queue = NULL;
	}
unmap:
	if (gpu->doorbell)
		pci_iounmap(pdev, gpu->doorbell);
	if (gpu->regs)
		pci_iounmap(pdev, gpu->regs);
regions:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
disable:
	pci_disable_device(pdev);
free_gpu:
	kref_put(&gpu->refs, gpgpu_destroy);
	return ret;
}

/*
 * gpgpu_remove() - 停止用户入口并拆除 PCI 资源
 *
 * 按模式先结束设备访问和中断，再解除 BAR 映射，
 * 存活会话继续持有状态引用。
 */
static void gpgpu_remove(struct pci_dev *pdev)
{
	struct gpgpu_device *gpu = pci_get_drvdata(pdev);

	misc_deregister(&gpu->misc);
	mutex_lock(&gpu->lock);
	gpu->present = false;
	if (gpu->queue) {
		/*
		 * 先禁止新的队列操作，串行结束管理/DMA，
		 * 再停止设备访问。
		 */
		gpgpu_qdev_stop(gpu->queue);
	} else {
		writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
		writel(GPGPU_CTRL_RESET, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	}
	for (unsigned int i = 0; i < gpu->vector_count; i++)
		free_irq(pci_irq_vector(pdev, i), gpu);
	pci_free_irq_vectors(pdev);
	if (gpu->queue) {
		gpgpu_qdev_put(gpu->queue);
		gpu->queue = NULL;
	}
	pci_iounmap(pdev, gpu->doorbell);
	pci_iounmap(pdev, gpu->regs);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	mutex_unlock(&gpu->lock);
	kref_put(&gpu->refs, gpgpu_destroy);
}

static const struct pci_device_id gpgpu_ids[] = { { PCI_DEVICE(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID) },
						  {} };
MODULE_DEVICE_TABLE(pci, gpgpu_ids);

static struct pci_driver gpgpu_driver = {
	.name = "gpgpu_pci",
	.id_table = gpgpu_ids,
	.probe = gpgpu_probe,
	.remove = gpgpu_remove,
};

/*
 * gpgpu_init() - 注册 Linux PCI 驱动
 *
 * 由模块加载入口安装匹配表与 probe/remove 回调。
 */
static int __init gpgpu_init(void)
{
	int ret = pci_register_driver(&gpgpu_driver);

	if (!ret)
		pr_info("gpgpu_pci: module loaded\n");
	return ret;
}

/*
 * gpgpu_exit() - 注销 Linux PCI 驱动
 *
 * 模块卸载时由 PCI 核心依次调用设备移除路径。
 */
static void __exit gpgpu_exit(void)
{
	pci_unregister_driver(&gpgpu_driver);
	pr_info("gpgpu_pci: module unloaded\n");
}

module_init(gpgpu_init);
module_exit(gpgpu_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("hxfei");
MODULE_DESCRIPTION("QEMU GPGPU PCI DMA and synchronous kernel driver");
