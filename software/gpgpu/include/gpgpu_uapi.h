/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_UAPI_H
#define GPGPU_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __u32 gpgpu_u32;
typedef __u64 gpgpu_u64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t gpgpu_u32;
typedef uint64_t gpgpu_u64;
#endif

#define GPGPU_ABI_VERSION 1
#define GPGPU_STAGING_SIZE (64U * 1024)
#define GPGPU_MAX_BLOCKS 4096U
#define GPGPU_MAX_BLOCK_THREADS 1024U
#define GPGPU_MAX_THREADS 65536U

/* 设备地址为 VRAM 字节偏移，不得传入主机指针。 */
struct gpgpu_info {
    gpgpu_u32 abi_version;
    gpgpu_u32 vram_size;
    gpgpu_u32 staging_size;
    gpgpu_u32 caps;
};

struct gpgpu_alloc {
    gpgpu_u32 size;
    gpgpu_u32 address;
};

#define GPGPU_COPY_H2D 0U
#define GPGPU_COPY_D2H 1U

/*
 * 单个 DMA 描述符：在已映射的暂存缓冲区
 * 与所属显存分配之间搬运。
 */
struct gpgpu_copy {
    gpgpu_u32 address;
    gpgpu_u32 size;
    gpgpu_u32 direction;
    gpgpu_u32 reserved;
};

struct gpgpu_launch {
    gpgpu_u32 code;
    gpgpu_u32 code_size;
    gpgpu_u32 args;
    gpgpu_u32 args_size;
    gpgpu_u32 grid[3];
    gpgpu_u32 block[3];
    gpgpu_u32 shared_size;
    gpgpu_u32 reserved;
};

struct gpgpu_stats {
    gpgpu_u64 kernel_irqs;
    gpgpu_u64 dma_irqs;
    gpgpu_u64 error_irqs;
};

#define GPGPU_IOC_INFO _IOR('G', 0, struct gpgpu_info)
#define GPGPU_IOC_ALLOC _IOWR('G', 1, struct gpgpu_alloc)
#define GPGPU_IOC_FREE _IOW('G', 2, gpgpu_u32)
#define GPGPU_IOC_COPY _IOW('G', 3, struct gpgpu_copy)
#define GPGPU_IOC_LAUNCH _IOW('G', 4, struct gpgpu_launch)
#define GPGPU_IOC_RESET _IO('G', 5)
#define GPGPU_IOC_STATS _IOR('G', 6, struct gpgpu_stats)

#endif /* GPGPU_UAPI_H */
