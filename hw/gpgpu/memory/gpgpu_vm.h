/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 管理 GPUVM 上下文、TLB 和首次访问故障；共享编码保持在原协议头。 */
#ifndef HW_MEMORY_GPGPU_VM_H
#define HW_MEMORY_GPGPU_VM_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "software/gpgpu/include/gpgpu_vm.h"

/**
 * struct GPGPUVMContext - GPUVM 管理的一份地址空间上下文
 * @root: 根目录的 GPU 物理地址，可位于 VRAM 或 Outbound。
 * @enabled: 页表发布和失效完成后由 Host 启用。
 * @accesses: 在途访问次数，包含可能让出设备主锁的跨进程 DMA。
 */
typedef struct GPGPUVMContext {
	uint64_t root;
	bool enabled;
	unsigned int accesses;
} GPGPUVMContext;

/**
 * struct GPGPUVMTlb - GPUVM 直接映射缓存中的一项翻译
 * @pte: 叶子表项快照，含目标存储与权限。
 * @page: GPU 虚拟页号。
 * @vmid: 翻译所属的用户地址空间。
 */
typedef struct GPGPUVMTlb {
	uint64_t pte;
	uint32_t page;
	uint32_t vmid;
} GPGPUVMTlb;

/**
 * struct GPGPUVMFault - 按 HQD 槽保存的首次访问故障
 * @address: 失败的原始 GPUVA，保留非法高位。
 * @reason: 非零故障原因；零表示未锁存故障。
 * @vmid: 发起请求的地址空间编号。
 * @access: 请求的 READ、WRITE 或 EXEC 权限。
 * @phase: 取包、参数、取指或数据访问阶段。
 *
 * 读取不清错；源槽停用后，Host 才能通过清错寄存器回收记录。
 */
typedef struct GPGPUVMFault {
	uint64_t address;
	uint32_t reason;
	uint32_t vmid;
	uint32_t access;
	uint32_t phase;
} GPGPUVMFault;

/**
 * gpgpu_vm_access() - 翻译并访问用户存储
 * @s: 设备实例；调用期间须保持设备存活。
 * @vmid: 用户地址空间编号。
 * @slot: 故障归属的 HQD 槽号。
 * @address: 原始 GPUVA，保留非法高位用于报错。
 * @data: 传输缓冲区。
 * @length: 访问字节数。
 * @access: READ、WRITE 或 EXEC。
 * @phase: 固件或执行器的访问阶段。
 *
 * 调用者持有设备主锁。标量完整访问，块访问逐页推进；失败保留已完成写入。
 *
 * Return: 成功为 true；失败记录源槽首次故障并返回 false。
 */
bool gpgpu_vm_access(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, void *data,
		     size_t length, uint32_t access, uint32_t phase);

/* 用户统一入口：按设备属性翻译或直通，并保持源槽故障归属。 */
bool gpgpu_user_access(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, void *data,
		       size_t length, uint32_t access, uint32_t phase);

/**
 * gpgpu_vm_fail() - 为非法窗口访问记录首次 GPUVM 故障
 * @s: 设备实例；调用期间须保持设备存活。
 * @vmid: 请求的用户地址空间。
 * @slot: 源 HQD 槽号。
 * @address: 失败的原始 GPUVA。
 * @access: 访问权限类型。
 * @phase: 访问阶段。
 * @reason: 非零故障原因。
 *
 * 已有故障保持原值，普通用户错误不升级为全局错误。
 *
 * Return: 始终为 false，便于调用方直接返回失败。
 */
bool gpgpu_vm_fail(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t address, uint32_t access,
		   uint32_t phase, uint32_t reason);

/**
 * gpgpu_vm_read() - 读取 GPUVM 能力、上下文或故障
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 *
 * Return: 寄存器值，读取不清除故障。
 */
uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr addr);

/**
 * gpgpu_vm_write() - 配置上下文或同步失效
 * @s: 设备实例；调用期间须保持设备存活。
 * @addr: 寄存器的字节偏移。
 * @value: 寄存器写入值。
 *
 * 存在活跃队列或在途访问时拒绝更改，保留原失效状态规则。
 */
void gpgpu_vm_write(GPGPUState *s, hwaddr addr, uint32_t value);

/**
 * gpgpu_vm_enabled() - 查询用户上下文是否启用
 * @s: 设备实例；调用期间须保持设备存活。
 * @vmid: 用户 VMID。
 *
 * Return: 编号在用户范围且上下文启用时为 true。
 */
bool gpgpu_vm_enabled(GPGPUState *s, unsigned int vmid);

/**
 * gpgpu_vm_reset() - 清零上下文、TLB、故障与固件握手
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 整体寄存器复位停止设备访问后调用。
 */
void gpgpu_vm_reset(GPGPUState *s);

/**
 * gpgpu_vm_reset_firmware() - 清除 CP 固件的版本握手
 * @s: 设备实例；调用期间须保持设备存活。
 *
 * 由原生 CPU 复位位置调用，保留用户页表和队列。
 */
void gpgpu_vm_reset_firmware(GPGPUState *s);

/**
 * gpgpu_vm_set_firmware() - 接收固件启动时发布的接口版本
 * @s: 设备实例；调用期间须保持设备存活。
 * @version: 固件写入的版本号。
 *
 * Return: 统一访存协议版本匹配时保存并返回 true。
 */
bool gpgpu_vm_set_firmware(GPGPUState *s, uint32_t version);

/**
 * struct GPGPUVMState - GPUVM 模块维护的翻译与故障状态
 * @firmware: 本次 CP 启动发布的接口版本。
 * @status: 最近一次同步失效的忙、确认或错误状态。
 * @contexts: 编号零保留，编号 1、2 保存用户上下文。
 * @tlb: 按 VMID 与 GPU 页号区分的缓存。
 * @fault: 每个 HQD 槽的首次访问故障。
 */
typedef struct GPGPUVMState {
	uint32_t firmware;
	uint32_t status;
	GPGPUVMContext contexts[3];
	GPGPUVMTlb tlb[64];
	GPGPUVMFault fault[4];
} GPGPUVMState;

#endif /* HW_MEMORY_GPGPU_VM_H */
