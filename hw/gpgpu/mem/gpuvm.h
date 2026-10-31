/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPUVM：用户地址的翻译与故障记录
 *
 * CP 用户域访问和 RV32 执行器的取指、数据访问都调用 gpgpu_user_rw()：
 *
 *   gpuvm=on   GPUVA ─TLB/三级页表─▶ GPU 物理地址 ─▶ gpgpu_phys_rw()
 *   gpuvm=off  用户地址就是 GPU 物理地址，只检查保护区 ─▶ gpgpu_phys_rw()
 *
 * 两种模式下用户都不能访问 VRAM 首页和固件保留区。访问失败时按来源
 * HQD 槽记录首次故障，供 Host 经 BAR0 0xa00 读取。
 */
#ifndef HW_GPGPU_MEM_GPUVM_H
#define HW_GPGPU_MEM_GPUVM_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"

/**
 * struct GPGPUVMContext - 一个用户 VMID 的地址空间
 * @root: 页表根的 GPU 物理地址，页对齐。
 * @enabled: Host 发布页表并完成失效后置位；置位期间根只读。
 * @accesses: 在途翻译访问数；非零时拒绝失效和上下文修改。
 */
typedef struct GPGPUVMContext {
	uint64_t root;
	bool enabled;
	unsigned int accesses;
} GPGPUVMContext;

/**
 * struct GPGPUVMTlbEntry - TLB 中的一项叶子翻译
 * @phys: 目标页的 GPU 物理基址，按 4 KiB 对齐；SYSTEM 已换算完毕。
 * @vpn: GPU 虚拟页号，即 GPUVA / GPGPU_VM_PAGE_SIZE。
 * @vmid: 所属 VMID。
 * @perms: 叶子项的 R/W/X 位，取值为 GPGPU_VM_PERMS 的子集。
 * @valid: 本项有效。
 */
typedef struct GPGPUVMTlbEntry {
	uint64_t phys;
	uint32_t vpn;
	uint32_t vmid;
	uint32_t perms;
	bool valid;
} GPGPUVMTlbEntry;

/**
 * struct GPGPUVMTlbSet - 组相联 TLB 的一组
 * @ways: 本组的各路。
 * @order: 路号从最近使用（MRU）到最久未用（LRU）排列，复位为 {0, 1, 2, 3}。
 */
typedef struct GPGPUVMTlbSet {
	GPGPUVMTlbEntry ways[GPGPU_VM_TLB_WAYS];
	uint8_t order[GPGPU_VM_TLB_WAYS];
} GPGPUVMTlbSet;

/**
 * struct GPGPUVMFault - 一个 HQD 槽的首次用户访问故障
 * @address: 出错的原始用户地址。
 * @reason: GPGPU_VM_FAULT_* 原因；0 表示无故障。
 * @vmid: 请求的 VMID。
 * @access: 访问类型 GPGPU_VM_READ/WRITE/EXEC。
 * @phase: 访问阶段。
 */
typedef struct GPGPUVMFault {
	uint64_t address;
	uint32_t reason;
	uint32_t vmid;
	uint32_t access;
	uint32_t phase;
} GPGPUVMFault;

/**
 * struct GPGPUVMState - GPUVM 模块状态
 * @firmware: CP 固件本次启动写回的协议版本；0 表示尚未握手。
 * @inv_ack: VM_INV_ACK：最近一次失效完成的 VMID 位图或 GPGPU_VM_INV_ERROR。
 * @inv_vpn_start: VM_INV_VPN_START，范围失效的起始 VPN。
 * @inv_vpn_end: VM_INV_VPN_END，范围失效的结束 VPN（含）。
 * @hits: TLB 命中次数，32 位回绕，失效不清零。
 * @misses: TLB 未命中次数，同上。
 * @evictions: 遍历成功后替换有效项的次数，同上。
 * @contexts: 用户上下文，下标即 VMID；下标 0 不使用。
 * @sets: 组相联 TLB，下标为组号 VPN % GPGPU_VM_TLB_SETS。
 * @fault: 每个 HQD 槽的首次故障。
 */
typedef struct GPGPUVMState {
	uint32_t firmware;
	uint32_t inv_ack;
	uint32_t inv_vpn_start;
	uint32_t inv_vpn_end;
	uint32_t hits;
	uint32_t misses;
	uint32_t evictions;
	GPGPUVMContext contexts[GPGPU_VM_USERS + 1];
	GPGPUVMTlbSet sets[GPGPU_VM_TLB_SETS];
	GPGPUVMFault fault[GPGPU_HQD_SLOTS];
} GPGPUVMState;

/**
 * gpgpu_user_rw() - 以用户身份访问一段地址
 * @s: 设备实例，调用者持有主锁。
 * @vmid: 用户 VMID。
 * @slot: 故障归属的 HQD 槽。
 * @addr: 用户地址（GPUVA 或 GPU 物理地址）。
 * @buf: 数据缓冲区。
 * @len: 字节数；不超过 8 时视为标量，要求自然对齐且为 2 的幂。
 * @access: GPGPU_VM_READ、GPGPU_VM_WRITE 或 GPGPU_VM_EXEC。
 * @phase: 记录到故障中的访问阶段。
 *
 * 标量一次完成；更长的块逐页翻译，失败时保留已完成的部分。
 *
 * Return: 成功为 true；失败时记录 slot 的首次故障并返回 false。
 */
bool gpgpu_user_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr, void *buf,
		   size_t len, uint32_t access, uint32_t phase);

/**
 * gpgpu_vm_record_fault() - 为 slot 记录首次用户访问故障
 * @s: 设备实例。
 * @vmid: 请求的 VMID。
 * @slot: 来源 HQD 槽；越界时忽略。
 * @addr: 出错地址。
 * @access: 访问类型。
 * @phase: 访问阶段。
 * @reason: 非零的 GPGPU_VM_FAULT_*。
 *
 * 已有故障时保持原记录。用户故障只归属到队列，不升级为全局错误。
 */
void gpgpu_vm_record_fault(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr,
			   uint32_t access, uint32_t phase, uint32_t reason);

/* VMID 在用户范围内且其上下文已启用。 */
bool gpgpu_vm_context_enabled(GPGPUState *s, unsigned int vmid);

/* GPUVM 寄存器组（BAR0 0x900–0xaff）。CP 视图只读 0x900–0x9ff。 */
uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr reg);
void gpgpu_vm_write(GPGPUState *s, hwaddr reg, uint32_t value);

/**
 * gpgpu_vm_firmware_handshake() - 接收 CP 固件写回的协议版本
 * @s: 设备实例。
 * @version: 固件写入 VM_FIRMWARE 的值。
 *
 * Return: 版本与设备一致时保存并返回 true。
 */
bool gpgpu_vm_firmware_handshake(GPGPUState *s, uint32_t version);

/* CP 复位时清除握手，保留页表、上下文和故障记录。 */
void gpgpu_vm_firmware_reset(GPGPUState *s);

/* 设备复位时清除全部 GPUVM 状态与 TLB 计数，各组顺序恢复为 {0, 1, 2, 3}。 */
void gpgpu_vm_reset(GPGPUState *s);

#endif /* HW_GPGPU_MEM_GPUVM_H */
