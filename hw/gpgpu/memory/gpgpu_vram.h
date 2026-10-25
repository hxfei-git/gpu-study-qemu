/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VRAM 生命周期、BAR2 和跨模块访问；存储目录内保留不同写入路径。 */
#ifndef HW_MEMORY_GPGPU_VRAM_H
#define HW_MEMORY_GPGPU_VRAM_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"
#include "system/address-spaces.h"

/**
 * gpgpu_vram_init() - 创建 CP 和 BAR2 共用的 VRAM 存储
 * @s: 设备实例；调用期间须保持设备存活。
 * @errp: 创建失败的原因。
 *
 * 仅分配后备 RAM；BAR2 在设备组装的原顺序注册。
 *
 * Return: 成功为 true。
 */
bool gpgpu_vram_init(GPGPUState *s, Error **errp);

/**
 * gpgpu_vram_register_bar() - 建立 BAR2 窗口并注册 PCI BAR
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_vram_register_bar(GPGPUState *s);

/**
 * gpgpu_vram_cleanup() - 销毁 VRAM 写入地址空间
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * CP、DMA 和计算已停止后调用。
 */
void gpgpu_vram_cleanup(GPGPUState *s);

/**
 * gpgpu_vram_reset() - 清空 VRAM 并使原生代码翻译失效
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 仅用于 QOM 设备复位，普通寄存器复位保留存储。
 */
void gpgpu_vram_reset(GPGPUState *s);

/**
 * gpgpu_vram_region() - 借用原生 CP 可直接映射的共享 RAM
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 调用方只建立别名，存储仍归设备所有。
 *
 * Return: 设备存活期间有效的 RAM 区域。
 */
MemoryRegion *gpgpu_vram_region(GPGPUState *s);

/**
 * gpgpu_vram_size() - 查询 VRAM 字节容量
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: 设备属性配置的容量。
 */
uint64_t gpgpu_vram_size(GPGPUState *s);

/**
 * gpgpu_vram_contains() - 检查字节范围是否完整位于 VRAM
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: VRAM 字节偏移。
 * @size: 请求字节数。
 *
 * 以剩余容量比较，避免地址相加溢出。
 *
 * Return: 起点在存储内且范围完整时为 true。
 */
bool gpgpu_vram_contains(GPGPUState *s, uint64_t addr, uint64_t size);

/**
 * gpgpu_vram_word_valid() - 检查 RV32 字的对齐与范围
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: VRAM 字节偏移。
 *
 * Return: 4 字节对齐且完整位于 VRAM 时为 true。
 */
bool gpgpu_vram_word_valid(GPGPUState *s, uint64_t addr);

/**
 * gpgpu_vram_read_word() - 读取一个小端 RV32 字
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: VRAM 字节偏移。
 * @value: 成功时写入结果。
 *
 * 失败不锁存全局错误，由调用者按访问路径处理。
 *
 * Return: 访问合法为 true，否则为 false。
 */
bool gpgpu_vram_read_word(GPGPUState *s, uint64_t addr, uint32_t *value);

/**
 * gpgpu_vram_read_bytes() - 复制已验证范围内的 VRAM 字节
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 调用者已验证的 VRAM 字节偏移。
 * @data: 目标缓冲区。
 * @length: 复制字节数。
 *
 * 保留原始字节顺序，不做端序转换。
 */
void gpgpu_vram_read_bytes(GPGPUState *s, uint64_t addr, void *data, size_t length);

/**
 * gpgpu_vram_store() - 写入小端标量并处理原生代码失效
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: VRAM 字节偏移。
 * @value: 要写入的标量。
 * @size: 标量字节数，最大为 8。
 *
 * 沿原 AddressSpace 路径写入；越界锁存 VRAM 错误。
 */
void gpgpu_vram_store(GPGPUState *s, hwaddr addr, uint64_t value, unsigned int size);

/**
 * struct GPGPUVRAMState - 由 VRAM 模块维护的共享后备存储
 * @bar: BAR2 的访问窗口。
 * @ram: CP 与设备共用的 RAM 区域。
 * @as: 标量和 DMA 写入使用的地址空间，负责代码失效。
 * @ptr: 连续 RAM 的宿主机指针，仅存储目录内部使用。
 */
typedef struct GPGPUVRAMState {
	MemoryRegion bar;
	MemoryRegion ram;
	AddressSpace as;
	uint8_t *ptr;
} GPGPUVRAMState;

#endif /* HW_MEMORY_GPGPU_VRAM_H */
