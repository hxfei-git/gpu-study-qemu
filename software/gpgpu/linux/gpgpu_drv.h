/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU Linux 驱动：对象、锁与模块间接口
 *
 * 文件划分：
 *   gpgpu_main.c   PCI probe/remove 与中断入口
 *   gpgpu_dev.c    设备生命周期：固件装载、CP 启停、DMA 缓冲区
 *   gpgpu_mgmt.c   KIQ/HIQ 管理 Ring 与管理请求
 *   gpgpu_ih.c     中断上半部与 IH Ring 消费
 *   gpgpu_drm.c    DRM 驱动、ioctl 分发与文件会话
 *   gpgpu_bo.c     GEM 对象、固定存储与 COPY
 *   gpgpu_vram.c   gpu_buddy 管理的 VRAM
 *   gpgpu_vm.c     GPUVM 页表、绑定与故障诊断
 *   gpgpu_queue.c  用户 AQL 队列与 Doorbell 映射
 *   gpgpu_event.c  完成事件的创建、等待与失败传播
 *
 * 对象关系与寿命：
 *
 *   gpgpu_device（kref：probe、每个会话、每个 GEM 对象各持一个）
 *     ├─ gpgpu_fpriv（每个 DRM 文件一个；文件关闭时若设备仍可能访问其对象，
 *     │   保留为 orphan，直到设备停止）
 *     │    ├─ gpgpu_bo（GEM 对象；会话列表、GEM 句柄、GPU 绑定各持一个引用）
 *     │    ├─ gpgpu_queue（引用 Ring 与进度两个 BO）
 *     │    ├─ gpgpu_event（引用来源队列）
 *     │    └─ gpgpu_vm（VMID、页表页与通知页）
 *     └─ 管理 Ring、管理请求结果区、IH Ring
 *
 * 锁的顺序：vm_serial → lock → management_lock / dma_lock → irq_lock。
 * 等待设备完成时（管理请求、DMA、事件）释放 lock，避免阻塞 IH 消费。
 */
#ifndef GPGPU_DRV_H
#define GPGPU_DRV_H

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/gpu_buddy.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <drm/drm_gem.h>
#include "gpgpu/gpgpu_protocol.h"
#include "gpgpu/uapi/gpgpu_drm.h"

#if !IS_REACHABLE(CONFIG_DRM) || !IS_ENABLED(CONFIG_GPU_BUDDY)
#error "gpgpu 驱动需要内核启用 CONFIG_DRM 与 CONFIG_GPU_BUDDY"
#endif

/* 等待管理请求、DMA 与设备停止的上限，单位毫秒。 */
#define GPGPU_TIMEOUT_MS 5000U
/* Doorbell 页的 mmap 偏移令牌（页号），低于 GEM 的 mmap 偏移区间。 */
#define GPGPU_DOORBELL_MMAP_PAGE 0x10000UL
/* IH Ring 字节数。 */
#define GPGPU_IH_BYTES 4096U
/* 每条管理 Ring 的字节数；其后另有一页存放 R/W 进度。 */
#define GPGPU_MGMT_BYTES 4096U
/* 每个 VM 通知页中的 8 字节通知槽数。 */
#define GPGPU_NOTIFY_SLOTS (PAGE_SIZE / sizeof(u64))
/*
 * 每个会话可同时占用的 Doorbell 入口数（页内入口 0、1），包括已卸载但
 * 尚未回收的队列。设备只有两个用户队列槽，更多入口用不上。
 */
#define GPGPU_QUEUES_PER_FILE 2

struct drm_device;
struct drm_file;
struct gpgpu_device;
struct gpgpu_fpriv;

/**
 * struct gpgpu_dma_buf - 一块一致性 DMA 内存
 * @cpu: 内核虚拟地址；为 NULL 表示未分配。
 * @dma: DMA API 使用的原始地址（启用 IOMMU 时为 IOVA）。
 * @addr: 写入设备协议的 GPU 物理地址，即 GPGPU_OUTBOUND_BASE + IOVA。
 * @size: 字节数。
 */
struct gpgpu_dma_buf {
	void *cpu;
	dma_addr_t dma;
	u64 addr;
	size_t size;
};

/**
 * struct gpgpu_bo - 固定存储的 GEM 对象
 * @gem: GEM 基类。
 * @node: 所属会话 bos 链表的节点；会话结束后为空节点。
 * @fpriv: 所属会话；会话放弃对象后为 NULL。
 * @dev: 设备，对象独立持有一个设备引用。
 * @handle: 所属会话中的 GEM 句柄；句柄关闭后为 0。
 * @kind: GPGPU_MEMORY_HOST 或 GPGPU_MEMORY_VRAM。
 * @size: 应用请求的字节数。
 * @mapped_size: 页对齐后的字节数，即 CPU 映射与 GPU 绑定的长度。
 * @host: Host 对象的存储；分配长度为 2 的幂以满足对齐。
 * @vram_blocks: VRAM 对象持有的 buddy 块。
 * @phys_addr: 存储的 GPU 物理地址：Host 对象为 host.addr，VRAM 对象为 VRAM 偏移。
 * @gpu_addr: 绑定后队列使用的地址：GPUVM 开启时为 GPUVA，否则等于 phys_addr。
 * @bound: 持有 GPU 绑定引用。
 * @permissions: 绑定时的 GPGPU_VM_READ/WRITE/EXEC。
 * @users: 进行中的 COPY 与存活队列持有的使用计数；非零时不能解绑。
 *
 * 除 gem 与 dev 外的字段由 dev->lock 保护。
 */
struct gpgpu_bo {
	struct drm_gem_object gem;
	struct list_head node;
	struct gpgpu_fpriv *fpriv;
	struct gpgpu_device *dev;
	u32 handle;
	u32 kind;
	u64 size;
	u64 mapped_size;
	struct gpgpu_dma_buf host;
	struct list_head vram_blocks;
	u64 phys_addr;
	u64 gpu_addr;
	bool bound;
	u32 permissions;
	u32 users;
};

/**
 * struct gpgpu_queue - 一个用户 AQL 队列
 * @node: 所属会话 queues 链表节点。
 * @fpriv: 所属会话。
 * @ring: Ring 所在 BO。
 * @progress: R/W 进度所在 BO。
 * @rptr: 共享 R 的内核映射。
 * @wptr: 共享 W 的内核映射。
 * @mqd: 本队列独占的 MQD；地址也是 IH 记录匹配队列的键。
 * @id: 会话内唯一的队列编号。
 * @db_index: 会话 Doorbell 页内的 8 字节入口号。
 * @state: GPGPU_QUEUE_READY / UNLOADING / FAULTED。
 * @error: 首次队列错误。
 * @management_status: 最近一次管理请求的结果。
 * @dead: 已确认卸载，并已放下 Ring 与进度的使用计数。
 * @uncertain: 管理请求结果未知，CP 可能仍在访问，禁止回收。
 * @fault: 卸载前保存的首次 GPUVM 故障。
 *
 * 由 dev->lock 保护。
 */
struct gpgpu_queue {
	struct list_head node;
	struct gpgpu_fpriv *fpriv;
	struct gpgpu_bo *ring;
	struct gpgpu_bo *progress;
	u32 *rptr;
	u32 *wptr;
	struct gpgpu_dma_buf mqd;
	u32 id;
	u32 db_index;
	u32 state;
	u32 error;
	u32 management_status;
	bool dead;
	bool uncertain;
	struct gpgpu_vm_fault fault;
};

/**
 * struct gpgpu_event - 一个完成事件
 * @node: 所属会话 events 链表节点。
 * @queue: 来源队列；事件存活期间队列对象不被回收。
 * @mailbox: 通知槽的内核映射。
 * @mailbox_addr: 通知槽写入 Signal 的地址（GPUVA 或 GPU 物理地址）。
 * @slot: 通知页内的槽号。
 * @id: 会话内唯一的事件编号，非零。
 * @state: GPGPU_EVENT_PENDING / NOTIFIED / FAILED；先写状态再唤醒。
 * @error: 失败时的错误值。
 * @waiters: 正在等待的调用数；非零时禁止销毁。
 * @wait: 等待队列。
 *
 * 除 state 外由 dev->lock 保护；state 用 READ_ONCE/WRITE_ONCE 供无锁等待读取。
 */
struct gpgpu_event {
	struct list_head node;
	struct gpgpu_queue *queue;
	u64 *mailbox;
	u64 mailbox_addr;
	u32 slot;
	u32 id;
	u32 state;
	u32 error;
	unsigned int waiters;
	wait_queue_head_t wait;
};

/**
 * struct gpgpu_vm - 会话的 GPU 地址空间
 * @vmid: 用户 VMID；0 表示尚未绑定任何对象。
 * @root: 页表根的 VRAM 偏移；GPUVM 关闭时不使用。
 * @failed: 页表写入或失效结果未知，禁止继续修改并禁止建队列。
 * @tables: 驱动私有的页表页。
 * @notify: 通知页，事件的通知槽都在这一页内。
 * @notify_slots: 通知槽占用位图。
 *
 * 由 dev->lock 保护。
 */
struct gpgpu_vm {
	u32 vmid;
	u32 root;
	bool failed;
	struct list_head tables;
	struct gpgpu_dma_buf notify;
	DECLARE_BITMAP(notify_slots, GPGPU_NOTIFY_SLOTS);
};

/**
 * struct gpgpu_fpriv - 一个 DRM 文件对应的会话
 * @node: 设备 fprivs 链表节点。
 * @dev: 设备，会话持有一个设备引用。
 * @file: DRM 文件；文件关闭后保留的 orphan 会话不再使用。
 * @owner: 创建会话的线程组；其他进程不能使用本会话。
 * @bos: 会话拥有的 GEM 对象。
 * @queues: 用户队列，包括已卸载但仍被事件引用的队列。
 * @events: 完成事件。
 * @vm: GPU 地址空间。
 * @next_queue: 下一个队列编号；回绕为 0 后不再分配。
 * @next_event: 下一个事件编号；回绕为 0 后不再分配。
 * @db_page: 设备 Doorbell BAR 中分给本会话的页号；0 表示未分配。
 * @db_maps: Doorbell 页的存活 VMA 数。
 * @orphan: 文件已关闭但设备访问尚未结束，等设备停止后回收。
 *
 * 除 db_maps 外由 dev->lock 保护。
 */
struct gpgpu_fpriv {
	struct list_head node;
	struct gpgpu_device *dev;
	struct drm_file *file;
	pid_t owner;
	struct list_head bos;
	struct list_head queues;
	struct list_head events;
	struct gpgpu_vm vm;
	u32 next_queue;
	u32 next_event;
	unsigned int db_page;
	atomic_t db_maps;
	bool orphan;
};

/**
 * struct gpgpu_mgmt_ring - 驱动拥有的 KIQ 或 HIQ
 * @mqd: 管理队列的 MQD。
 * @storage: Ring 加一页进度：R 在偏移 GPGPU_MGMT_BYTES，W 在其后 8 字节。
 * @rptr: R 的内核映射。
 * @wptr: W 的内核映射。
 * @doorbell: BAR4 内的 Doorbell 偏移。
 * @failed: IH 报告该管理队列出错，停止发布与等待。
 */
struct gpgpu_mgmt_ring {
	struct gpgpu_dma_buf mqd;
	struct gpgpu_dma_buf storage;
	u32 *rptr;
	u32 *wptr;
	u32 doorbell;
	bool failed;
};

/**
 * struct gpgpu_mgmt_request - 一次管理请求独占的结果区
 * @node: 设备 requests 链表节点。
 * @result: 16 字节 GpgpuManagementResult。
 *
 * 请求已发布但未确认完成（超时或通道故障）时保留，避免 CP 迟到的写回
 * 落到被复用的内存上。
 */
struct gpgpu_mgmt_request {
	struct list_head node;
	struct gpgpu_dma_buf result;
};

/**
 * struct gpgpu_device - 设备
 * @pdev: PCI 设备（持有引用）。
 * @regs: BAR0 映射。
 * @doorbell: BAR4 映射。
 * @vram: BAR2 映射，用于装载固件、清零 BO 与写页表。
 * @refs: probe、每个会话、每个 GEM 对象各持一个引用。
 * @lock: 保护对象链表、对象状态、IH 消费与 VM 修改。
 * @vm_serial: 串行化会跨越等待的 ioctl（队列与映射变更），事件等待除外。
 * @management_lock: 串行化管理命令的发布，不覆盖等待阶段。
 * @dma_lock: 串行化整个 COPY 请求，包括等待完成。
 * @irq_lock: 保护中断使能、确认与 DMA 完成状态。
 * @ih_work: 在可睡眠上下文中消费 IH 记录。
 * @dma_done: DMA 完成或失败时唤醒。
 * @present: BAR 仍可访问；移除时最先清除。
 * @stopped: 设备已确认停止访问，可以回收 orphan 会话。
 * @fatal: KIQ、IH 或固件发生设备级故障。
 * @dma_failed: 本次 DMA 报告了错误。
 * @dma_uncertain: DMA 等待超时，无法证明访问已结束；之后禁止 COPY 与回收。
 * @gpuvm: 设备以 gpuvm=on 启动。
 * @vram_size: VRAM 字节数；VRAM 占 GPU 物理地址 [0, vram_size)。
 * @vmids: 用户 VMID 1、2 的占用位图。
 * @drm: DRM 设备。
 * @fprivs: 全部会话，包括 orphan。
 * @requests: 未回收的管理请求结果区。
 * @buddy: 管理固件区之前 VRAM 的分配器。
 * @reserved_blocks: 分配器中预留的 VRAM 首页。
 * @buddy_ready: 分配器已初始化。
 * @db_pages: Doorbell 页占用位图；第 0 页给管理队列。
 * @next_sequence: 下一个管理请求序号，从 1 开始，回绕后不再发布。
 * @mgmt: KIQ（下标 0）与 HIQ（下标 1）。
 * @ih: IH Ring。
 * @ih_rptr: 下一条待消费的 IH 记录编号。
 */
struct gpgpu_device {
	struct pci_dev *pdev;
	void __iomem *regs;
	void __iomem *doorbell;
	void __iomem *vram;
	struct kref refs;
	struct mutex lock;
	struct mutex vm_serial;
	struct mutex management_lock;
	struct mutex dma_lock;
	spinlock_t irq_lock;
	struct work_struct ih_work;
	struct completion dma_done;
	bool present;
	bool stopped;
	bool fatal;
	bool dma_failed;
	bool dma_uncertain;
	bool gpuvm;
	u32 vram_size;
	unsigned long vmids;
	struct drm_device *drm;
	struct list_head fprivs;
	struct list_head requests;
	struct gpu_buddy buddy;
	struct list_head reserved_blocks;
	bool buddy_ready;
	unsigned long db_pages;
	u64 next_sequence;
	struct gpgpu_mgmt_ring mgmt[2];
	struct gpgpu_dma_buf ih;
	u32 ih_rptr;
};

/* ---- 寄存器访问 ---- */

static inline u32 gpgpu_rreg(struct gpgpu_device *dev, u32 reg)
{
	return readl(dev->regs + reg);
}

static inline void gpgpu_wreg(struct gpgpu_device *dev, u32 reg, u32 value)
{
	writel(value, dev->regs + reg);
}

/* 先低后高写入 LO/HI 寄存器对；只用于静态配置，不用于发布动态共享值。 */
static inline void gpgpu_wreg64(struct gpgpu_device *dev, u32 reg, u64 value)
{
	writel(lower_32_bits(value), dev->regs + reg);
	writel(upper_32_bits(value), dev->regs + reg + 4);
}

/* 经 Host 选槽窗口读取 HQD 字段。 */
static inline u32 gpgpu_hqd_read(struct gpgpu_device *dev, u32 slot, u32 field)
{
	gpgpu_wreg(dev, GPGPU_REG_HQD_SELECT, slot);
	return gpgpu_rreg(dev, GPGPU_REG_HQD_WINDOW + field);
}

/* 经 Host 选槽窗口读取槽绑定的 MQD 地址。 */
static inline u64 gpgpu_hqd_mqd(struct gpgpu_device *dev, u32 slot)
{
	return gpgpu_hqd_read(dev, slot, GPGPU_HQD_MQD_LO) |
		(u64)gpgpu_rreg(dev, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_MQD_HI) << 32;
}

/* ---- gpgpu_dev.c ---- */

struct gpgpu_device *gpgpu_device_create(struct pci_dev *pdev, void __iomem *regs,
					 void __iomem *doorbell, bool gpuvm);
int gpgpu_device_start(struct gpgpu_device *dev);
void gpgpu_device_stop(struct gpgpu_device *dev);
void gpgpu_device_get(struct gpgpu_device *dev);
void gpgpu_device_put(struct gpgpu_device *dev);
int gpgpu_dma_buf_alloc(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf, size_t size,
			size_t align);
void gpgpu_dma_buf_free(struct gpgpu_device *dev, struct gpgpu_dma_buf *buf);

/* ---- gpgpu_mgmt.c ---- */

int gpgpu_mgmt_init(struct gpgpu_device *dev);
void gpgpu_mgmt_fini(struct gpgpu_device *dev);
int gpgpu_mgmt_submit(struct gpgpu_device *dev, unsigned int ring, u32 opcode, u32 kind,
		      u64 mqd, u32 *status, bool *published);

/* ---- gpgpu_ih.c ---- */

irqreturn_t gpgpu_irq_handler(int irq, void *data);
void gpgpu_ih_work(struct work_struct *work);
void gpgpu_ih_drain(struct gpgpu_device *dev);

/* ---- gpgpu_drm.c ---- */

int gpgpu_drm_register(struct gpgpu_device *dev);
void gpgpu_drm_unregister(struct gpgpu_device *dev);
void gpgpu_fpriv_destroy(struct gpgpu_fpriv *fpriv);

/* ---- gpgpu_vram.c ---- */

int gpgpu_vram_init(struct gpgpu_device *dev);
void gpgpu_vram_fini(struct gpgpu_device *dev);
int gpgpu_vram_alloc(struct gpgpu_device *dev, u64 size, struct list_head *blocks, u64 *offset);
void gpgpu_vram_free(struct gpgpu_device *dev, struct list_head *blocks);

/* ---- gpgpu_bo.c ---- */

int gpgpu_bo_create(struct gpgpu_fpriv *fpriv, struct gpgpu_bo_create *args);
struct gpgpu_bo *gpgpu_bo_lookup(struct gpgpu_fpriv *fpriv, u32 handle);
bool gpgpu_bo_range_ok(struct gpgpu_bo *bo, u64 offset, u64 size);
void gpgpu_bo_drop(struct gpgpu_bo *bo);
void gpgpu_bo_release_all(struct gpgpu_fpriv *fpriv);
int gpgpu_bo_copy(struct gpgpu_fpriv *fpriv, struct gpgpu_memory_copy *args);

/* ---- gpgpu_vm.c ---- */

int gpgpu_vm_map(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args);
int gpgpu_vm_unmap(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args);
int gpgpu_vm_cleanup(struct gpgpu_fpriv *fpriv);
void gpgpu_vm_save_fault(struct gpgpu_queue *q);
void gpgpu_vm_clear_free_faults(struct gpgpu_device *dev);

/* ---- gpgpu_queue.c ---- */

int gpgpu_queue_create(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_create *args);
int gpgpu_queue_query(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args);
int gpgpu_queue_destroy(struct gpgpu_fpriv *fpriv, struct gpgpu_queue_state *args);
struct gpgpu_queue *gpgpu_queue_lookup(struct gpgpu_fpriv *fpriv, u32 id);
bool gpgpu_queue_any_live(struct gpgpu_fpriv *fpriv);
void gpgpu_queue_free(struct gpgpu_queue *q);
int gpgpu_doorbell_mmap(struct gpgpu_fpriv *fpriv, struct vm_area_struct *vma);

/* ---- gpgpu_event.c ---- */

int gpgpu_event_create(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args);
int gpgpu_event_query(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args);
int gpgpu_event_wait(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args);
int gpgpu_event_destroy(struct gpgpu_fpriv *fpriv, struct gpgpu_event_request *args);
void gpgpu_event_release_all(struct gpgpu_fpriv *fpriv);
void gpgpu_event_signal(struct gpgpu_queue *q, u32 event_id);
void gpgpu_fail_queues(struct gpgpu_device *dev, struct gpgpu_queue *queue, u32 error);

#endif /* GPGPU_DRV_H */
