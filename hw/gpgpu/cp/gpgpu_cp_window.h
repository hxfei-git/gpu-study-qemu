/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CP 的架构无关访存窗口；访问配置在一次请求中保持快照。 */
#ifndef HW_CP_GPGPU_CP_WINDOW_H
#define HW_CP_GPGPU_CP_WINDOW_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/* 由板级 MemoryRegion 使用；opaque 为 GPGPUState，映射仍属于板级对象。 */
extern const MemoryRegionOps gpgpu_cp_host_ops;
extern const MemoryRegionOps gpgpu_cp_host_regs_ops;

/**
 * gpgpu_cp_window_reset() - 清零窗口基址和访存归属
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 原生 CPU 保持复位时调用；不改变窗口映射。
 */
void gpgpu_cp_window_reset(GPGPUState *s);

/**
 * struct GPGPUCPWindowState - 由窗口模块保存的地址与故障归属
 * @host_iova_base: GPU 物理地址或 GPUVA 窗口基址，低 16 位为零。
 * @vm_domain: 管理物理域或用户访问域。
 * @vm_id: 用户地址空间编号。
 * @vm_slot: 故障归属的 HQD 槽号。
 * @vm_phase: 访问所处的固件执行阶段。
 */
typedef struct GPGPUCPWindowState {
	uint64_t host_iova_base;
	uint32_t vm_domain;
	uint32_t vm_id;
	uint32_t vm_slot;
	uint32_t vm_phase;
} GPGPUCPWindowState;

#endif /* HW_CP_GPGPU_CP_WINDOW_H */
