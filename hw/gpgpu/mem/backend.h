/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 后备存储：mem/ 目录私有的原始访问接口
 *
 * 只有访存入口 access.c 和 dcache.c 的回填、回写使用这些函数。它们直接
 * 读写 VRAM，或经 Outbound 访问 Host 内存，既不查 dcache，也不处理缓存
 * 副本。mem/ 以外的模块一律调用 gpgpu_mem_access()，否则会绕过属性判断
 * 和旁路访问前的缓存处理：
 *
 *   gpgpu_mem_access()
 *     ├─ WB 读写 ─────▶ dcache.c ─回填、回写─┐
 *     └─ DEVICE、FETCH、COPY ────────────────┤
 *                                            ▼
 *                          gpgpu_phys_rw() ─┬─ VRAM ─▶ gpgpu_vram_rw()
 *                                           └─ Outbound ─▶ vfio-user DMA
 *
 * COPY 另用 gpgpu_vram_map() 把 VRAM 端借成宿主机指针，与 Host 端整段互拷。
 */
#ifndef HW_GPGPU_MEM_BACKEND_H
#define HW_GPGPU_MEM_BACKEND_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/memattrs.h"

/*
 * 一次物理事务的开始与结束，可以嵌套。计数非零期间 gpgpu_phys_busy()
 * 为真，设备复位、Outbound 重配和维护命令都被拒绝。
 */
void gpgpu_phys_begin(GPGPUState *s);
void gpgpu_phys_end(GPGPUState *s);

/**
 * gpgpu_phys_rw() - 访问一段 GPU 物理地址的后备存储
 * @s: 设备实例，调用者持有主锁，并已用 gpgpu_phys_begin() 计入事务。
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

/**
 * gpgpu_vram_rw() - 经内部地址空间读写 VRAM
 * @s: 设备实例。
 * @addr: VRAM 字节偏移，调用者已检查范围。
 * @buf: 数据缓冲区。
 * @len: 字节数。
 * @is_write: 为 true 时写入 VRAM。
 *
 * 写入经内部地址空间完成，原生 CP 已翻译的代码随之失效。
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
 * VRAM 是连续 RAM，已检查的范围总能完整映射。
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

#endif /* HW_GPGPU_MEM_BACKEND_H */
