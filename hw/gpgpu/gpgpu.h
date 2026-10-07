/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU Educational GPGPU Device
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

/* Guest-written launch descriptor, consumed by gpgpu_dispatch_kernel(). */
typedef struct GPGPUKernelParams {
    uint64_t kernel_addr;     /* Code byte offset in VRAM; 4-byte aligned. */
    uint64_t kernel_args;     /* Args byte offset in VRAM; loaded into a0. */
    uint32_t grid_dim[3];     /* Number of blocks in X/Y/Z; each nonzero. */
    uint32_t block_dim[3];    /* Threads per block in X/Y/Z; each nonzero. */
    uint32_t shared_mem_size; /* Bytes per block; currently only zero works. */
} GPGPUKernelParams;

typedef struct GPGPUDMAState {
    /*
     * TO_VRAM: src = guest DMA address, dst = VRAM byte offset; vice versa
     * for FROM_VRAM. Neither value is a QEMU process pointer.
     */
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;             /* Guest-written transfer size in bytes. */
    GPGPUDMAControl ctrl;      /* DMA_CTRL flags, with START self-clearing. */
    GPGPUDMAStatus status;     /* Read-only IDLE/BUSY/COMPLETE/ERROR. */
    QEMUTimer *timer;          /* Completion callback; owned by DMA module. */
} GPGPUDMAState;

/*
 * BAR0 teaching registers: guest read/write storage, separate from the
 * interpreter's per-lane context in gpgpu_core.h. No barrier implementation.
 */
typedef struct GPGPUSIMTContext {
    uint32_t thread_id[3];     /* threadIdx.x/y/z readback. */
    uint32_t block_id[3];      /* blockIdx.x/y/z readback. */
    uint32_t warp_id;          /* Warp index readback. */
    uint32_t lane_id;          /* Lane index readback. */
    uint32_t thread_mask;      /* Bit N corresponds to lane N. */
} GPGPUSIMTContext;

struct GPGPUState {
    PCIDevice parent_obj;
    MemoryRegion ctrl_mmio;    /* BAR0: register routing in gpgpu_regs.c. */
    MemoryRegion vram;         /* BAR2: PCI aperture onto vram_ptr. */
    MemoryRegion doorbell_mmio; /* BAR4: reserved in gpgpu_doorbell.c. */

    uint32_t num_cus;          /* QOM property, reported in DEV_CAPS. */
    uint32_t warps_per_cu;     /* QOM property, reported in DEV_CAPS. */
    uint32_t warp_size;        /* QOM property; interpreter uses 32 lanes. */
    uint64_t vram_size;        /* QOM property, in bytes. */
    uint8_t *vram_ptr;         /* QEMU process pointer to backing storage. */

    GPGPUControl global_ctrl;  /* Stored ENABLE flag; RESET is not latched. */
    GPGPUStatus global_status; /* Read-only GPGPUStatus flags. */
    GPGPUError error_status;   /* Sticky GPGPUError flags; write 1 to clear. */
    GPGPUIrq irq_enable;      /* Guest read/write event mask. */
    GPGPUIrq irq_status;      /* Sticky events; write IRQ_ACK to clear. */

    GPGPUKernelParams kernel;
    GPGPUDMAState dma;
    GPGPUSIMTContext simt;
};

/* Shared bounds check: subtraction avoids overflow in addr + size. */
static inline bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr,
                                     uint64_t size)
{
    return addr < s->vram_size && size <= s->vram_size - addr;
}

static inline bool gpgpu_vram_word_valid(GPGPUState *s, uint64_t addr)
{
    return !(addr & 3) && gpgpu_vram_contains(s, addr, sizeof(uint32_t));
}

/* PCI BAR callbacks and register reset. */
extern const MemoryRegionOps gpgpu_ctrl_ops;
extern const MemoryRegionOps gpgpu_doorbell_ops;
void gpgpu_reset_state(GPGPUState *s);

/* IRQ register access and PCI notification. */
uint32_t gpgpu_irq_read(GPGPUState *s, hwaddr addr);
void gpgpu_irq_write(GPGPUState *s, hwaddr addr, uint32_t val);
void gpgpu_raise_irq(GPGPUState *s, GPGPUIrq events);
void gpgpu_set_error(GPGPUState *s, GPGPUError error);
bool gpgpu_irq_init(GPGPUState *s, Error **errp);
void gpgpu_irq_cleanup(GPGPUState *s);
void gpgpu_irq_reset(GPGPUState *s);
void gpgpu_irq_clear(GPGPUState *s);

/* DMA owns its descriptor, transfer and completion timer. */
uint32_t gpgpu_dma_read(GPGPUState *s, hwaddr addr);
void gpgpu_dma_write(GPGPUState *s, hwaddr addr, uint32_t val);
void gpgpu_dma_init(GPGPUState *s);
void gpgpu_dma_cleanup(GPGPUState *s);
void gpgpu_dma_reset(GPGPUState *s);

/* Launch registers and synchronous entry into the RV32 interpreter. */
uint32_t gpgpu_kernel_read(GPGPUState *s, hwaddr addr);
void gpgpu_kernel_write(GPGPUState *s, hwaddr addr, uint32_t val);

#endif /* HW_GPGPU_H */
