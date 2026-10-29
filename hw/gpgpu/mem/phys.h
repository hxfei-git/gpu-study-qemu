/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPU 物理地址空间：VRAM 与 Outbound 路由
 *
 * GPU 物理地址连续覆盖 32 位空间：
 *
 *   [0, vram_size)              VRAM
 *   [vram_size, 0x1_0000_0000)  Outbound：减去 vram_size 得到 Host IOVA，经 PCIe DMA 访问
 *
 * CP 管理域访问、页表遍历、IH 写记录和 DMA 的 Host 端都走 gpgpu_phys_rw()；
 * 用户地址先经 mem/gpuvm.c 翻译（或直通）再进入这里。
 */
#ifndef HW_GPGPU_MEM_PHYS_H
#define HW_GPGPU_MEM_PHYS_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "exec/memattrs.h"

/**
 * struct GPGPUPhysState - Outbound 窗口配置与在途访问
 * @gpu_base: 窗口在 GPU 物理空间的起点，固定为 vram_size。
 * @size: 窗口字节数，固定为 4 GiB - vram_size。
 * @host_base: 窗口起点对应的 Host IOVA，固定为 0。
 * @status: 最近一次配置写入的 ACK 或 ERROR。
 * @accesses: 在途物理访问数。跨进程 DMA 可能暂时释放主锁，计数非零时
 *            拒绝设备复位和 Outbound 重配。
 * @enabled: 窗口已使能。
 */
typedef struct GPGPUPhysState {
	uint64_t gpu_base;
	uint64_t size;
	uint64_t host_base;
	uint32_t status;
	unsigned int accesses;
	bool enabled;
} GPGPUPhysState;

/**
 * gpgpu_phys_contains() - 检查 GPU 物理范围是否可路由
 * @s: 设备实例。
 * @addr: GPU 物理地址。
 * @len: 字节数，非零。
 *
 * 只检查路由；Host 端是否真有映射要到访问时才知道。
 *
 * Return: 范围完整位于 VRAM 或已使能的 Outbound 窗口内时为 true。
 */
bool gpgpu_phys_contains(GPGPUState *s, uint64_t addr, uint64_t len);

/**
 * gpgpu_phys_rw() - 访问一段 GPU 物理地址
 * @s: 设备实例，调用者持有主锁。
 * @addr: GPU 物理地址。
 * @buf: 数据缓冲区。
 * @len: 字节数。
 * @is_write: 为 true 时写入。
 *
 * 范围不可路由或 Host 访问失败时返回错误；失败前已写入的部分保留。
 *
 * Return: 访问结果。
 */
MemTxResult gpgpu_phys_rw(GPGPUState *s, uint64_t addr, void *buf, size_t len, bool is_write);

/* 是否仍有在途物理访问；为 true 时不能复位或重配地址空间。 */
bool gpgpu_phys_busy(GPGPUState *s);

/* Outbound 寄存器组（BAR0 0xb00）。 */
uint32_t gpgpu_outbound_read(GPGPUState *s, hwaddr reg);
void gpgpu_outbound_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 复位：窗口未使能，布局为 [vram_size, 4 GiB) 到 IOVA 0。 */
void gpgpu_outbound_reset(GPGPUState *s);

#endif /* HW_GPGPU_MEM_PHYS_H */
