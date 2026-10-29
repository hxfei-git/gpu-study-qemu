/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 教学设备：QOM 类型与跨模块的全局入口
 *
 * 设备由以下模块组成，每个模块只拥有自己的子状态（见 gpgpu_state.h），
 * 其他模块只能通过模块头文件里的函数访问它：
 *
 *   gpgpu.c   QOM/PCI 组装、全局控制、复位编排
 *   bar0.c    BAR0 寄存器分组转发
 *   cp/       原生 CP 控制、CP 访存窗口、HQD、Doorbell
 *   compute/  Dispatch 寄存器、warp 调度、RV32 解释器、lane 访存
 *   mem/      VRAM、GPU 物理地址路由、GPUVM 翻译、DMA
 *   irq/      中断锁存、IH 通知 Ring
 */
#ifndef HW_GPGPU_H
#define HW_GPGPU_H

#include "qom/object.h"
#include "qemu/bitops.h"
#include "hw/pci/pci_ids.h"
#include "gpgpu/gpgpu_protocol.h"

/* QOM 类型名，供 -device gpgpu 与类型转换使用。 */
#define TYPE_GPGPU "gpgpu"
OBJECT_DECLARE_SIMPLE_TYPE(GPGPUState, GPGPU)

/* PCI 修订号。 */
#define GPGPU_REVISION 0x01
/* PCI 类别：3D 控制器。 */
#define GPGPU_CLASS_CODE PCI_CLASS_DISPLAY_3D

/* QOM 属性 vram_size 的默认值，单位字节。 */
#define GPGPU_DEFAULT_VRAM_SIZE (64 * 1024 * 1024)
/* DEV_CAPS 报告的计算单元数默认值；执行器不使用该值。 */
#define GPGPU_DEFAULT_NUM_CUS 4
/* DEV_CAPS 报告的每计算单元 warp 数默认值；执行器不使用该值。 */
#define GPGPU_DEFAULT_WARPS_PER_CU 4

/**
 * gpgpu_set_error() - 锁存全局错误并通知 Host
 * @s: 设备实例。
 * @error: 要锁存的 GPGPU_ERR_* 位。
 *
 * 先更新 ERROR_STATUS 与 GLOBAL_STATUS，再发出 GPGPU_IRQ_ERROR。
 */
void gpgpu_set_error(GPGPUState *s, uint32_t error);

/**
 * gpgpu_enabled() - 查询 GLOBAL_CTRL 的使能位
 * @s: 设备实例。
 *
 * Return: Host 已使能设备时为 true；DMA 与派发在未使能时拒绝启动。
 */
bool gpgpu_enabled(GPGPUState *s);

/**
 * gpgpu_set_busy() - 更新 GLOBAL_STATUS 的执行器忙标志
 * @s: 设备实例。
 * @busy: 派发开始时为 true，任务终态时为 false。
 */
void gpgpu_set_busy(GPGPUState *s, bool busy);

/**
 * gpgpu_gpuvm_enabled() - 查询设备是否以 gpuvm=on 启动
 * @s: 设备实例。
 *
 * Return: 属性在设备寿命内不变；为 true 时用户地址需经页表翻译。
 */
bool gpgpu_gpuvm_enabled(GPGPUState *s);

/**
 * gpgpu_reset_registers() - 停止执行并复位全部寄存器，保留 VRAM
 * @s: 设备实例。
 *
 * 由 GLOBAL_CTRL.RESET 与 QOM 复位调用。仍有跨进程物理访问在途时拒绝
 * 复位并锁存 INVALID_CMD。
 *
 * Return: 已复位为 true，拒绝复位为 false。
 */
bool gpgpu_reset_registers(GPGPUState *s);

/**
 * gpgpu_engines_idle() - 查询全部设备引擎是否停止
 * @s: 设备实例。
 *
 * CP 保持复位、执行器不忙、DMA 不忙、IH 不在写记录、没有激活的 HQD 时才
 * 为 true。全设备共享的地址配置（如 Outbound）只在此时允许修改。
 */
bool gpgpu_engines_idle(GPGPUState *s);

/**
 * gpgpu_range_ok() - 检查 [addr, addr + len) 是否位于 [0, limit) 内
 * @addr: 起始地址。
 * @len: 字节数；为 0 时只检查起点。
 * @limit: 上界（不含）。
 *
 * 用减法比较，避免 addr + len 溢出。
 */
static inline bool gpgpu_range_ok(uint64_t addr, uint64_t len, uint64_t limit)
{
	return addr < limit && len <= limit - addr;
}

/**
 * gpgpu_reg64_read() - 读取 64 位值的一半，供 LO/HI 寄存器对使用
 * @value: 完整 64 位值。
 * @half: 寄存器相对 *_LO 的字节偏移：0 为低 32 位，4 为高 32 位。
 */
static inline uint32_t gpgpu_reg64_read(uint64_t value, unsigned int half)
{
	return value >> (half * 8);
}

/**
 * gpgpu_reg64_write() - 更新 64 位值的一半，供 LO/HI 寄存器对使用
 * @value: 被更新的 64 位值。
 * @half: 寄存器相对 *_LO 的字节偏移：0 为低 32 位，4 为高 32 位。
 * @data: 写入的 32 位值。
 */
static inline void gpgpu_reg64_write(uint64_t *value, unsigned int half, uint32_t data)
{
	*value = deposit64(*value, half * 8, 32, data);
}

#endif /* HW_GPGPU_H */
