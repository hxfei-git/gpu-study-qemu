/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BAR0 与 CP 本地寄存器窗口
 *
 * BAR0 是一个容器区域，每个寄存器组（0x100 字节）是一个子区域，
 * 由各自模块的读写函数处理。所有组共用同一条访问规则：只接受对齐的
 * 32 位访问，否则锁存 INVALID_CMD（IH 组另锁存 IH 协议故障），
 * 由 bar0.c 统一检查后再转发。
 *
 * CP 本地寄存器窗口（gpgpu_cp_map.h 中的 GPGPU_CP_REG_BASE）沿用 BAR0 偏移，
 * 但只开放 CP 需要的组：VM 只读信息与固件握手、Dispatch、IH 与 ERROR_STATUS。
 */
#ifndef HW_GPGPU_BAR0_H
#define HW_GPGPU_BAR0_H

#include "gpgpu.h"
#include "system/memory.h"

/* BAR0 子区域数：10 个寄存器组、HQD 窗口与背景区。 */
#define GPGPU_BAR0_REGIONS 12

typedef struct Bar0Group Bar0Group;

/**
 * struct GPGPUBar0Region - 一个寄存器组子区域
 * @mr: 子区域。
 * @s: 所属设备。
 * @group: 组描述，见 bar0.c。
 */
typedef struct GPGPUBar0Region {
	MemoryRegion mr;
	GPGPUState *s;
	const Bar0Group *group;
} GPGPUBar0Region;

/* 建立 BAR0 容器及全部寄存器组子区域，并注册 PCI BAR 0。 */
void gpgpu_bar0_init(GPGPUState *s);

/* 设备信息组（0x000）与全局控制组（0x100），实现位于 gpgpu.c。 */
uint32_t gpgpu_info_read(GPGPUState *s, hwaddr reg);
uint32_t gpgpu_global_read(GPGPUState *s, hwaddr reg);
void gpgpu_global_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 板级代码映射到 CP 地址空间的本地寄存器窗口，opaque 为 GPGPUState。 */
extern const MemoryRegionOps gpgpu_cp_regs_ops;

#endif /* HW_GPGPU_BAR0_H */
