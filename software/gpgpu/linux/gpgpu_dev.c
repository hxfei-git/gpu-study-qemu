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
 * 等跨进程访问结束后整体复位设备，最后回收 orphan 会话。整体复位会先
 * 回写并清空 dcache，确认停止后释放存储不再需要维护。
 *
 * 运行中恢复（gpgpu_device_recover）：让旧会话失效、旧等待失败，等
 * ioctl 退出后整体复位设备；确认停止才回收旧任务保留的资源，再按启动
 * 顺序重新建立 KIQ、HIQ。旧任务不保存现场，也不续跑。
 *
 * 存储归还：Host DMA 内存和 VRAM 块归还前都经 gpgpu_storage_release_ok()
 * 回写并失效 dcache 中的旧行；不能确认时不归还，Host 内存转入
 * dev->retired，VRAM 块转入 dev->reserved_blocks，设备释放时再回收。
 */
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/firmware.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include "gpgpu_drv.h"

/* DMA 地址的可用范围，等于 Outbound 窗口长度（31 位）。 */
#define GPGPU_DMA_LIMIT GPGPU_OUTBOUND_SIZE
/* 设备只读零源的字节数，不随 BO 大小增长。 */
#define DEV_ZERO_BYTES SZ_64K

/* 只供 A/B 测量；默认保持原有 BAR2 清零，加载后不能改写。 */
static bool vram_clear_dma;
module_param(vram_clear_dma, bool, 0444);
MODULE_PARM_DESC(vram_clear_dma, "未发布 VRAM BO 使用 DMA 清零，默认关闭");

/* ---- dcache ---- */

/**
 * gpgpu_dcache_command() - 经 BAR0 发出一条 Host dcache 命令
 * @dev: 设备；调用者持 dev->lock，或设备尚未对外发布、已不再被并发使用。
 * @cmd: GPGPU_DCACHE_* 命令。
 * @addr: GPU 物理地址；@cmd 含 GPGPU_DCACHE_ALL 时忽略。
 * @size: 字节数，不超过 32 位。
 *
 * 设备在 CMD 写入时同步执行，随后读到的 STATUS 就是本次结果。
 *
 * Return: 0；遇到脏行或有在途访问为 -EBUSY；参数被拒绝为 -EINVAL；回写
 * 失败为 -EIO。
 */
int gpgpu_dcache_command(struct gpgpu_device *dev, u32 cmd, u64 addr, u64 size)
{
	if (size > U32_MAX)
		return -EINVAL;
	gpgpu_wreg64(dev, GPGPU_REG_DCACHE_ADDR_LO, addr);
	gpgpu_wreg(dev, GPGPU_REG_DCACHE_LENGTH, size);
	gpgpu_wreg(dev, GPGPU_REG_DCACHE_CMD, cmd);
	switch (gpgpu_rreg(dev, GPGPU_REG_DCACHE_STATUS)) {
	case GPGPU_DCACHE_OK:
		return 0;
	case GPGPU_DCACHE_DIRTY:
	case GPGPU_DCACHE_BUSY:
		return -EBUSY;
	case GPGPU_DCACHE_INVALID:
		return -EINVAL;
	default:
		return -EIO;
	}
}

/**
 * gpgpu_storage_release_ok() - 归还一段存储前处理 dcache 中的旧行
 * @dev: 设备；调用者持 dev->lock，或设备尚未对外发布、已不再被并发使用。
 * @addr: 存储的 GPU 物理地址。
 * @size: 字节数，不超过 32 位。
 *
 * 缓存行按 GPU 物理地址标记。存储分给新对象后，旧脏行回写会覆盖
 * 新内容，旧干净行会让新对象读到旧数据。设备在位时回写并失效这段
 * 范围；设备已移除时不再发命令，只有确认停止时整体复位已经清空
 * dcache，才可以归还。
 *
 * Return: 可以归还为 true；维护失败，或设备已移除但未确认停止时为
 * false，调用者必须保留存储。
 */
bool gpgpu_storage_release_ok(struct gpgpu_device *dev, u64 addr, u64 size)
{
	if (!READ_ONCE(dev->present))
		return dev->stopped;
	if (!gpgpu_dcache_command(dev, GPGPU_DCACHE_CLEAN | GPGPU_DCACHE_INVALIDATE, addr, size))
		return true;
	dev_err(&dev->pdev->dev, "dcache 维护失败，保留存储 0x%llx+%llu\n", addr, size);
	return false;
}

/* ---- DMA 缓冲区 ---- */

/*
 * gpgpu_dma_buf_alloc() - 分配一块清零的一致性 DMA 内存并换算 GPU 物理地址
 * @dev: 设备。
 * @buf: 输出。
 * @size: 字节数。
 * @align: DMA 地址的对齐要求（2 的幂）；dma_alloc_coherent 按分配阶对齐，
 *         调用者需保证 size 足以满足该对齐。
 *
 * DMA 地址必须整段落在 Outbound 窗口覆盖的 IOVA 范围内，才能换算成
 * GPU 物理地址 GPGPU_OUTBOUND_BASE + IOVA。例：IOVA 0x4100_2000 对应
 * 0xc100_2000；IOVA 0 也合法，对应窗口起点。
 *
 * 退役记录与存储一起分配，释放失败时直接使用它，不在释放路径上
 * 申请内存。地址不合格的存储从未交给设备，直接归还，不经 dcache
 * 维护。
 *
 * Return: 0，或 -ENOMEM / -ERANGE（失败时不保留任何分配）。
 */
int gpgpu_dma_buf_alloc(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf, size_t size,
			size_t align)
{
	struct gpgpu_retired_buf *retire = kzalloc(sizeof(*retire), GFP_KERNEL);
	dma_addr_t dma;
	void *cpu;

	if (!retire)
		return -ENOMEM;
	cpu = dma_alloc_coherent(&dev->pdev->dev, size, &dma, GFP_KERNEL);
	if (!cpu) {
		kfree(retire);
		return -ENOMEM;
	}
	if ((dma & (align - 1)) || dma >= GPGPU_DMA_LIMIT || size > GPGPU_DMA_LIMIT - dma) {
		dma_free_coherent(&dev->pdev->dev, size, cpu, dma);
		kfree(retire);
		return -ERANGE;
	}
	memset(cpu, 0, size);
	*buf = (struct gpgpu_dma_buf) {
		.cpu = cpu,
		.dma = dma,
		.addr = GPGPU_OUTBOUND_BASE + dma,
		.size = size,
		.retire = retire,
	};
	return 0;
}

/*
 * 归还存储并清空 buf。不能确认 dcache 已处置这段地址时，存储描述转入
 * dev->retired，设备释放时再归还。调用者持 dev->lock，或设备尚未对外
 * 发布、已不再被并发使用。
 */
void gpgpu_dma_buf_free(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf)
{
	struct gpgpu_retired_buf *retire = buf->retire;

	if (!buf->cpu)
		return;
	if (gpgpu_storage_release_ok(dev, buf->addr, buf->size)) {
		dma_free_coherent(&dev->pdev->dev, buf->size, buf->cpu, buf->dma);
		kfree(retire);
	} else {
		*retire = (struct gpgpu_retired_buf) {
			.cpu = buf->cpu,
			.dma = buf->dma,
			.size = buf->size,
		};
		list_add_tail(&retire->node, &dev->retired);
	}
	*buf = (struct gpgpu_dma_buf) { 0 };
}

/* ---- 引用计数 ---- */

/* 设备释放时归还 dev->retired 中的存储；设备已停止访问，不必维护。 */
static void gpgpu_release_retired(struct gpgpu_device *dev)
{
	struct gpgpu_retired_buf *retire, *next;

	list_for_each_entry_safe(retire, next, &dev->retired, node) {
		dma_free_coherent(&dev->pdev->dev, retire->size, retire->cpu, retire->dma);
		list_del(&retire->node);
		kfree(retire);
	}
}

/*
 * 最后一个引用：释放管理对象、IH、只读零源、保留存储与 VRAM 分配器。
 * DMA 仍可能在途时，会话或 stop 的引用保留设备。
 * 未发布设备尚未启动 DMA，沿原有初始化失败路径回收。
 * 保留的 VRAM 块在 dev->reserved_blocks 中，随分配器一起归还。
 */
static void gpgpu_device_release(struct kref *refs)
{
	struct gpgpu_device *dev = container_of(refs, struct gpgpu_device, refs);

	gpgpu_mgmt_fini(dev);
	gpgpu_dma_buf_free(dev, &dev->ih);
	gpgpu_dma_buf_free(dev, &dev->vram_zero);
	gpgpu_release_retired(dev);
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
 * 驱动要求：DMA 一致；刚复位，CP 在位且保持复位；VRAM 容量与共享
 * 布局一致；Doorbell BAR 至少 3 页。
 */
static bool gpgpu_device_supported(struct pci_dev *pdev, void __iomem *regs)
{
	return dev_is_dma_coherent(&pdev->dev) &&
		readl(regs + GPGPU_REG_CP_CTRL) == (GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD) &&
		!readl(regs + GPGPU_REG_VRAM_SIZE_HI) &&
		readl(regs + GPGPU_REG_VRAM_SIZE_LO) == GPGPU_VRAM_SIZE &&
		GPGPU_DOORBELL_BAR_SIZE / 3 >= PAGE_SIZE;
}

/*
 * gpgpu_device_create() - 建立设备对象
 *
 * probe 在复位并使能设备后调用，只分配软件对象和内存，不启动 CP。
 *
 * 1. 检查一致性 DMA、CP 在位且保持复位、VRAM 与共享布局一致，
 *    Doorbell BAR 至少容纳 3 页；
 * 2. 分配设备对象，初始化锁、链表与引用计数（probe 持有第一个引用）。
 *    Doorbell 第 0 页留给管理队列，管理请求序号从 1 开始；
 * 3. 建立 VRAM 分配器（预留首页），映射 BAR2 用于装载固件、清零对象
 *    和写页表；
 * 4. 分配 KIQ、HIQ 的 Ring 与 MQD，以及 IH Ring。CP 管理域与 IH 都按
 *    DEVICE 访问它们，不需要登记缓存属性。可选 DMA 零源在此后分配，
 *    失败保持 BAR2 清零；成功后只读，随设备的最终安全释放归还。
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
	INIT_LIST_HEAD(&dev->retired);
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

	/* 零源只在发布设备前写一次；失败不影响原有 BAR2 路径。 */
	if (vram_clear_dma) {
		ret = gpgpu_dma_buf_alloc(dev, &dev->vram_zero, DEV_ZERO_BYTES, DEV_ZERO_BYTES);
		if (ret)
			dev_warn(&pdev->dev, "DMA 零源分配失败 %d，使用 BAR2\n", ret);
	}
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
 * 第 2 步：Outbound 布局由设备固定为 [0x8000_0000, 4 GiB) → IOVA 0。驱动只核对
 * 起点、长度与 Host 基址和 gpgpu_dma_buf_alloc() 的换算一致，再打开窗口。
 */
static int gpgpu_enable_outbound(struct gpgpu_device *dev)
{
	if (gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_GPU_BASE) != GPGPU_OUTBOUND_BASE ||
	    gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_SIZE_LO) != lower_32_bits(GPGPU_OUTBOUND_SIZE) ||
	    gpgpu_rreg(dev, GPGPU_REG_OUTBOUND_SIZE_HI) != upper_32_bits(GPGPU_OUTBOUND_SIZE) ||
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
 * 跨进程 DMA 可能仍在途，设备会拒绝复位；在超时前反复尝试。设备只在
 * 复位的全部步骤（含 dcache 回写）完成后才把 GLOBAL_CTRL 清零，所以每次
 * 复位前先写 ENABLE，读回 0 说明的是本次复位成功，不会把上一次复位
 * 留下的 0 当成本次结果。
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
			gpgpu_wreg(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
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

/* 等中断处理函数返回，再取消并等待 IH 工作项；调用者不持 dev->lock。 */
static void gpgpu_sync_irqs(struct gpgpu_device *dev)
{
	for (unsigned int i = 0; i < GPGPU_MSIX_VECTORS; i++)
		synchronize_irq(pci_irq_vector(dev->pdev, i));
	cancel_work_sync(&dev->ih_work);
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
	gpgpu_sync_irqs(dev);

	/* 4. 与文件关闭串行，设备确认停止才回收 orphan 会话。 */
	mutex_lock(&dev->vm_serial);
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

	mutex_unlock(&dev->vm_serial);

	/* 5. BAR2 即将随 PCI 区域释放，之后不再经它访问 VRAM。 */
	pci_iounmap(dev->pdev, dev->vram);
	dev->vram = NULL;
}

/* ---- 运行中恢复 ---- */

/*
 * recover_reset() - 串行化寄存器访问后尝试整体复位
 *
 * 恢复路径调用，调用者持 vm_serial，不持其他驱动互斥锁。
 * 1. 依次取设备锁、管理锁与 DMA 锁，阻止 IH 消费及同步访问寄存器；
 * 2. 尝试整体复位，不能确认时不回收资源；
 * 3. 逆序释放锁，资源处理交给调用者。
 *
 * Return: 已确认设备停止访问时为 true。
 */
static bool recover_reset(struct gpgpu_device *dev)
{
	bool stopped;

	/* 1. 按锁序排除设备寄存器的其他同步使用者。 */
	mutex_lock(&dev->lock);
	mutex_lock(&dev->management_lock);
	mutex_lock(&dev->dma_lock);

	/* 2. 未确认停止时，调用者继续保留资源。 */
	stopped = gpgpu_reset_hardware(dev);

	/* 3. 这里只结束互斥，不回收任何对象。 */
	mutex_unlock(&dev->dma_lock);
	mutex_unlock(&dev->management_lock);
	mutex_unlock(&dev->lock);
	return stopped;
}

/*
 * recover_reclaim() - 设备确认复位后，结束旧会话对设备的占用
 *
 * gpgpu_device_recover() 第 4 步调用，调用者持 vm_serial 与 dev->lock。设备
 * 已停止访问内存，复位前的队列、VMID、管理请求和 IH 进度都已失效：
 *
 * 1. 放下等待超时的 COPY 为两端对象保留的 users 计数，清除 dma_uncertain；
 * 2. 旧会话中未卸载的队列已随复位卸载：放下 Ring 与进度的 users 计数并
 *    标为 dead，状态在恢复第 1 步已置为 FAULTED。各 VM 不再占有 VMID，
 *    复位前未确认的失效结果也随设备上下文清除；
 * 3. 暂置 stopped，回收 orphan 会话，它们拆除 VM 时不访问设备。
 *    vm_serial 挡住了文件关闭，打开新会话也被拒绝，遍历期间链表
 *    不会被别人修改；
 * 4. 释放全部管理请求结果区并重置管理 Ring，IH 从编号 0 重新消费。
 *
 * 仍有文件的旧会话连同对象、页表和 Doorbell 页保留到文件关闭，不交给新
 * 会话；对象的 CPU 映射另行保活各自的存储。
 */
static void recover_reclaim(struct gpgpu_device *dev)
{
	struct gpgpu_fpriv *fpriv, *next;
	struct gpgpu_queue *q;

	/* 1. 超时 COPY 的两端对象不会再被 DMA 访问。 */
	if (dev->copy_host) {
		dev->copy_host->users--;
		dev->copy_vram->users--;
		dev->copy_host = NULL;
		dev->copy_vram = NULL;
	}
	dev->dma_uncertain = false;

	/* 2. 复位清空了 HQD 与 VM 上下文。 */
	list_for_each_entry(fpriv, &dev->fprivs, node) {
		list_for_each_entry(q, &fpriv->queues, node) {
			if (q->dead)
				continue;
			q->ring->users--;
			q->progress->users--;
			q->dead = true;
			q->uncertain = false;
		}
		fpriv->vm.vmid = 0;
		fpriv->vm.failed = false;
		fpriv->reset_done = true;
	}
	dev->vmids = 0;

	/* 3. probe 仍持有一个设备引用，这里放下会话引用不会释放设备。 */
	dev->stopped = true;
	list_for_each_entry_safe(fpriv, next, &dev->fprivs, node) {
		if (!fpriv->orphan)
			continue;
		gpgpu_fpriv_destroy(fpriv);
		gpgpu_device_put(dev);
	}
	dev->stopped = false;

	/* 4. 管理通道与 IH 回到启动前的状态。 */
	gpgpu_mgmt_reset(dev);
	dev->ih_rptr = 0;
}

int gpgpu_device_recover(struct gpgpu_device *dev)
{
	struct gpgpu_fpriv *fpriv;
	bool stopped;
	int ret = 0;

	/* 1. 拒绝新工作，结束旧等待。 */
	mutex_lock(&dev->lock);
	if (!dev->present || dev->recovering) {
		ret = dev->present ? -EBUSY : -ENODEV;
		mutex_unlock(&dev->lock);
		return ret;
	}
	dev->recovering = true;
	WRITE_ONCE(dev->fatal, true);
	list_for_each_entry(fpriv, &dev->fprivs, node)
		fpriv->stale = true;
	gpgpu_fail_queues(dev, NULL, GPGPU_EVENT_ERROR_RESET);
	mutex_unlock(&dev->lock);

	/* 2. 此后没有 ioctl 或文件关闭与恢复并发。 */
	mutex_lock(&dev->vm_serial);

	/* 3. 复位确认后，异步代码也不再访问设备。 */
	stopped = recover_reset(dev);
	if (!stopped) {
		ret = -EIO;
		goto out;
	}
	gpgpu_sync_irqs(dev);

	/* 4. 旧任务的保留资源可以回收。 */
	mutex_lock(&dev->lock);
	recover_reclaim(dev);
	WRITE_ONCE(dev->fatal, false);
	mutex_unlock(&dev->lock);

	/* 5. 重新启动失败时先停住设备，再报告失败。 */
	gpgpu_wreg(dev, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
	ret = gpgpu_device_start(dev);
	if (ret) {
		WRITE_ONCE(dev->fatal, true);
		stopped = recover_reset(dev);
		gpgpu_sync_irqs(dev);
	}

out:
	/* 6. 解除恢复状态。 */
	mutex_lock(&dev->lock);
	if (!stopped)
		dev->dma_uncertain = true;
	dev->recovering = false;
	mutex_unlock(&dev->lock);
	mutex_unlock(&dev->vm_serial);
	if (ret) {
		dev_err(&dev->pdev->dev, "设备恢复失败（%d），%s\n", ret,
			stopped ? "设备已停止" : "未确认设备停止，保留全部资源");
		return ret;
	}
	schedule_work(&dev->ih_work);
	dev_info(&dev->pdev->dev, "设备恢复完成\n");
	return 0;
}
