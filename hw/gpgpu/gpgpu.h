/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#ifndef HW_GPGPU_H
#define HW_GPGPU_H

#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "qom/object.h"
#include "gpgpu_regs.h"
#include "software/gpgpu/include/gpgpu_queue.h"

/* QOM 注册名，供命令行 -device 与类型转换使用。 */
#define TYPE_GPGPU "gpgpu"
OBJECT_DECLARE_SIMPLE_TYPE(GPGPUState, GPGPU)

/* 教学设备的 PCI 厂商编号。 */
#define GPGPU_VENDOR_ID 0x1234
/* 教学设备的 PCI 设备编号。 */
#define GPGPU_DEVICE_ID 0x1337
/* PCI 修订版本号。 */
#define GPGPU_REVISION 0x01
/* PCI 设备类别，声明为 3D 控制器。 */
#define GPGPU_CLASS_CODE PCI_CLASS_DISPLAY_3D

/* BAR0 控制空间的字节容量，含 MSI/MSI-X 表。 */
#define GPGPU_CTRL_BAR_SIZE (1 * 1024 * 1024)
/* BAR2 默认显存字节容量。 */
#define GPGPU_VRAM_BAR_SIZE (64 * 1024 * 1024)
/* BAR4 Doorbell 空间的字节容量。 */
#define GPGPU_DOORBELL_BAR_SIZE (64 * 1024)
/* DEV_CAPS 默认报告的计算单元数。 */
#define GPGPU_DEFAULT_NUM_CUS 4
/* DEV_CAPS 默认报告的每计算单元 warp 数。 */
#define GPGPU_DEFAULT_WARPS_PER_CU 4
/* 默认每 warp 的 lane 数，解释器固定支持 32。 */
#define GPGPU_DEFAULT_WARP_SIZE 32
/* QOM 未指定 vram_size 时使用的字节容量。 */
#define GPGPU_DEFAULT_VRAM_SIZE GPGPU_VRAM_BAR_SIZE

/* 设备暴露的 MSI-X 向量总数。 */
#define GPGPU_MSIX_VECTORS 4
/* 同步 Kernel 完成使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_KERNEL 0
/* DMA 完成使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_DMA 1
/* 设备错误使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_ERROR 2
/* IH Ring 非空通知使用的 MSI-X 向量号。 */
#define GPGPU_MSIX_VEC_IH 3

/**
 * struct GPGPUKernelParams - 一次派发的配置快照，队列模式在提交时固定
 * @kernel_addr: RV32 代码入口；GPUVM 模式为 GPUVA，旧模式为 VRAM 偏移。
 * @kernel_args: 同步模式为 VRAM 偏移；物理队列为 DMA 地址；GPUVM 队列为 GPUVA。
 * @grid_dim: X/Y/Z 方向的工作组数，均非零。
 * @block_dim: 工作组 X/Y/Z 方向的线程数，均非零。
 * @code_size: 代码范围的字节数，队列模式用于取指边界检查。
 * @args_size: 参数块字节数，队列模式用于只读参数窗口检查。
 * @shared_mem_size: 每工作组共享内存字节数，本版只接受零。
 * @vmid: 本对象所属的设备地址空间编号，管理访问为零。
 * @source_slot: 本次任务所属的 HQD 槽号。
 */
typedef struct GPGPUKernelParams {
	uint64_t kernel_addr;
	uint64_t kernel_args;
	uint32_t grid_dim[3];
	uint32_t block_dim[3];
	uint32_t code_size;
	uint32_t args_size;
	uint32_t shared_mem_size;
	uint32_t vmid;	      /* 本次任务的用户地址空间编号。 */
	uint32_t source_slot; /* 本次任务所属 HQD 槽。 */
} GPGPUKernelParams;

/**
 * struct GPGPUDMAState - 保存 DMA 描述符及延迟完成通知状态
 * @src_addr: 源地址；H2D 为 Host DMA 地址，D2H 为 VRAM 偏移。
 * @dst_addr: 目的地址；H2D 为 VRAM 偏移，D2H 为 Host DMA 地址。
 * @size: 客体写入的传输字节数。
 * @ctrl: DMA_CTRL 标志，START 位自动清除。
 * @status: 只读：IDLE/BUSY/COMPLETE/ERROR。
 * @timer: DMA 模块的完成回调定时器。
 */
typedef struct GPGPUDMAState {
	/*
	 * TO_VRAM：src 为客体 DMA 地址，dst 为 VRAM 字节偏移；
	 * FROM_VRAM 的含义相反。两者均不是 QEMU 进程指针。
	 */
	uint64_t src_addr;
	uint64_t dst_addr;
	uint32_t size;
	GPGPUDMAControl ctrl;
	GPGPUDMAStatus status;
	QEMUTimer *timer;
} GPGPUDMAState;

/**
 * struct GPGPUSIMTContext - BAR0 教学寄存器读回值，与解释器的 lane 上下文分开
 * @thread_id: threadIdx.x/y/z 的读回值。
 * @block_id: blockIdx.x/y/z 的读回值。
 * @warp_id: warp 编号的读回值。
 * @lane_id: lane 编号的读回值。
 * @thread_mask: 第 N 位对应 lane N。
 */
typedef struct GPGPUSIMTContext {
	uint32_t thread_id[3];
	uint32_t block_id[3];
	uint32_t warp_id;
	uint32_t lane_id;
	uint32_t thread_mask;
} GPGPUSIMTContext;

/* machine 在目标 CPU 线程中同步完成复位和启动。 */
typedef void (*GPGPUCPSetState)(void *opaque, bool reset_hold, uint32_t reset_vector);

typedef struct GPGPUDispatchTask GPGPUDispatchTask;

/**
 * struct GPGPUIHState - 设备共用的 Host RAM 中断记录 Ring
 * @base: Ring 的完整 Host 物理基址，按 4 KiB 对齐。
 * @size: Ring 的字节容量，必须为 2 的幂。
 * @wptr: 设备下一条待写记录的 32 位编号。
 * @rptr: Host 下一条待消费记录的 32 位编号。
 * @ctrl: IH_ENABLE 使能位；RESET 是命令位，不锁存。
 * @status: 忙、首次故障以及最近一次提交结果的组合位域。
 */
typedef struct GPGPUIHState {
	uint64_t base;
	uint32_t size;
	uint32_t wptr;
	uint32_t rptr;
	uint32_t ctrl;
	uint32_t status;
} GPGPUIHState;

/* GPUVM 上下文由 Host 配置，所有访问在设备主锁下同步执行。 */
typedef struct GPGPUVMContext {
	uint64_t root;	       /* VRAM 中的根目录字节偏移。 */
	bool enabled;	       /* 发布页表和失效后才启用。 */
	unsigned int accesses; /* 正在进行的访问，含跨进程 DMA。 */
} GPGPUVMContext;

/* 直接映射的小型 TLB，权限与目标一起缓存。 */
typedef struct GPGPUVMTlb {
	uint64_t pte;  /* 叶子表项的完整快照。 */
	uint32_t page; /* GPU 页号。 */
	uint32_t vmid; /* 所属上下文。 */
} GPGPUVMTlb;

/* 首次故障读不清；槽位安全卸载后才能复用。 */
typedef struct GPGPUVMFault {
	uint64_t address; /* 原始 GPUVA，保留非法高位。 */
	uint32_t reason;  /* 非零故障原因。 */
	uint32_t vmid;	  /* 请求所属 VMID。 */
	uint32_t access;  /* READ、WRITE 或 EXEC。 */
	uint32_t phase;	  /* 取包、参数、取指或数据阶段。 */
} GPGPUVMFault;

/**
 * struct GPGPUState - PCI 教学加速器的寄存器、内存与执行状态
 * @parent_obj: QOM 的 PCI 父对象，必须位于结构体首部。
 * @ctrl_mmio: BAR0：gpgpu_regs.c 分发访问。
 * @vram: BAR2：vram_ptr 的 PCI 窗口。
 * @vram_ram: BAR2 和 CP 共用的 RAM。
 * @vram_as: 外部写入触发 TCG 失效。
 * @doorbell_mmio: BAR4 的 8 字节 Doorbell 通知窗口。
 * @num_cus: QOM 属性，通过 DEV_CAPS 返回。
 * @warps_per_cu: QOM 属性，通过 DEV_CAPS 返回。
 * @warp_size: QOM 属性；解释器使用 32 个 lane。
 * @vram_size: QOM 属性，单位为字节。
 * @vram_ptr: 后备存储的 QEMU 进程指针。
 * @cp_set_state: 由板级代码提供的原生 CP 复位与启动回调。
 * @cp_opaque: 传给 CP 状态回调的板级私有数据。
 * @cp_reset_hold: CP 当前是否保持复位。
 * @cp_started: 本次设备复位后 CP 是否已启动，限制 Host 重配 HQD。
 * @queue_mode: HQD 激活后置位，阻止 Host 使用同步派发入口。
 * @cp_reset_vector: CP 从复位状态启动时的 VRAM 入口偏移。
 * @global_ctrl: 保存 ENABLE 标志，不锁存 RESET。
 * @global_status: 只读的 GPGPUStatus 标志。
 * @error_status: GPGPUError 锁存标志，写 1 清除。
 * @irq_enable: 客体可读写的事件掩码。
 * @irq_status: 锁存的事件，写 IRQ_ACK 清除。
 * @hqd_select: Host 间接访问窗口当前选择的 HQD 槽号。
 * @hqd: 四个槽的寄存器存储，每槽按 32 位寄存器编号索引。
 * @doorbell_pending: 各槽收到通知的提示位，不替代共享 W/R。
 * @ih: Host 共用的 IH Ring 配置与进度。
 * @kernel: 可写的派发配置寄存器，提交时复制到任务中。
 * @task: 当前分段执行任务；发布终态前释放并置为 NULL。
 * @dispatch_timer: 让分段计算在 QEMU 主循环中继续推进的定时器。
 * @dispatch_status: 执行器 IDLE/BUSY/DONE/FAILED 状态。
 * @dispatch_error: 本次派发的细分错误码，确认结果时清除。
 * @dma: DMA 描述符、状态与完成定时器。
 * @simt: Host 可读写的 SIMT 教学寄存器。
 * @gpuvm: 本连接或设备是否采用 GPUVM 地址语义。
 * @vm_firmware: 固件启动时发布的 GPUVM 接口版本。
 * @vm_status: 最近一次同步失效的忙、ACK 或错误状态。
 * @vm: 三个编号对应的根、使能和在途访问记录。
 * @tlb: 按 VMID 和 GPU 页号区分的 64 项翻译缓存。
 * @vm_fault: 按 HQD 槽分别锁存的首次用户访问故障。
 */
struct GPGPUState {
	PCIDevice parent_obj;
	MemoryRegion ctrl_mmio;
	MemoryRegion vram;
	MemoryRegion vram_ram;
	AddressSpace vram_as;
	MemoryRegion doorbell_mmio;

	uint32_t num_cus;
	uint32_t warps_per_cu;
	uint32_t warp_size;
	uint64_t vram_size;
	uint8_t *vram_ptr;

	GPGPUCPSetState cp_set_state;
	void *cp_opaque;
	bool cp_reset_hold;
	bool cp_started;
	bool queue_mode;
	bool gpuvm;	      /* QOM 启动属性，设备寿命内不变。 */
	uint32_t vm_firmware; /* 固件版本握手。 */
	uint32_t vm_status;   /* 本次失效状态。 */
	GPGPUVMContext vm[3];
	GPGPUVMTlb tlb[64];
	GPGPUVMFault vm_fault[4];
	uint32_t cp_reset_vector;

	GPGPUControl global_ctrl;
	GPGPUStatus global_status;
	GPGPUError error_status;
	GPGPUIrq irq_enable;
	GPGPUIrq irq_status;

	uint32_t hqd_select;
	uint32_t hqd[4][0x100 / 4];
	bool doorbell_pending[4];
	GPGPUIHState ih;
	GPGPUKernelParams kernel;
	GPGPUDispatchTask *task;
	QEMUTimer *dispatch_timer;
	uint32_t dispatch_status;
	uint32_t dispatch_error;
	GPGPUDMAState dma;
	GPGPUSIMTContext simt;
};

/* 保存调用参数快照，翻译后访问完整标量或逐页搬运。 */
bool gpgpu_vm_access(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, void *data,
		     size_t length, uint32_t access, uint32_t phase);
/* 读取/更新 GPUVM Host 管理寄存器。 */
uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr addr);
void gpgpu_vm_write(GPGPUState *s, hwaddr addr, uint32_t value);
/* 为参数窗/控制页的非法访问记录源队列故障。 */
bool gpgpu_vm_fail(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, uint32_t access,
		   uint32_t phase, uint32_t reason);

/*
 * gpgpu_vram_contains() - 检查字节范围是否位于 VRAM 内
 *
 * 用减法比较剩余容量，避免 addr + size 的无符号溢出。
 */
static inline bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr, uint64_t size)
{
	return addr < s->vram_size && size <= s->vram_size - addr;
}

/*
 * gpgpu_vram_word_valid() - 检查一个 RV32 字访问是否合法
 *
 * 同时要求 4 字节对齐和完整落在 VRAM 内。
 */
static inline bool gpgpu_vram_word_valid(GPGPUState *s, uint64_t addr)
{
	return !(addr & 3) && gpgpu_vram_contains(s, addr, sizeof(uint32_t));
}

/* PCI BAR 访问回调与寄存器复位。 */
extern const MemoryRegionOps gpgpu_ctrl_ops;
extern const MemoryRegionOps gpgpu_doorbell_ops;
/* 复位设备寄存器与执行状态。 */
void gpgpu_reset_state(GPGPUState *s);

/* CP 连接和寄存器不依赖具体 CPU 架构。 */
/* 连接板级原生 CP 的控制入口。 */
void gpgpu_attach_cp(GPGPUState *s, GPGPUCPSetState set_state, void *opaque);
/* 使已连接的 CP 保持复位。 */
void gpgpu_cp_reset(GPGPUState *s);
/* 读回 CP 控制状态或复位入口。 */
uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr addr);
/* 处理 CP 复位与启动控制。 */
void gpgpu_cp_write(GPGPUState *s, hwaddr addr, uint32_t val);
/* 向 VRAM 写入一个小端标量。 */
void gpgpu_vram_store(GPGPUState *s, hwaddr addr, uint64_t value, unsigned int size);

/* 中断寄存器访问与 PCI 中断通知。 */
/* 读回中断使能与挂起状态。 */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr addr);
/* 更新中断使能或确认已处理事件。 */
void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val);
/* 锁存设备事件并通知 PCI 中断层。 */
void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events);
/* 保存全局错误并发出错误事件。 */
void gpgpu_set_error(GPGPUState *s, GPGPUError error);
/* 建立 MSI-X 和 MSI 能力。 */
bool gpgpu_irq_init(GPGPUState *s, Error **errp);
/* 撤销 PCI 中断能力。 */
void gpgpu_irq_cleanup(GPGPUState *s);
/* 复位 PCI 中断能力状态。 */
void gpgpu_irq_reset(GPGPUState *s);
/* 清除软件锁存的中断状态。 */
void gpgpu_irq_clear(GPGPUState *s);

/* DMA 模块管理描述符、数据传输和完成定时器。 */
/* 读回 DMA 描述符与状态。 */
uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr addr);
/* 处理 DMA 配置与启动写入。 */
void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val);
/* 创建 DMA 完成定时器。 */
void gpgpu_dma_init(GPGPUState *s);
/* 释放 DMA 定时器。 */
void gpgpu_dma_cleanup(GPGPUState *s);
/* 复位 DMA 寄存器与通知状态。 */
void gpgpu_dma_reset(GPGPUState *s);

/* 启动寄存器与 RV32 解释器的同步执行入口。 */
/* 读回派发配置和执行结果寄存器。 */
uint32_t gpgpu_kernel_read(GPGPUState *s, hwaddr addr);
/* 处理 Host 的同步派发寄存器写入。 */
void gpgpu_kernel_write(GPGPUState *s, hwaddr addr, uint32_t val);

/* 两个入口共用状态，由 MemoryRegion 确定访问来源。 */
extern const MemoryRegionOps gpgpu_cp_hqd_ops;
extern const MemoryRegionOps gpgpu_cp_regs_ops;
/* 读取指定 HQD 槽的寄存器。 */
uint32_t gpgpu_hqd_read(GPGPUState *s, unsigned int slot, hwaddr addr, unsigned int size);
/* 按访问来源更新 HQD 字段。 */
void gpgpu_hqd_write(GPGPUState *s, unsigned int slot, hwaddr addr, uint32_t val, unsigned int size,
		     bool from_cp);
/* 读回 IH 配置、进度与状态。 */
uint32_t gpgpu_ih_read(GPGPUState *s, hwaddr addr);
/* 区分 Host 配置与 CP 通知提交。 */
void gpgpu_ih_write(GPGPUState *s, hwaddr addr, uint32_t val, bool from_cp);
/* 为未消费的 IH 记录保持通知。 */
void gpgpu_ih_refresh_irq(GPGPUState *s);
/* 创建分段执行定时器。 */
void gpgpu_dispatch_init(GPGPUState *s);
/* 取消分段执行并清理任务。 */
void gpgpu_dispatch_reset(GPGPUState *s);
/* 释放执行器生命周期资源。 */
void gpgpu_dispatch_cleanup(GPGPUState *s);
/* 处理 CP 的配置、提交与结果确认。 */
void gpgpu_kernel_cp_write(GPGPUState *s, hwaddr addr, uint32_t val);
/* 读取 RV32 只读 Host 参数窗口。 */
bool gpgpu_param_read(GPGPUState *s, uint32_t addr, uint32_t *value);

#endif /* HW_GPGPU_H */
