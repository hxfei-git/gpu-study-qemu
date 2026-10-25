/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 封装底层 Host DMA 传输，不持有描述符或完成事件。 */
#ifndef HW_MEMORY_GPGPU_HOST_H
#define HW_MEMORY_GPGPU_HOST_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/memattrs.h"

/**
 * gpgpu_host_read() - 读取 Host 存储
 * @s: 设备实例；调用期间须保持设备存活。
 * @address: 设备发出的 Host DMA 地址；系统 IOMMU 语义由现有传输层处理。
 * @data: 传输缓冲区。
 * @length: 传输字节数。
 *
 * 同步返回，不修改描述符 DMA 状态；失败时可能已有部分传输。
 *
 * Return: 底层完整成功时为 true。
 */
bool gpgpu_host_read(GPGPUState *s, uint64_t address, void *data, size_t length);

/**
 * gpgpu_host_access() - 保留底层访问结果的 Host 传输入口
 * @s: 设备实例；调用期间须保持设备存活。
 * @address: Host DMA 字节地址。
 * @data: 传输缓冲区。
 * @length: 传输字节数。
 * @is_write: 为 true 表示从缓冲区写入 Host。
 *
 * 供 CP 窗口保留原 MemTxResult 语义。
 *
 * Return: 底层访问结果，成功为 MEMTX_OK。
 */
MemTxResult gpgpu_host_access(GPGPUState *s, uint64_t address, void *data, size_t length,
			      bool is_write);

#endif /* HW_MEMORY_GPGPU_HOST_H */
