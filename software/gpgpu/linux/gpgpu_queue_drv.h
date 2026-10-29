/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_QUEUE_DRV_H
#define GPGPU_QUEUE_DRV_H

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gpu_buddy.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "gpgpu/gpgpu_protocol.h"
#include "gpgpu/uapi/gpgpu_drm.h"
#if IS_REACHABLE(CONFIG_DRM)
#include <drm/drm_gem.h>
#endif
struct drm_device;
struct drm_file;

/* 驱动管理请求与队列模式 DMA 的等待上限，单位毫秒。 */
#define GPGPU_Q_TIMEOUT_MS 5000U
/* GPUVM 的 Doorbell 令牌低于 GEM 的 4 GiB 偏移起点。 */
#define GPGPU_VM_DB_MMAP_PAGE 0x10000UL
/* 驱动分配的 IH Ring 字节容量。 */
#define GPGPU_IH_BYTES 4096U
/* 每条管理 Ring 的字节容量，不含附加进度页。 */
#define GPGPU_MGMT_BYTES 4096U

struct gpgpu_qdev;
struct gpgpu_qclient;
/**
 * struct gpgpu_qmemory - 由会话持有的内存对象，设备锁保护对象生命周期
 * @node: 所属会话 memory 链表的节点。
 * @client: 拥有此对象的会话。
 * @id: 会话内唯一的内存句柄。
 * @kind: Host 一致性 DMA 内存或 VRAM 分配。
 * @offset: VRAM 对象的字节偏移。
 * @users: 队列和进行中的 DMA 持有的引用数。
 * @size: 实际分配的字节数。
 * @cpu: Host 对象的内核虚拟地址，VRAM 对象不使用。
 * @dma: Host 对象的完整 DMA 地址；启用系统 IOMMU 时为 IOVA。
 * @blocks: VRAM 对象持有的 gpu_buddy 块链表。
 * @dev: 独立持有引用的设备状态。
 * @gem: 包装固定存储的 GEM 对象。
 * @gem_ready: 已建立 GEM 创建引用，列表持有该引用。
 * @logical_size: 应用请求的逻辑字节数。
 * @mapped_size: CPU/GPU 可见的 4 KiB 对齐字节数。
 * @gpu_address: 绑定后供用户队列使用的地址。
 * @physical_address: 固定存储的 GPU 物理地址。
 * @bound: 是否持有绑定引用，不由地址值推断。
 * @permissions: GPU 读、写和取指权限位。
 */
struct gpgpu_qmemory {
	struct list_head node;
	struct gpgpu_qclient *client;
	struct gpgpu_qdev *dev; /* GEM 最后释放不依赖文件上下文。 */
#if IS_REACHABLE(CONFIG_DRM)
	struct drm_gem_object gem; /* 固定存储的 GEM 包装。 */
#endif
	bool gem_ready;	  /* 创建引用由会话列表持有。 */
	u64 logical_size; /* 应用申请长度。 */
	u64 mapped_size;  /* GPU 映射与 CPU mmap 可见的页对齐长度。 */
	u64 gpu_address; /* 绑定后供用户队列使用的地址。 */
	u64 physical_address; /* 固定存储的 GPU 物理地址。 */
	bool bound; /* 独立于地址值和页表存在的绑定引用。 */
	u32 permissions;  /* GPU 读写取指权限。 */
	u32 id;
	u32 kind;
	u32 offset;
	u32 users;
	size_t size;
	void *cpu;
	dma_addr_t dma;
	struct list_head blocks;
};

/**
 * struct gpgpu_user_queue - 一个用户队列的 Host 对象与 CP 绑定状态
 * @node: 所属会话 queues 链表节点。
 * @client: 拥有队列的会话。
 * @ring: 被队列引用的 Ring 内存对象。
 * @progress: 被队列引用的 R/W 内存对象。
 * @rptr: 内核映射中的 32 位消费进度地址。
 * @wptr: 内核映射中的 32 位预留进度地址。
 * @mqd: 独占 MQD 的内核虚拟地址。
 * @mqd_dma: MQD 的原始 DMA 地址，仅供 DMA API 使用。
 * @mqd_address: MQD 的 GPU 物理地址，也是 IH 队列匹配键。
 * @id: 会话内唯一队列编号。
 * @db_index: 会话 Doorbell 页内的 8 字节入口编号。
 * @state: 对用户回报的 READY/UNLOADING/FAULTED 状态。
 * @error: 保存的首次队列错误。
 * @management_status: 最近一次管理请求的 CP 结果码。
 * @dead: 已确认卸载，并已放下 Ring 和进度内存引用。
 * @uncertain: 尚不能确认映射或卸载结果，禁止普通回收。
 * @fault: 软件保存的源队列首次 GPUVM 故障。
 */
struct gpgpu_user_queue {
	struct list_head node;
	struct gpgpu_qclient *client;
	struct gpgpu_qmemory *ring;
	struct gpgpu_qmemory *progress;
	u32 *rptr;
	u32 *wptr;
	GpgpuMqd *mqd;
	dma_addr_t mqd_dma;
	u64 mqd_address; /* 设备协议与 IH 匹配使用 GPU 物理地址。 */
	u32 id;
	u32 db_index;
	u32 state;
	u32 error;
	u32 management_status;
	bool dead;
	bool uncertain;
	struct gpgpu_vm_fault fault; /* 槽复用前保存诊断。 */
};

/**
 * struct gpgpu_qevent - 驱动持有的通知槽、持久事件状态与等待队列
 * @node: 所属会话 events 链表节点。
 * @queue: 产生通知的源队列，事件存活期间不能删除该对象。
 * @mailbox: 8 字节一致性通知槽的内核映射。
 * @mailbox_address: 通知槽的 GPU 地址，随设备模式采用 GPUVA 或物理地址。
 * @id: 会话内非零且不复用的存活事件编号。
 * @state: 先保存状态再唤醒，多个等待者均可观察。
 * @error: 事件失败时保存的错误值。
 * @waiters: 当前等待调用数，非零时禁止销毁事件。
 * @wait: 阻塞等待者登记的 Linux 等待队列。
 * @mailbox_slot: 通知页内的 8 字节槽编号。
 */
struct gpgpu_qevent {
	struct list_head node;
	struct gpgpu_user_queue *queue;
	u64 *mailbox;
	u64 mailbox_address;
	u32 id;
	u32 state;
	u32 error;
	u32 mailbox_slot; /* GPUVM 通知页内槽号。 */
	unsigned int waiters;
	wait_queue_head_t wait;
};

/**
 * struct gpgpu_qclient - 一次打开产生的队列会话，必要时保留为 orphan
 * @node: 设备 clients 链表节点。
 * @memory: 本会话的内存对象链表。
 * @queues: 本会话的队列对象链表，包含尚不能回收的旧对象。
 * @events: 本会话的事件对象链表。
 * @dev: 所属设备，会话持有一个设备引用。
 * @next_memory: GEM 包装前分配临时内部编号，回绕为零后停止分配。
 * @next_queue: 下一个队列编号，回绕为零后停止分配。
 * @next_event: 下一个事件编号，回绕为零后停止分配。
 * @db_page: 设备 Doorbell 空间内的页号，零表示尚未分配。
 * @db_maps: 本会话 Doorbell 页的存活用户 VMA 数。
 * @orphan: 文件已关闭但设备访问未结束，保留到设备停机。
 * @owner: 创建会话的线程组 ID，用于拒绝其他进程复用。
 * @drm_file: DRM 文件上下文。
 * @vmid: 本对象所属的设备地址空间编号，管理访问为零。
 * @root: 根目录在 VRAM 中的字节偏移。
 * @vm_failed: 页表更新失败或结果未知，禁止重建 Queue。
 * @tables: 驱动私有页表页链表。
 * @notify_cpu: 内部通知页的内核映射，不映射给应用。
 * @notify_dma: 内部通知页的原始 DMA 地址。
 * @notify_address: 内部通知页的 GPU 物理地址。
 * @notify_slots: 通知页的 512 个槽占用位图。
 */
struct gpgpu_qclient {
	struct list_head node;
	struct list_head memory;
	struct list_head queues;
	struct list_head events;
	struct gpgpu_qdev *dev;
	u32 next_memory;
	u32 next_queue;
	u32 next_event;
	unsigned int db_page;
	atomic_t db_maps;
	bool orphan;
	pid_t owner;
	struct drm_file *drm_file; /* DRM 文件上下文。 */
	u32 vmid;		   /* 首次绑定取得并持有到上下文清理。 */
	u32 root;		   /* VRAM 页表根偏移。 */
	bool vm_failed;		   /* 更新结果未知时禁止重建队列。 */
	struct list_head tables;   /* 驱动私有表页，不进入用户对象链表。 */
	void *notify_cpu;	   /* 内部 4 KiB 通知页。 */
	dma_addr_t notify_dma;
	u64 notify_address; /* 通知页的 GPU 物理地址。 */
	DECLARE_BITMAP(notify_slots, 512);
};

/**
 * struct gpgpu_management_ring - 驱动拥有的 KIQ 或 HIQ 及共享进度
 * @mqd: 管理队列 MQD 的内核映射。
 * @mqd_dma: 管理队列 MQD 的原始 DMA 地址。
 * @mqd_address: 管理队列 MQD 的 GPU 物理地址。
 * @storage: Ring 与附加进度页的连续内核映射。
 * @dma: storage 的原始 DMA 基址。
 * @address: storage 的 GPU 物理基址。
 * @rptr: 附加页内的 32 位消费进度。
 * @wptr: 附加页内的 32 位已发布进度。
 * @doorbell: BAR4 内的 Doorbell 字节偏移。
 * @failed: IH 已报告该管理通道故障，停止后续发布和等待。
 */
struct gpgpu_management_ring {
	GpgpuMqd *mqd;
	dma_addr_t mqd_dma;
	u64 mqd_address; /* 设备协议与 IH 匹配使用 GPU 物理地址。 */
	void *storage;
	dma_addr_t dma;
	u64 address; /* Ring 与进度存储的 GPU 物理基址。 */
	u32 *rptr;
	u32 *wptr;
	u32 doorbell;
	bool failed;
};

/**
 * struct gpgpu_management_request - 一次管理请求独占的结果区，超时后继续保留
 * @node: 设备 requests 链表节点。
 * @result: 16 字节结果区的内核映射。
 * @dma: 结果区的原始 DMA 地址。
 * @address: 结果区的 GPU 物理地址。
 */
struct gpgpu_management_request {
	struct list_head node;
	GpgpuManagementResult *result;
	dma_addr_t dma;
	u64 address; /* 完成写回使用的 GPU 物理地址。 */
};

/**
 * struct gpgpu_qdev - 队列模式设备上下文，串联管理、DMA、IH 和会话
 * @pdev: 持有引用的 PCI 设备。
 * @regs: BAR0 控制寄存器的内核 MMIO 映射。
 * @doorbell: BAR4 Doorbell 空间的内核映射。
 * @vram: BAR2 显存映射，用于加载 CP 固件。
 * @refs: probe、会话及 orphan 持有的设备引用。
 * @lock: 保护对象链表、生命周期和 IH 消费；等待时暂时释放。
 * @management_lock: 串行发布相邻管理命令对，不覆盖等待阶段。
 * @dma_lock: 串行执行整个 DMA 请求，包括等待完成。
 * @irq_lock: 保护中断屏蔽、确认和 DMA 完成状态准备。
 * @ih_work: 在可睡眠上下文消费 IH 记录的工作项。
 * @dma_done: DMA 中断唤醒的完成量。
 * @present: BAR 是否仍可用；移除时先清除此标记。
 * @stopped: 复位、中断与异步工作都结束后置位，可回收 orphan。
 * @fatal: KIQ、IH 或固件发生设备级故障。
 * @dma_failed: 本次 DMA 已报告错误。
 * @dma_uncertain: DMA 等待超时，尚不能证明相关访问结束。
 * @started: CP 是否已由启动路径释放复位。
 * @clients: 所有打开及保留会话的链表。
 * @requests: 尚存活的管理结果区链表。
 * @reserved_blocks: gpu_buddy 中保留的 VRAM 首页。
 * @buddy: 仅管理固件区之前 VRAM 的分配器。
 * @buddy_ready: gpu_buddy 是否已成功初始化。
 * @db_pages: Doorbell 页分配位图，第零页专供内核管理队列。
 * @next_sequence: 下一个管理完成序号，从 1 开始且不回绕复用。
 * @management: KIQ 和 HIQ，分别使用下标 0 和 1。
 * @ih: Host IH Ring 的内核虚拟地址。
 * @ih_dma: IH Ring 的原始 DMA 地址。
 * @ih_address: IH Ring 的 GPU 物理地址。
 * @vram_size: 设备显存大小，也是 Outbound 的 GPU 基址。
 * @ih_rptr: Host 下一条待消费的 32 位记录编号。
 * @vm_serial: 跨管理等待串行队列和映射变更，不阻塞事件等待。
 * @gpuvm: 本连接或设备是否采用 GPUVM 地址语义。
 * @vmids: 用户 VMID 1、2 的占用位图。
 * @drm: 两种地址模式共用的 DRM 设备。
 */
struct gpgpu_qdev {
	struct pci_dev *pdev;
	void __iomem *regs;
	void __iomem *doorbell;
	void __iomem *vram;
	struct kref refs;
	/*
	 * lock 保护对象和 IH；
	 * 管理发布和 DMA 各有独立锁，等待不持 lock。
	 */
	struct mutex lock;
	struct mutex management_lock;
	struct mutex vm_serial; /* 跨管理等待串行 GPUVM 队列和页表变更。 */
	struct mutex dma_lock;
	spinlock_t irq_lock;
	struct work_struct ih_work;
	struct completion dma_done;
	bool present;
	bool stopped;
	bool fatal;
	bool dma_failed;
	bool dma_uncertain;
	bool started;
	bool gpuvm;		/* 本次启动使用 GPUVM。 */
	unsigned long vmids;	/* 固定用户 VMID 占用位图。 */
	struct drm_device *drm; /* 两种地址模式共用。 */
	struct list_head clients;
	struct list_head requests;
	struct list_head reserved_blocks;
	struct gpu_buddy buddy;
	bool buddy_ready;
	unsigned long db_pages;
	u64 next_sequence;
	struct gpgpu_management_ring management[2];
	GpgpuIhRecord *ih;
	dma_addr_t ih_dma;
	u64 ih_address;
	u32 vram_size;
	u32 ih_rptr;
};

/*
 * probe 提供已映射 BAR；
 * 启动成功后由 CP 持续访问管理对象。
 */
/* 建立队列模式设备上下文。 */
struct gpgpu_qdev *gpgpu_qdev_create(struct pci_dev *pdev, void __iomem *regs,
				     void __iomem *doorbell, bool gpuvm);
/* 加载固件并建立 KIQ 到 HIQ 的管理路径。 */
int gpgpu_qdev_start(struct gpgpu_qdev *dev);
/* 在移除或启动失败时停止设备访问。 */
void gpgpu_qdev_stop(struct gpgpu_qdev *dev);
/* 放下 PCI 入口持有的队列设备引用。 */
void gpgpu_qdev_put(struct gpgpu_qdev *dev);
/* 处理队列模式的中断上半部。 */
irqreturn_t gpgpu_qdev_irq(struct gpgpu_qdev *dev);
/* 创建一次打开对应的队列会话。 */
int gpgpu_qopen(struct gpgpu_qdev *dev, struct file *file);
/* 结束文件对应的队列会话。 */
int gpgpu_qrelease(struct inode *inode, struct file *file);
/* 建立 Doorbell 用户映射；CPU 存储映射由 GEM 保活。 */
int gpgpu_qmmap(struct file *file, struct vm_area_struct *vma);

/*
 * 以下对象操作由调用者持 dev->lock，
 * 带 wait 的操作明确释放该锁。
 */
/* 建立队列模式的 VRAM 分配器。 */
int gpgpu_memory_init(struct gpgpu_qdev *dev);
/* 销毁 VRAM 分配器。 */
void gpgpu_memory_fini(struct gpgpu_qdev *dev);
/* 按句柄查找会话内存对象。 */
struct gpgpu_qmemory *gpgpu_memory_find(struct gpgpu_qclient *client, u32 id);
/* 检查对象内字节范围。 */
bool gpgpu_memory_range(struct gpgpu_qmemory *memory, u64 offset, u64 size);
/* 为会话分配 Host 或 VRAM 对象。 */
int gpgpu_memory_alloc(struct gpgpu_qclient *client, struct gpgpu_bo_create *r);
/* 释放已结束会话的全部内存。 */
void gpgpu_memory_release_all(struct gpgpu_qclient *client);
/* 通过 DMA 同步搬运两个对象内的数据。 */
int gpgpu_memory_copy(struct gpgpu_qclient *client, struct gpgpu_memory_copy *r);
/* 在工作队列中处理 IH 通知。 */
void gpgpu_ih_work(struct work_struct *work);
/* 消费当前已发布的 IH 记录。 */
void gpgpu_ih_drain_locked(struct gpgpu_qdev *dev);
/* 向受影响队列及等待者发布失败。 */
void gpgpu_events_fault(struct gpgpu_qdev *dev, struct gpgpu_user_queue *queue, u32 error);
/* 执行事件创建、查询、等待或销毁。 */
int gpgpu_event_operation(struct gpgpu_qclient *client, unsigned int command,
			  struct gpgpu_event_request *r);
/* 释放会话的所有事件。 */
void gpgpu_event_release_all(struct gpgpu_qclient *client);
/* 按队列编号查找会话对象。 */
struct gpgpu_user_queue *gpgpu_queue_find(struct gpgpu_qclient *client, u32 id);
/* 供 DRM 入口复用的队列对象操作；调用方持设备锁。 */
int gpgpu_queue_create(struct gpgpu_qclient *c, struct gpgpu_queue_create *r);
int gpgpu_queue_operation(struct gpgpu_qclient *c, unsigned int command,
			  struct gpgpu_queue_state *r);
/* GEM 存储回收及句柄验证。 */
void gpgpu_memory_storage_free(struct gpgpu_qmemory *m);
/* 绑定生命周期和 DRM 入口；缺少所需内核配置时拒绝注册。 */
int gpgpu_drm_register(struct gpgpu_qdev *dev);
void gpgpu_drm_unregister(struct gpgpu_qdev *dev);
void gpgpu_gem_drop(struct gpgpu_qmemory *m);
int gpgpu_vm_cleanup(struct gpgpu_qclient *c);
bool gpgpu_vm_queue_live(struct gpgpu_qclient *c);
void gpgpu_vm_save_fault(struct gpgpu_user_queue *q);
/* 只转换已分配的完整 DMA 区间，DMA API 始终使用原 dma_addr_t。 */
static inline bool gpgpu_dma_address(struct gpgpu_qdev *dev, dma_addr_t dma,
                                     u64 size, u64 *address)
{
	u64 iova = (u64)dma;
	u64 limit = 1ULL << 31;
	u64 window_size = (1ULL << 32) - dev->vram_size;

	if (!size || iova >= limit || size > limit - iova ||
	    iova >= window_size || size > window_size - iova)
		return false;
	*address = (u64)dev->vram_size + iova;
	return true;
}

/*
 * gpgpu_qwrite64() - 写入一对 32 位地址配置寄存器
 *
 * 先低位后高位，适用于静态配置，不用于发布动态 64 位共享内存值。
 */
static inline void gpgpu_qwrite64(struct gpgpu_qdev *dev, u32 reg, u64 value)
{
	writel(lower_32_bits(value), dev->regs + reg);
	writel(upper_32_bits(value), dev->regs + reg + 4);
}
#endif
