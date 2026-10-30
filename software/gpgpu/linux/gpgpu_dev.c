// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：设备生命周期
 *
 * 启动顺序（gpgpu_device_start）：
 *   1. 保持 CP 复位，把固件写入 VRAM 末尾保留区并设置复位向量；
 *   2. 使能 Outbound，使设备能经 GPU 物理地址访问 Host RAM；
 *   3. 配置并使能 IH Ring；
 *   4. 经 Host 选槽窗口写入 KIQ（槽 0）并激活；
 *   5. 使能中断，释放 CP 复位；
 *   6. 经 KIQ 映射 HIQ，确认固件已完成版本握手。
 *
 * 停止顺序（gpgpu_device_stop）：拒绝新请求并让等待者失败，保持 CP 复位，
 * 等跨进程访问结束后整体复位设备，最后回收 orphan 会话。
 */
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include "gpgpu_drv.h"

/* DMA 地址的可用范围：驱动把 DMA mask 设为 31 位。 */
#define GPGPU_DMA_LIMIT (1ULL << 31)

/* ---- DMA 缓冲区 ---- */

/*
 * gpgpu_dma_buf_alloc() - 分配一块清零的一致性 DMA 内存并换算 GPU 物理地址
 * @dev: 设备。
 * @buf: 输出。
 * @size: 字节数。
 * @align: DMA 地址的对齐要求（2 的幂）；dma_alloc_coherent 按分配阶对齐，
 *         调用者需保证 size 足以满足该对齐。
 *
 * DMA 地址必须落在 31 位范围和 Outbound 窗口内，才能换算成
 * GPU 物理地址 vram_size + IOVA。
 *
 * Return: 0，或 -ENOMEM / -ERANGE（失败时不保留任何分配）。
 */
int gpgpu_dma_buf_alloc(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf, size_t size,
			size_t align)
{
	u64 window = (1ULL << 32) - dev->vram_size;

	buf->cpu = dma_alloc_coherent(&dev->pdev->dev, size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu)
		return -ENOMEM;
	buf->size = size;
	if ((buf->dma & (align - 1)) || buf->dma >= GPGPU_DMA_LIMIT ||
	    size > GPGPU_DMA_LIMIT - buf->dma || buf->dma >= window || size > window - buf->dma) {
		gpgpu_dma_buf_free(dev, buf);
		return -ERANGE;
	}
	buf->addr = dev->vram_size + buf->dma;
	memset(buf->cpu, 0, size);
	return 0;
}

void gpgpu_dma_buf_free(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf)
{
	if (buf->cpu)
		dma_free_coherent(&dev->pdev->dev, buf->size, buf->cpu, buf->dma);
	*buf = (struct gpgpu_dma_buf) { 0 };
}

/* ---- 引用计数 ---- */

/* 最后一个引用：释放管理对象、IH、VRAM 分配器与 PCI 引用。 */
static void gpgpu_device_release(struct kref *refs)
{
	struct gpgpu_device *dev = container_of(refs, struct gpgpu_device, refs);

	gpgpu_mgmt_fini(dev);
	gpgpu_dma_buf_free(dev, &dev->ih);
	gpgpu_vram_fini(dev);
	if (dev->vram)
		pci_iounmap(dev->pdev, dev->vram);
	pci_dev_put(dev->pdev);
	kfree(dev);
}

void gpgpu_device_get(struct gpgpu_device *dev)
{
	kref_get(&dev->refs);
}

void gpgpu_device_put(struct gpgpu_device *dev)
{
	kref_put(&dev->refs, gpgpu_device_release);
}

/* ---- 创建 ---- */

/*
 * 驱动对设备的假设：DMA 一致；刚复位，CP 在位且保持复位；VRAM 恰为固件
 * 布局要求的 64 MiB（栈顶即 VRAM 末尾）；Doorbell BAR 至少 3 页。
 */
static bool gpgpu_device_supported(struct pci_dev *pdev, void __iomem *regs)
{
	return dev_is_dma_coherent(&pdev->dev) &&
		readl(regs + GPGPU_REG_CP_CTRL) == (GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD) &&
		readl(regs + GPGPU_REG_VRAM_SIZE_LO) == GPGPU_FIRMWARE_STACK_TOP &&
		GPGPU_DOORBELL_BAR_SIZE / 3 >= PAGE_SIZE;
}

/*
 * gpgpu_device_create() - 建立设备对象
 *
 * probe 在复位并使能设备后调用，只分配软件对象和内存，不启动 CP。
 *
 * 1. 检查设备满足驱动假设：一致性 DMA、CP 在位且保持复位、VRAM 为
 *    64 MiB，Doorbell BAR 至少容纳 3 页；
 * 2. 分配设备对象，初始化锁、链表与引用计数（probe 持有第一个引用）。
 *    Doorbell 第 0 页留给管理队列，管理请求序号从 1 开始；
 * 3. 建立 VRAM 分配器（预留首页），映射 BAR2 用于装载固件、清零对象
 *    和写页表；
 * 4. 分配 KIQ、HIQ 的 Ring 与 MQD，以及 IH Ring。
 *
 * 失败时放下唯一的引用，gpgpu_device_release() 只回收已分配的部分。
 */
struct gpgpu_device *gpgpu_device_create(struct pci_dev *pdev, void __iomem *regs,
					 void __iomem *doorbell, bool gpuvm)
{
	struct gpgpu_device *dev;
	int ret;

	/* 1. 设备不满足驱动假设时不建立对象。 */
	if (!gpgpu_device_supported(pdev, regs))
		return ERR_PTR(-EOPNOTSUPP);

	/* 2. 软件状态；present 置位后各接口才接受请求。 */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->pdev = pci_dev_get(pdev);
	dev->regs = regs;
	dev->doorbell = doorbell;
	dev->gpuvm = gpuvm;
	dev->vram_size = readl(regs + GPGPU_REG_VRAM_SIZE_LO);

	kref_init(&dev->refs);
	mutex_init(&dev->lock);
	mutex_init(&dev->vm_serial);
	mutex_init(&dev->management_lock);
	mutex_init(&dev->dma_lock);
	spin_lock_init(&dev->irq_lock);
	INIT_WORK(&dev->ih_work, gpgpu_ih_work);
	init_completion(&dev->dma_done);

	INIT_LIST_HEAD(&dev->fprivs);
	INIT_LIST_HEAD(&dev->requests);
	INIT_LIST_HEAD(&dev->reserved_blocks);
	dev->next_sequence = 1;
	dev->db_pages = BIT(0);
	dev->present = true;

	/* 3. VRAM 分配器与 BAR2 映射。 */
	ret = gpgpu_vram_init(dev);
	if (ret)
		goto fail;
	dev->vram = pci_iomap(pdev, 2, 0);
	if (!dev->vram) {
		ret = -ENOMEM;
		goto fail;
	}

	/* 4. 管理队列与 IH Ring 都在 Host 内存，由设备经 Outbound 访问。 */
	ret = gpgpu_mgmt_init(dev);
	if (ret)
		goto fail;
	ret = gpgpu_dma_buf_alloc(dev, &dev->ih, GPGPU_IH_BYTES, GPGPU_IH_BYTES);
	if (ret)
		goto fail;

	return dev;
fail:
	gpgpu_device_put(dev);
	return ERR_PTR(ret);
}

/* ---- 启动 ---- */

/* 第 1 步：保持 CP 复位，装载固件并设置复位向量。 */
static int gpgpu_load_firmware(struct gpgpu_device *dev)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, "gpgpu-cp.bin", &dev->pdev->dev);
	if (ret)
		return ret;
	if (!fw->size || fw->size > GPGPU_FIRMWARE_STACK_BASE - GPGPU_FIRMWARE_BASE) {
		release_firmware(fw);
		return -EINVAL;
	}

	/* 读回 CP_CTRL，确认 CP 已停在复位状态后再改写它的代码区。 */
	gpgpu_wreg(dev, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
	gpgpu_rreg(dev, GPGPU_REG_CP_CTRL);
	memcpy_toio(dev->vram + GPGPU_FIRMWARE_BASE, fw->data, fw->size);
	release_firmware(fw);
	gpgpu_wreg(dev, GPGPU_REG_CP_RESET_VECTOR, GPGPU_FIRMWARE_BASE);
	return 0;
}

/*
 * 第 2 步：Outbound 布局由设备固定为 [vram_size, 4 GiB) → IOVA 0，驱动只核对
 * 布局与 gpgpu_dma_buf_alloc() 的换算一致，然后打开窗口。
 */
static int gpgpu_enable_outbound(struct gpgpu_device *dev)
{
	if (gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_GPU_BASE) != dev->vram_size ||
	    gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_HOST_LO) || gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_HOST_HI))
		return -EIO;
	gpgpu_wreg(dev, GPGPU_REG_OUTBOUND_CONTROL, GPGPU_OUTBOUND_ENABLE);
	return gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_STATUS) == GPGPU_OUTBOUND_ACK ? 0 : -EIO;
}

/* 第 4 步：经 Host 选槽窗口写入 KIQ 并激活；激活前所有字段都已写入。 */
static int gpgpu_load_kiq(struct gpgpu_device *dev)
{
	struct gpgpu_mgmt_ring *kiq = &dev->mgmt[GPGPU_HQD_SLOT_KIQ];
	u32 win = GPGPU_REG_HQD_WINDOW;

	gpgpu_wreg(dev, GPGPU_REG_HQD_SELECT, GPGPU_HQD_SLOT_KIQ);
	if (gpgpu_rreg(dev, GPGPU_REG_HQD_SELECT) != GPGPU_HQD_SLOT_KIQ)
		return -EIO;

	gpgpu_wreg64(dev, win + GPGPU_HQD_MQD_LO, kiq->mqd.addr);
	gpgpu_wreg64(dev, win + GPGPU_HQD_RING_LO, kiq->storage.addr);
	gpgpu_wreg64(dev, win + GPGPU_HQD_RPTR_ADDR_LO, kiq->storage.addr + GPGPU_MGMT_BYTES);
	gpgpu_wreg64(dev, win + GPGPU_HQD_WPTR_ADDR_LO, kiq->storage.addr + GPGPU_MGMT_BYTES + 8);
	gpgpu_wreg(dev, win + GPGPU_HQD_CONTROL, GPGPU_MGMT_BYTES);
	gpgpu_wreg(dev, win + GPGPU_HQD_DOORBELL, GPGPU_DOORBELL_ENABLE | kiq->doorbell);
	gpgpu_wreg(dev, win + GPGPU_HQD_VMID, 0);
	gpgpu_wreg(dev, win + GPGPU_HQD_AQL, 0);
	gpgpu_wreg(dev, win + GPGPU_HQD_RPTR, 0);

	dma_wmb();
	gpgpu_wreg(dev, win + GPGPU_HQD_ACTIVE, 1);
	return gpgpu_rreg(dev, win + GPGPU_HQD_ACTIVE) == 1 ? 0 : -EIO;
}

/* 第 6 步：经 KIQ 映射 HIQ，并确认固件已写回协议版本。 */
static int gpgpu_map_hiq(struct gpgpu_device *dev)
{
	bool published;
	u32 status;
	int ret;

	mutex_lock(&dev->lock);
	ret = gpgpu_mgmt_submit(dev, GPGPU_HQD_SLOT_KIQ, GPGPU_MAP_QUEUES, GPGPU_QUEUE_HIQ,
				dev->mgmt[GPGPU_HQD_SLOT_HIQ].mqd.addr, &status, &published);
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;
	if (gpgpu_rreg(dev, GPGPU_REG_VM_FIRMWARE) != GPGPU_PROTOCOL_VERSION)
		return -EPROTONOSUPPORT;
	return status == GPGPU_MANAGEMENT_OK ? 0 : -EIO;
}

/*
 * gpgpu_device_start() - 按文件头的启动顺序让 CP 开始工作
 *
 * 第 1～5 步在 CP 复位期间完成配置，释放复位后 CP 从固件入口运行并开始
 * 轮询 KIQ；第 6 步第一次经 KIQ 发送管理命令。任一步失败直接返回，由
 * probe 调用 gpgpu_device_stop() 复位设备并回收。
 */
int gpgpu_device_start(struct gpgpu_device *dev)
{
	int ret;

	/* 1. CP 保持复位，把固件写进 VRAM 末尾保留区并设置复位向量。 */
	ret = gpgpu_load_firmware(dev);
	if (ret)
		return ret;

	/* 2. 打开 Outbound，设备才能访问 Host 内存中的 Ring、MQD 与 IH。 */
	ret = gpgpu_enable_outbound(dev);
	if (ret)
		return ret;

	/* 3. 告诉设备 IH Ring 的地址和大小，再使能。 */
	gpgpu_wreg64(dev, GPGPU_REG_IH_BASE_LO, dev->ih.addr);
	gpgpu_wreg(dev, GPGPU_REG_IH_SIZE, GPGPU_IH_BYTES);
	gpgpu_wreg(dev, GPGPU_REG_IH_CTRL, GPGPU_IH_ENABLE);

	/* 4. KIQ 由 Host 直接写入 HQD 槽 0，不经管理命令。 */
	ret = gpgpu_load_kiq(dev);
	if (ret)
		return ret;

	/* 5. 使能中断，再释放 CP 复位；以上配置须先对设备可见。 */
	gpgpu_wreg(dev, GPGPU_REG_IRQ_ENABLE, GPGPU_IRQ_ALL);
	wmb(); /* 固件、IH、KIQ 与内存中的 MQD/Ring 先于释放 CP 复位可见。 */
	gpgpu_wreg(dev, GPGPU_REG_CP_CTRL, 0);

	/* 6. 经 KIQ 映射 HIQ，同时确认固件完成了版本握手。 */
	return gpgpu_map_hiq(dev);
}

/* ---- 停止 ---- */

/*
 * gpgpu_reset_hardware() - 保持 CP 复位后整体复位设备
 *
 * 跨进程 DMA 可能仍在途，设备会拒绝复位；在超时前反复尝试。
 *
 * Return: 确认 CP 保持复位、设备与 Outbound 均已复位且无在途访问时为 true。
 */
static bool gpgpu_reset_hardware(struct gpgpu_device *dev)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(GPGPU_TIMEOUT_MS);

	gpgpu_wreg(dev, GPGPU_REG_IRQ_ENABLE, 0);
	gpgpu_wreg(dev, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
	gpgpu_rreg(dev, GPGPU_REG_CP_CTRL);
	do {
		if (!(gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_STATUS) & GPGPU_OUTBOUND_BUSY)) {
			gpgpu_wreg(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
			if ((gpgpu_rreg(dev, GPGPU_REG_CP_CTRL) & GPGPU_CP_RESET_HOLD) &&
			    !gpgpu_rreg(dev, GPGPU_REG_GLOBAL_CTRL) &&
			    !gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_CONTROL) &&
			    !(gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_STATUS) & GPGPU_OUTBOUND_BUSY))
				return true;
		}
		usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	return false;
}

/*
 * gpgpu_device_stop() - 移除或启动失败时停止设备访问
 *
 * PCI remove 或 probe 启动失败时调用。目标是确认设备不再访问任何 Host
 * 内存后再回收资源；无法确认时宁可永久保留。
 *
 * 1. 持 dev->lock 清除 present：新的 ioctl、管理请求和 COPY 都会拒绝。
 *    让全部队列与 PENDING 事件以失败结束，唤醒等待者；
 * 2. 依次取 management_lock 与 dma_lock，等正在发布的管理命令和进行中
 *    的 COPY 退出，再保持 CP 复位并整体复位设备，最后关闭总线主控；
 * 3. 等全部中断向量的处理函数返回，取消并等待 IH 工作项，此后不再有
 *    异步代码访问 BAR；
 * 4. 确认停止后回收 orphan 会话。无法确认时置 dma_uncertain，并额外
 *    取一个设备引用，让设备对象和全部存储永久保留；
 * 5. 取消 BAR2 映射：之后 PCI 区域会被释放，存活的 GEM 对象不再访问 VRAM。
 */
void gpgpu_device_stop(struct gpgpu_device *dev)
{
	struct gpgpu_fpriv *fpriv, *next;
	bool stopped;

	/* 1. 拒绝新请求，让现有等待以失败结束。 */
	mutex_lock(&dev->lock);
	WRITE_ONCE(dev->present, false);
	gpgpu_fail_queues(dev, NULL, GPGPU_ERR_FIRMWARE);
	mutex_unlock(&dev->lock);

	/* 2. 两把锁保证此刻没有管理命令正在发布、没有 COPY 在等待。 */
	mutex_lock(&dev->management_lock);
	mutex_lock(&dev->dma_lock);
	stopped = gpgpu_reset_hardware(dev);
	pci_clear_master(dev->pdev);
	mutex_unlock(&dev->dma_lock);
	mutex_unlock(&dev->management_lock);

	/* 3. 等中断上半部与 IH 工作项全部结束。 */
	for (unsigned int i = 0; i < GPGPU_MSIX_VECTORS; i++)
		synchronize_irq(pci_irq_vector(dev->pdev, i));
	cancel_work_sync(&dev->ih_work);

	/* 4. 设备确认停止才回收 orphan 会话。 */
	mutex_lock(&dev->lock);
	dev->stopped = stopped;
	if (stopped) {
		/* probe 仍持有一个引用，这里放下会话引用不会释放设备。 */
		list_for_each_entry_safe(fpriv, next, &dev->fprivs, node) {
			if (!fpriv->orphan)
				continue;
			gpgpu_fpriv_destroy(fpriv);
			gpgpu_device_put(dev);
		}
	} else {
		dev->dma_uncertain = true;
		gpgpu_device_get(dev);
		dev_err(&dev->pdev->dev, "未确认设备停止访问，保留 DMA 与队列资源\n");
	}
	mutex_unlock(&dev->lock);

	/* 5. BAR2 即将随 PCI 区域释放，之后不再经它访问 VRAM。 */
	pci_iounmap(dev->pdev, dev->vram);
	dev->vram = NULL;
}
