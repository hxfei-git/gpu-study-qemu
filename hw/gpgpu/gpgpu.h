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

#define TYPE_GPGPU "gpgpu"
OBJECT_DECLARE_SIMPLE_TYPE(GPGPUState, GPGPU)

#define GPGPU_VENDOR_ID             0x1234
#define GPGPU_DEVICE_ID             0x1337
#define GPGPU_REVISION              0x01
#define GPGPU_CLASS_CODE            PCI_CLASS_DISPLAY_3D

#define GPGPU_CTRL_BAR_SIZE         (1 * 1024 * 1024)
#define GPGPU_VRAM_BAR_SIZE         (64 * 1024 * 1024)
#define GPGPU_DOORBELL_BAR_SIZE     (64 * 1024)
#define GPGPU_DEFAULT_NUM_CUS       4
#define GPGPU_DEFAULT_WARPS_PER_CU  4
#define GPGPU_DEFAULT_WARP_SIZE     32
#define GPGPU_DEFAULT_VRAM_SIZE     GPGPU_VRAM_BAR_SIZE

#define GPGPU_MSIX_VECTORS          4
#define GPGPU_MSIX_VEC_KERNEL       0
#define GPGPU_MSIX_VEC_DMA          1
#define GPGPU_MSIX_VEC_ERROR        2

/* 客体写入的启动描述符，由 gpgpu_dispatch_kernel() 读取。 */
typedef struct GPGPUKernelParams {
    uint64_t kernel_addr;     /* VRAM 字节偏移，4 字节对齐。 */
    uint64_t kernel_args;     /* 预留的参数块 VRAM 字节偏移。 */
    uint32_t grid_dim[3];     /* X/Y/Z 的 block 数，均非零。 */
    uint32_t block_dim[3];    /* block 的 X/Y/Z 线程数，均非零。 */
    uint32_t shared_mem_size; /* 预留的每个 block 共享内存字节数。 */
} GPGPUKernelParams;

typedef struct GPGPUDMAState {
    /*
     * TO_VRAM：src 为客体 DMA 地址，dst 为 VRAM 字节偏移；
     * FROM_VRAM 的含义相反。两者均不是 QEMU 进程指针。
     */
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;             /* 客体写入的传输字节数。 */
    GPGPUDMAControl ctrl;      /* 预留的 DMA_CTRL 标志。 */
    GPGPUDMAStatus status;     /* 只读：IDLE/BUSY/COMPLETE/ERROR。 */
    QEMUTimer *timer;          /* DMA 模块的完成回调定时器。 */
} GPGPUDMAState;

/*
 * BAR0 教学寄存器保存客体读写的值，与 gpgpu_core.h 中
 * 解释器的各 lane 上下文分开保存。此处尚未实现屏障。
 */
typedef struct GPGPUSIMTContext {
    uint32_t thread_id[3];     /* threadIdx.x/y/z 的读回值。 */
    uint32_t block_id[3];      /* blockIdx.x/y/z 的读回值。 */
    uint32_t warp_id;          /* warp 编号的读回值。 */
    uint32_t lane_id;          /* lane 编号的读回值。 */
    uint32_t thread_mask;      /* 第 N 位对应 lane N。 */
} GPGPUSIMTContext;

struct GPGPUState {
    PCIDevice parent_obj;
    MemoryRegion ctrl_mmio;    /* BAR0：gpgpu_regs.c 分发访问。 */
    MemoryRegion vram;         /* BAR2：vram_ptr 的 PCI 窗口。 */
    MemoryRegion doorbell_mmio; /* BAR4：gpgpu_doorbell.c 中预留。 */

    uint32_t num_cus;          /* QOM 属性，通过 DEV_CAPS 返回。 */
    uint32_t warps_per_cu;     /* QOM 属性，通过 DEV_CAPS 返回。 */
    uint32_t warp_size;        /* QOM 属性；解释器使用 32 个 lane。 */
    uint64_t vram_size;        /* QOM 属性，单位为字节。 */
    uint8_t *vram_ptr;         /* 后备存储的 QEMU 进程指针。 */

    GPGPUControl global_ctrl;  /* 保存 ENABLE 标志，不锁存 RESET。 */
    GPGPUStatus global_status; /* 只读的 GPGPUStatus 标志。 */
    GPGPUError error_status;   /* GPGPUError 锁存标志，写 1 清除。 */
    GPGPUIrq irq_enable;      /* 客体可读写的事件掩码。 */
    GPGPUIrq irq_status;      /* 锁存的事件，写 IRQ_ACK 清除。 */

    GPGPUKernelParams kernel;
    GPGPUDMAState dma;
    GPGPUSIMTContext simt;
};

/* 共用的边界检查：用减法避免 addr + size 溢出。 */
static inline bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr,
                                     uint64_t size)
{
    return addr < s->vram_size && size <= s->vram_size - addr;
}

static inline bool gpgpu_vram_word_valid(GPGPUState *s, uint64_t addr)
{
    return !(addr & 3) && gpgpu_vram_contains(s, addr, sizeof(uint32_t));
}

/* PCI BAR 访问回调与寄存器复位。 */
extern const MemoryRegionOps gpgpu_ctrl_ops;
extern const MemoryRegionOps gpgpu_doorbell_ops;
void gpgpu_reset_state(GPGPUState *s);

/* 中断寄存器访问与 PCI 中断通知。 */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr addr);
void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val);
void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events);
void gpgpu_set_error(GPGPUState *s, GPGPUError error);
bool gpgpu_irq_init(GPGPUState *s, Error **errp);
void gpgpu_irq_cleanup(GPGPUState *s);
void gpgpu_irq_clear(GPGPUState *s);

/* DMA 模块管理描述符、数据传输和完成定时器。 */
uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr addr);
void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val);
void gpgpu_dma_init(GPGPUState *s);
void gpgpu_dma_cleanup(GPGPUState *s);
void gpgpu_dma_reset(GPGPUState *s);

/* 启动寄存器与 RV32 解释器的同步执行入口。 */
uint32_t gpgpu_kernel_read(GPGPUState *s, hwaddr addr);
void gpgpu_kernel_write(GPGPUState *s, hwaddr addr, uint32_t val);

#endif /* HW_GPGPU_H */
