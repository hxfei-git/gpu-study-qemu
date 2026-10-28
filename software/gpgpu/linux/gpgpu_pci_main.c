// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include "gpgpu_queue_drv.h"

/* PCI 入口只管理 BAR 和中断；会话及存储由队列设备独立持有引用。 */
struct gpgpu_device {
	struct gpgpu_qdev *queue;
	void __iomem *regs;
	void __iomem *doorbell;
};

static irqreturn_t gpgpu_irq(int irq, void *data)
{
	struct gpgpu_device *gpu = data;
	struct gpgpu_qdev *queue = READ_ONCE(gpu->queue);

	return queue ? gpgpu_qdev_irq(queue) : IRQ_NONE;
}

/* 两种地址模式都注册 DRM，模式只从设备只读属性取得。 */
static int gpgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct gpgpu_device *gpu;
	u32 vram_size, gpuvm;
	int ret, requested = 0;

	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto free_gpu;
	ret = pci_request_regions(pdev, "gpgpu_pci");
	if (ret)
		goto disable;
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
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(31));
	if (ret)
		goto regions;
	pci_set_master(pdev);
	gpu->regs = pci_iomap(pdev, 0, 0);
	gpu->doorbell = pci_iomap(pdev, 4, 0);
	if (!gpu->regs || !gpu->doorbell) {
		ret = -ENOMEM;
		goto unmap;
	}
	gpuvm = readl(gpu->regs + GPGPU_REG_VM_MODE);
	if (gpuvm > 1 || readl(gpu->regs + GPGPU_REG_VM_VERSION) != GPGPU_VM_VERSION) {
		ret = -EPROTONOSUPPORT;
		goto unmap;
	}
	vram_size = readl(gpu->regs + GPGPU_REG_VRAM_SIZE_LO);
	if (readl(gpu->regs + GPGPU_REG_DEV_ID) != GPGPU_DEV_ID_VALUE ||
	    readl(gpu->regs + GPGPU_REG_VRAM_SIZE_HI) || vram_size <= PAGE_SIZE ||
	    vram_size >= SZ_2G || vram_size > pci_resource_len(pdev, 2)) {
		ret = -ENODEV;
		goto unmap;
	}
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	ret = pci_alloc_irq_vectors(pdev, 4, 4, PCI_IRQ_MSIX);
	if (ret < 0)
		goto unmap;
	for (; requested < 4; requested++) {
		ret = request_irq(pci_irq_vector(pdev, requested), gpgpu_irq, 0, "gpgpu_pci", gpu);
		if (ret)
			goto irqs;
	}
	pci_set_drvdata(pdev, gpu);
	writel(GPGPU_CTRL_RESET, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	readl(gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	writel(GPGPU_CTRL_ENABLE, gpu->regs + GPGPU_REG_GLOBAL_CTRL);
	gpu->queue = gpgpu_qdev_create(pdev, gpu->regs, gpu->doorbell, gpuvm);
	if (IS_ERR(gpu->queue)) {
		ret = PTR_ERR(gpu->queue);
		gpu->queue = NULL;
		goto irqs;
	}
	ret = gpgpu_qdev_start(gpu->queue);
	if (ret)
		goto irqs;
	ret = gpgpu_drm_register(gpu->queue);
	if (ret)
		goto irqs;
	dev_info(&pdev->dev, "DRM render，GPUVM %s，VRAM %u 字节\n",
		 gpuvm ? "开启" : "关闭", vram_size);
	return 0;

irqs:
	writel(0, gpu->regs + GPGPU_REG_IRQ_ENABLE);
	if (gpu->queue)
		gpgpu_qdev_stop(gpu->queue);
	while (requested)
		free_irq(pci_irq_vector(pdev, --requested), gpu);
	pci_free_irq_vectors(pdev);
	if (gpu->queue)
		gpgpu_qdev_put(gpu->queue);
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
	kfree(gpu);
	return ret;
}

/* 停止设备访问后再释放中断与 BAR；存活 GEM 对象继续持有设备引用。 */
static void gpgpu_remove(struct pci_dev *pdev)
{
	struct gpgpu_device *gpu = pci_get_drvdata(pdev);

	gpgpu_drm_unregister(gpu->queue);
	gpgpu_qdev_stop(gpu->queue);
	for (unsigned int i = 0; i < 4; i++)
		free_irq(pci_irq_vector(pdev, i), gpu);
	pci_free_irq_vectors(pdev);
	gpgpu_qdev_put(gpu->queue);
	pci_iounmap(pdev, gpu->doorbell);
	pci_iounmap(pdev, gpu->regs);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(gpu);
}

static const struct pci_device_id gpgpu_ids[] = {
	{ PCI_DEVICE(GPGPU_VENDOR_ID, GPGPU_DEVICE_ID) }, {}
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
