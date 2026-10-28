/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GPU 物理地址经 VRAM 或 Outbound 路由，Host DMA 地址只留在底层。 */
#ifndef HW_MEMORY_GPGPU_HOST_H
#define HW_MEMORY_GPGPU_HOST_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/memattrs.h"
#include "exec/hwaddr.h"

typedef struct GPGPUPhysicalState {
	uint64_t gpu_base;
	uint64_t size;
	uint64_t host_base;
	uint32_t status;
	unsigned int accesses;
	bool enabled;
} GPGPUPhysicalState;

/* 检查完整范围是否可由当前 VRAM/Outbound 配置路由。 */
bool gpgpu_phys_contains(GPGPUState *s, uint64_t address, uint64_t length);
/* 公共物理入口；调用者持设备主锁，DMA 期间在途计数阻止重配。 */
MemTxResult gpgpu_phys_access(GPGPUState *s, uint64_t address, void *data, size_t length,
			      bool is_write);
/* Outbound 配置为全设备共享，只有停止所有访问后才允许写入。 */
uint32_t gpgpu_outbound_read(GPGPUState *s, hwaddr addr);
void gpgpu_outbound_write(GPGPUState *s, hwaddr addr, uint32_t value);
void gpgpu_outbound_reset(GPGPUState *s);

#endif /* HW_MEMORY_GPGPU_HOST_H */
