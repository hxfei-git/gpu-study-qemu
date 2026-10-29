/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HQD：4 个硬件队列槽的寄存器
 *
 * 槽 0 为 KIQ，槽 1 为 HIQ，槽 2、3 为用户 AQL 队列。两条访问路径共用同一份
 * 状态和检查规则：
 *
 *   Host：BAR0 0x600 选槽，再经 0x700 窗口读写；只在 CP 冷启动前配置 KIQ。
 *   CP  ：本地 0xffff_0000 + 槽号 × 0x100 + 字段偏移，直接读写任意槽。
 *
 * 激活（ACTIVE 由 0 写为 1）前检查整套配置；激活期间只允许改 ACTIVE、RPTR、
 * ERROR 和 Doorbell 使能位。
 */
#ifndef HW_GPGPU_CP_HQD_H
#define HW_GPGPU_CP_HQD_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/* 每槽寄存器空间的 32 位字数。 */
#define GPGPU_HQD_WORDS (GPGPU_HQD_STRIDE / 4)

/**
 * struct GPGPUHQDState - HQD 状态
 * @select: Host 选槽窗口当前选中的槽号。
 * @regs: 每槽寄存器，按字段字节偏移除以 4 索引。
 */
typedef struct GPGPUHQDState {
	uint32_t select;
	uint32_t regs[GPGPU_HQD_SLOTS][GPGPU_HQD_WORDS];
} GPGPUHQDState;

/* 板级代码映射到 CP 地址空间的 HQD 直接窗口，opaque 为 GPGPUState。 */
extern const MemoryRegionOps gpgpu_cp_hqd_ops;

/* Host 选槽寄存器组（BAR0 0x600）：只有 HQD_SELECT 有效。 */
uint32_t gpgpu_hqd_select_read(GPGPUState *s, hwaddr reg);
void gpgpu_hqd_select_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* Host 选槽窗口（BAR0 0x700）：field 为槽内字段偏移。 */
uint32_t gpgpu_hqd_window_read(GPGPUState *s, hwaddr field);
void gpgpu_hqd_window_write(GPGPUState *s, hwaddr field, uint32_t value);

/* 槽号合法且 ACTIVE 为 1。 */
bool gpgpu_hqd_active(GPGPUState *s, unsigned int slot);

/* 是否存在任何激活的槽。 */
bool gpgpu_hqd_any_active(GPGPUState *s);

/* slot 是已激活的用户槽，且绑定的 VMID 等于 vmid。 */
bool gpgpu_hqd_binding_valid(GPGPUState *s, unsigned int slot, unsigned int vmid);

/* 是否有已激活的用户槽绑定到 vmid。 */
bool gpgpu_hqd_vmid_active(GPGPUState *s, unsigned int vmid);

/* 设备复位：清零全部槽和选槽；CP 单独复位不调用。 */
void gpgpu_hqd_reset(GPGPUState *s);

#endif /* HW_GPGPU_CP_HQD_H */
