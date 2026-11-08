/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VRAM：设备本地存储、BAR2 窗口与 CP 共享映射
 *
 * 同一块 RAM 有三条访问路径：Host 经 BAR2；原生 CP 经板级别名直接取指和
 * 访存；设备内部（GPU 物理访问、DMA、IH）经本模块的 AddressSpace。设备内部
 * 写入必须走 AddressSpace，原生 CP 已翻译的代码才会随之失效。
 *
 * 本模块是后备存储，不经过 dcache。BAR2 把每次访问交给访存入口，按 DEVICE
 * 处理重叠的缓存行；直接读写后备 RAM 的函数只对 mem/ 可见，声明在
 * mem/backend.h。
 */
#ifndef HW_GPGPU_MEM_VRAM_H
#define HW_GPGPU_MEM_VRAM_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/**
 * struct GPGPUVRAMState - VRAM 模块状态
 * @ram: 后备 RAM，也供板级代码建立 CP 别名。
 * @bar: BAR2 的 MMIO 窗口，带越界检查。
 * @as: 设备内部访问使用的地址空间。
 */
typedef struct GPGPUVRAMState {
	MemoryRegion ram;
	MemoryRegion bar;
	AddressSpace as;
} GPGPUVRAMState;

/**
 * gpgpu_vram_init() - 分配后备 RAM 并注册 BAR2
 * @s: 设备实例。
 * @errp: 失败原因。
 *
 * Return: 成功为 true。
 */
bool gpgpu_vram_init(GPGPUState *s, Error **errp);

/* 销毁内部地址空间；调用前 CP、DMA 与执行器已停止。 */
void gpgpu_vram_cleanup(GPGPUState *s);

/* 经内部地址空间清零全部 VRAM，仅用于 QOM 设备复位。 */
void gpgpu_vram_clear(GPGPUState *s);

/* 借出后备 RAM，板级代码用它建立 CP 别名；存储仍归设备所有。 */
MemoryRegion *gpgpu_vram_region(GPGPUState *s);

/* VRAM 字节数，等于 vram_size 属性。 */
uint64_t gpgpu_vram_size(GPGPUState *s);

/**
 * gpgpu_vram_contains() - 检查 [addr, addr + size) 是否完整位于 VRAM
 * @s: 设备实例。
 * @addr: VRAM 字节偏移。
 * @size: 字节数。
 *
 * Return: 范围完整位于 VRAM 内时为 true；size 为 0 时只检查起点。
 */
bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr, uint64_t size);

#endif /* HW_GPGPU_MEM_VRAM_H */
