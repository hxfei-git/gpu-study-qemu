/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HQD 拥有槽寄存器、选槽及通知提示；跨模块只通过查询和操作访问。 */
#ifndef HW_CP_GPGPU_HQD_H
#define HW_CP_GPGPU_HQD_H

#include "hw/gpgpu/gpgpu.h"
#include "system/memory.h"

/* CP 直接窗口沿用槽号步长，opaque 为设备实例。 */
extern const MemoryRegionOps gpgpu_cp_hqd_ops;

/**
 * gpgpu_hqd_read() - 读取指定 HQD 字段
 * @s: 设备实例；调用期间须保持设备存活。
 * @slot: HQD 槽号。
 * @addr: HQD 槽内字节偏移，不含 Host 窗口基址或 CP 槽步长。
 * @size: 访问字节数，必须为 4。
 *
 * Return: 字段值；非法访问锁存全局错误并返回零。
 */
uint32_t gpgpu_hqd_read(GPGPUState *s, unsigned int slot, hwaddr addr, unsigned int size);

/**
 * gpgpu_hqd_write() - 按来源检查权限并更新 HQD
 * @s: 设备实例；调用期间须保持设备存活。
 * @slot: HQD 槽号。
 * @addr: HQD 槽内字节偏移，不含 Host 窗口基址或 CP 槽步长。
 * @val: 本次写入的寄存器值。
 * @size: 访问字节数，必须为 4。
 * @from_cp: 为 true 表示 CP 访问，否则为 Host 访问。
 *
 * 激活前检查完整配置；非法写入保留原字段。
 */
void gpgpu_hqd_write(GPGPUState *s, unsigned int slot, hwaddr addr, uint32_t val, unsigned int size,
		     bool from_cp);

/**
 * gpgpu_hqd_selected() - 取得 Host 当前选槽
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: 当前合法槽号。
 */
unsigned int gpgpu_hqd_selected(GPGPUState *s);

/**
 * gpgpu_hqd_select() - 更新 Host 间接窗口的选槽
 * @s: 设备实例；调用期间须保持设备存活。
 * @slot: 请求的槽号；非法值保留原选择并报错。
 */
void gpgpu_hqd_select(GPGPUState *s, uint32_t slot);

/**
 * gpgpu_hqd_active() - 查询槽是否已激活
 * @s: 设备实例；调用期间须保持设备存活。
 * @slot: 待查询槽号。
 *
 * Return: 槽号合法且 ACTIVE 非零时为 true。
 */
bool gpgpu_hqd_active(GPGPUState *s, unsigned int slot);

/**
 * gpgpu_hqd_binding_valid() - 检查用户派发的槽与 VMID 绑定
 * @s: 设备实例；调用期间须保持设备存活。
 * @slot: 用户槽号，须为 2 或 3。
 * @vmid: 派发请求的用户 VMID。
 *
 * Return: 用户槽已激活且 VMID 匹配时为 true。
 */
bool gpgpu_hqd_binding_valid(GPGPUState *s, unsigned int slot, unsigned int vmid);

/**
 * gpgpu_hqd_vmid_active() - 查询 VMID 是否仍有活跃用户队列
 * @s: 设备实例；调用期间须保持设备存活。
 * @vmid: 要检查的地址空间编号。
 *
 * Return: 存在匹配的活跃用户槽时为 true。
 */
bool gpgpu_hqd_vmid_active(GPGPUState *s, unsigned int vmid);

/**
 * gpgpu_hqd_queue_mode() - 查询本轮是否已进入队列模式
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * Return: 队列模式锁存值，整体寄存器复位时清除。
 */
bool gpgpu_hqd_queue_mode(GPGPUState *s);

/**
 * gpgpu_hqd_enter_queue_mode() - 锁存队列模式以阻止 Host 同步派发
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_hqd_enter_queue_mode(GPGPUState *s);

/**
 * gpgpu_hqd_notify() - 为匹配的已使能队列记录门铃提示
 * @s: 设备实例；调用期间须保持设备存活。
 * @offset: BAR4 内已验证对齐的字节偏移。
 */
void gpgpu_hqd_notify(GPGPUState *s, hwaddr offset);

/**
 * gpgpu_hqd_reset() - 清零队列寄存器、选槽和通知提示
 * @s: 设备实例；调用期间须保持设备存活。
 */
void gpgpu_hqd_reset(GPGPUState *s);

/**
 * struct GPGPUHQDState - 由 HQD 模块维护的槽与队列模式
 * @select: Host 间接窗口选择的槽号。
 * @slots: 四个槽的 32 位寄存器，每槽按字节偏移除以 4 索引。
 * @doorbell_pending: 各槽的通知提示，不替代共享读写进度。
 * @queue_mode: 激活队列或接受 CP 派发后锁存，设备复位时清除。
 */
typedef struct GPGPUHQDState {
	uint32_t select;
	uint32_t slots[4][0x100 / 4];
	bool doorbell_pending[4];
	bool queue_mode;
} GPGPUHQDState;

#endif /* HW_CP_GPGPU_HQD_H */
