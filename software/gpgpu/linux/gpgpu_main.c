// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU 驱动：PCI 入口
 *
 * probe 顺序：
 *   1. 使能 PCI 设备，申请并检查 BAR0（寄存器）、BAR2（VRAM）、BAR4（Doorbell）；
 *   2. 设置 31 位 DMA mask，映射 BAR0 与 BAR4；
 *   3. 核对设备签名、协议版本与 VRAM 大小，读取 GPUVM 模式；
 *   4. 申请 MSI-X 中断；
 *   5. 复位并使能设备，创建设备对象，启动 CP，注册 DRM。
 *
 * remove 顺序相反：先撤销 DRM 入口，再停止设备访问，最后释放中断与 BAR。
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include "gpgpu_drv.h"

/**
 * struct gpgpu_pci - PCI 层资源
 * @regs: BAR0 映射。
 * @doorbell: BAR4 映射。
 * @dev: 设备对象；中断处理在其创建前收到中断时返回 IRQ_NONE。
 * @irqs: 已申请的中断向量数。
 */
struct gpgpu_pci {
	void __iomem *regs;
	void __iomem *doorbell;
	struct gpgpu_device *dev;
	unsigned int irqs;
};

static irqreturn_t gpgpu_pci_irq(int irq, void *data)
{
	struct gpgpu_pci *gpu = data;
	struct gpgpu_device *dev = READ_ONCE(gpu->dev);

	return dev ? gpgpu_irq_handler(irq, dev) : IRQ_NONE;
}

/* 第 1 步：BAR0 至少 1 MiB；BAR0、BAR2、BAR4 都是内存 BAR。 */
static int gpgpu_check_bars(struct pci_dev *pdev)
{
	for (unsigned int bar = 0; bar <= 4; bar += 2)
		if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM) ||
		    !pci_resource_len(pdev, bar))
			return -ENODEV;
	return pci_resource_len(pdev, 0) < GPGPU_CTRL_BAR_SIZE ? -ENODEV : 0;
}

/* 第 3 步：签名、协议版本与 VRAM 大小；输出 GPUVM 模式。 */
static int gpgpu_check_device(struct pci_dev *pdev, void __iomem *regs, bool *gpuvm)
{
	u32 mode = readl(regs + GPGPU_REG_VM_MODE);
	u32 vram_size = readl(regs + GPGPU_REG_VRAM_SIZE_LO);

	if (mode > 1 || readl(regs + GPGPU_REG_VM_VERSION) != GPGPU_PROTOCOL_VERSION)
		return -EPROTONOSUPPORT;
	if (readl(regs + GPGPU_REG_DEV_ID) != GPGPU_DEV_ID_VALUE ||
	    readl(regs + GPGPU_REG_VRAM_SIZE_HI) || vram_size <= PAGE_SIZE ||
	    vram_size >= SZ_2G || vram_size > pci_resource_len(pdev, 2))
		return -ENODEV;
	*gpuvm = mode;
	return 0;
}

static void gpgpu_free_irqs(struct pci_dev *pdev, struct gpgpu_pci *gpu)
{
	while (gpu->irqs)
		free_irq(pci_irq_vector(pdev, --gpu->irqs), gpu);
	pci_free_irq_vectors(pdev);
}

/*
 * 第 4 步：申请全部 MSI-X 向量，它们共用 gpgpu_pci_irq()。申请前先关闭
 * 设备中断使能，设备对象建立之前不产生中断。
 */
static int gpgpu_request_irqs(struct pci_dev *pdev, struct gpgpu_pci *gpu)
{
	int ret;

	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	ret = pci_alloc_irq_vectors(pdev, GPGPU_MSIX_VECTORS, GPGPU_MSIX_VECTORS, PCI_IRQ_MSIX);
	if (ret < 0)
		return ret;
	for (; gpu->irqs < GPGPU_MSIX_VECTORS; gpu->irqs++) {
		ret = request_irq(pci_irq_vector(pdev, gpu->irqs), gpgpu_pci_irq, 0, "gpgpu_pci",
				  gpu);
		if (ret) {
			gpgpu_free_irqs(pdev, gpu);
			return ret;
		}
	}
	return 0;
}

/* 第 5 步：复位使能设备，创建设备对象，启动 CP，注册 DRM。 */
static int gpgpu_bring_up(struct pci_dev *pdev, struct gpgpu_pci *gpu, bool gpuvm)
{
	struct gpgpu_device *dev;
	int ret;

	writel(GPGPU_CTRL_RESET, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	readl(gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	writel(GPGPU_CTRL_ENABLE, gpu->regs + GPGPU_REG_GLOBAL_CTRL);

	dev = gpgpu_device_create(pdev, gpu->regs, gpu->doorbell, gpuvm);
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	WRITE_ONCE(gpu->dev, dev);

	ret = gpgpu_device_start(dev);
	if (!ret)
		ret = gpgpu_drm_register(dev);
	if (!ret)
		return 0;

	/* 设备可能已开始运行，先停止访问；设备引用由 probe 放下。 */
	gpgpu_device_stop(dev);
	return ret;
}

/*
 * gpgpu_probe() - 按文件头的 probe 顺序接管设备
 *
 * 失败时按 irqs → unmap → release → disable 的标签逆序撤销已完成的步骤。
 * gpgpu_bring_up() 失败时已经停止设备，这里放下 probe 持有的设备引用。
 */
static int gpgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct gpgpu_pci *gpu;
	bool gpuvm;
	int ret;

	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	/* 1. 使能内存空间，申请全部 BAR 并检查三个 BAR 的类型与大小。 */
	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto free_gpu;
	ret = pci_request_regions(pdev, "gpgpu_pci");
	if (ret)
		goto disable;
	ret = gpgpu_check_bars(pdev);
	if (ret)
		goto release;

	/*
	 * 2. DMA 地址限制在 Outbound 窗口覆盖的 2 GiB IOVA 内（31 位），换算成
	 *    GPU 物理地址 GPGPU_OUTBOUND_BASE + IOVA；
	 *    映射 BAR0 寄存器与 BAR4 Doorbell。
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, GPGPU_OUTBOUND_SIZE - 1);
	if (ret)
		goto release;
	pci_set_master(pdev);
	gpu->regs = pci_iomap(pdev, 0, 0);
	gpu->doorbell = pci_iomap(pdev, 4, 0);
	if (!gpu->regs || !gpu->doorbell) {
		ret = -ENOMEM;
		goto unmap;
	}

	/* 3. 读寄存器确认设备身份与协议版本，并取得 GPUVM 模式。 */
	ret = gpgpu_check_device(pdev, gpu->regs, &gpuvm);
	if (ret)
		goto unmap;

	/*
	 * 4. 申请 MSI-X 向量。gpu->dev 此时还是 NULL，提前到达的中断返回
	 *    IRQ_NONE。
	 */
	ret = gpgpu_request_irqs(pdev, gpu);
	if (ret)
		goto unmap;
	pci_set_drvdata(pdev, gpu);

	/* 5. 复位设备、创建设备对象、启动 CP 并注册 DRM。 */
	ret = gpgpu_bring_up(pdev, gpu, gpuvm);
	if (ret)
		goto irqs;
	dev_info(&pdev->dev, "DRM render，GPUVM %s，VRAM %u 字节\n", gpuvm ? "开启" : "关闭",
		 gpu->dev->vram_size);
	return 0;

irqs:
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	gpgpu_free_irqs(pdev, gpu);
	if (gpu->dev)
		gpgpu_device_put(gpu->dev);
unmap:
	if (gpu->doorbell)
		pci_iounmap(pdev, gpu->doorbell);
	if (gpu->regs)
		pci_iounmap(pdev, gpu->regs);
release:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
disable:
	pci_disable_device(pdev);
free_gpu:
	kfree(gpu);
	return ret;
}

/*
 * 先撤销 DRM 入口，不再有新的会话；再停止设备访问并回收 orphan 会话；
 * 中断处理已结束后释放中断向量，最后放下 probe 的设备引用并释放 BAR。
 * 仍有会话或 GEM 对象时，设备对象由它们的引用继续保活。
 */
static void gpgpu_remove(struct pci_dev *pdev)
{
	struct gpgpu_pci *gpu = pci_get_drvdata(pdev);

	gpgpu_drm_unregister(gpu->dev);
	gpgpu_device_stop(gpu->dev);
	gpgpu_free_irqs(pdev, gpu);
	gpgpu_device_put(gpu->dev);
	pci_iounmap(pdev, gpu->doorbell);
	pci_iounmap(pdev, gpu->regs);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(gpu);
}

static const struct pci_device_id gpgpu_ids[] = {
	{ PCI_DEVICE(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, gpgpu_ids);

static struct pci_driver gpgpu_driver = {
	.name = "gpgpu_pci",
	.id_table = gpgpu_ids,
	.probe = gpgpu_probe,
	.remove = gpgpu_remove,
};
module_pci_driver(gpgpu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hxfei");
MODULE_DESCRIPTION("QEMU GPGPU DRM/GEM 队列驱动");
