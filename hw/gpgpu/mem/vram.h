/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VRAM：设备本地存储、BAR2 窗口与 CP 共享映射
 *
 * 同一块 RAM 有三条访问路径：Host 经 BAR2；原生 CP 经板级别名直接取指和
 * 访存；设备内部（GPU 物理访问、DMA、IH）经本模块的 AddressSpace。设备内部
 * 写入必须走 AddressSpace，原生 CP 已翻译的代码才会随之失效。
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
 * @ptr: 后备 RAM 的宿主机指针，只在 vram.c 内使用。
 */
typedef struct GPGPUVRAMState {
	MemoryRegion ram;
	MemoryRegion bar;
	AddressSpace as;
	uint8_t *ptr;
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

/**
 * gpgpu_vram_rw() - 经内部地址空间读写 VRAM
 * @s: 设备实例。
 * @addr: VRAM 字节偏移，调用者已检查范围。
 * @buf: 数据缓冲区。
 * @len: 字节数。
 * @is_write: 为 true 时写入 VRAM。
 *
 * Return: 访问结果。
 */
MemTxResult gpgpu_vram_rw(GPGPUState *s, uint64_t addr, void *buf, uint64_t len, bool is_write);

/**
 * gpgpu_vram_map() - 把一段 VRAM 映射为可直接读写的宿主机指针
 * @s: 设备实例。
 * @addr: VRAM 字节偏移，调用者已检查范围。
 * @len: 字节数。
 * @is_write: 映射期间是否写入。
 *
 * VRAM 是连续 RAM，已检查的范围总能完整映射。DMA 用它直接在 VRAM 与 Host
 * 之间搬运，失败时已写入的部分保留。
 *
 * Return: 映射起点，必须用 gpgpu_vram_unmap() 归还。
 */
void *gpgpu_vram_map(GPGPUState *s, uint64_t addr, uint64_t len, bool is_write);

/**
 * gpgpu_vram_unmap() - 归还 gpgpu_vram_map() 的映射
 * @s: 设备实例。
 * @ptr: 映射起点。
 * @len: 映射字节数。
 * @is_write: 与映射时相同；为 true 时整个范围按已写入处理，
 *            使 CP 代码缓存失效。
 */
void gpgpu_vram_unmap(GPGPUState *s, void *ptr, uint64_t len, bool is_write);

#endif /* HW_GPGPU_MEM_VRAM_H */
