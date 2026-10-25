/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 仅组合设备和模块状态；模块间调用须显式包含对应接口头。 */
#ifndef HW_GPGPU_INTERNAL_H
#define HW_GPGPU_INTERNAL_H

#include "gpgpu.h"
#include "hw/pci/pci_device.h"
#include "gpgpu_mmio.h"
#include "cp/gpgpu_cp.h"
#include "cp/gpgpu_hqd.h"
#include "cp/gpgpu_doorbell.h"
#include "compute/gpgpu_dispatch.h"
#include "memory/gpgpu_vram.h"
#include "memory/gpgpu_vm.h"
#include "memory/gpgpu_dma.h"
#include "interrupt/gpgpu_ih.h"
#include "interrupt/gpgpu_irq.h"

/**
 * struct GPGPUState - 设备组装状态，仅实现文件可见
 * @parent_obj: QOM 的 PCI 父对象，必须位于首部。
 * @ctrl_mmio: BAR0 全局控制窗口。
 * @num_cus: DEV_CAPS 报告的计算单元数属性。
 * @warps_per_cu: DEV_CAPS 报告的每单元 warp 数属性。
 * @warp_size: 每 warp 的 lane 数属性，当前固定为 32。
 * @vram_size: VRAM 字节容量属性。
 * @gpuvm: 设备寿命内不变的 GPUVM 启动属性。
 * @global_ctrl: 全局使能，不锁存 RESET 命令。
 * @global_status: 就绪、忙和错误标志。
 * @error_status: 按位累积、写一清除的全局错误。
 * @cp: CP 模块维护的控制与窗口状态。
 * @hqd: HQD 模块维护的队列与门铃提示。
 * @doorbell: Doorbell 模块维护的 BAR4 窗口。
 * @dispatch: Dispatch 模块维护的配置、任务句柄与结果。
 * @vram: VRAM 模块拥有的共享存储。
 * @vm: GPUVM 模块拥有的地址翻译状态。
 * @dma: DMA 模块拥有的描述符与完成定时器。
 * @ih: IH 模块拥有的记录 Ring。
 * @irq: IRQ 模块拥有的中断锁存。
 * @simt: MMIO 模块维护的教学读回寄存器。
 */
struct GPGPUState {
	PCIDevice parent_obj;
	MemoryRegion ctrl_mmio;
	uint32_t num_cus;
	uint32_t warps_per_cu;
	uint32_t warp_size;
	uint64_t vram_size;
	bool gpuvm;
	GPGPUControl global_ctrl;
	GPGPUStatus global_status;
	GPGPUError error_status;
	GPGPUCPState cp;
	GPGPUHQDState hqd;
	GPGPUDoorbellState doorbell;
	GPGPUDispatchState dispatch;
	GPGPUVRAMState vram;
	GPGPUVMState vm;
	GPGPUDMAState dma;
	GPGPUIHState ih;
	GPGPUIRQState irq;
	GPGPUSIMTContext simt;
};

#endif /* HW_GPGPU_INTERNAL_H */
