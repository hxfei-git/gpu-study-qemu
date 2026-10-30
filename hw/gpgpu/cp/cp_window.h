/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 访存窗口：原生 CP 访问 GPU 地址的唯一通道
 *
 * RV64 CP 只有 32 位本地地址空间，不能直接发出 GPU 物理或用户地址。
 * 固件先写配置寄存器（目标基址、访问域、VMID、故障归属槽与阶段），
 * 再读写 64 KiB 窗口：
 *
 *   CP 访问 0x8000_0000 + off
 *     → 目标地址 = 窗口基址 + off
 *     → 管理域：gpgpu_phys_rw()（GPU 物理地址）
 *       用户域：gpgpu_user_rw()（按 gpuvm 翻译或直通，故障记到配置的槽）
 *
 * 访问失败返回 MEMTX_ERROR，CP 上表现为 access fault，由固件的陷阱处理恢复。
 */
#ifndef HW_GPGPU_CP_WINDOW_H
#define HW_GPGPU_CP_WINDOW_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/**
 * struct GPGPUCPWindowState - 窗口配置
 * @base: 目标基址，低 16 位为 0。
 * @domain: GPGPU_CP_DOMAIN_MANAGEMENT 或 GPGPU_CP_DOMAIN_USER；其他值使访问失败。
 * @vmid: 用户域访问的 VMID。
 * @slot: 用户域访问失败时记录故障的 HQD 槽。
 * @phase: 记录到故障中的阶段。
 */
typedef struct GPGPUCPWindowState {
	uint64_t base;
	uint32_t domain;
	uint32_t vmid;
	uint32_t slot;
	uint32_t phase;
} GPGPUCPWindowState;

/* 板级代码映射到 CP 地址空间的窗口与配置寄存器，opaque 为 GPGPUState。 */
extern const MemoryRegionOps gpgpu_cp_window_ops;
extern const MemoryRegionOps gpgpu_cp_window_regs_ops;

/* CP 复位时清零窗口配置。 */
void gpgpu_cp_window_reset(GPGPUState *s);

#endif /* HW_GPGPU_CP_WINDOW_H */
