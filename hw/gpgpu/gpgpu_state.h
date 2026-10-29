/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 设备的组装状态
 *
 * GPGPUState 把各模块的子状态放在一起。模块实现包含本文件只为取得自己的
 * 子状态（如 hqd.c 只访问 s->hqd）；读取或修改其他模块的状态必须调用对方
 * 头文件中的函数。全局寄存器（global_ctrl 等）只由 gpgpu.c 访问。
 */
#ifndef HW_GPGPU_STATE_H
#define HW_GPGPU_STATE_H

#include "hw/pci/pci_device.h"
#include "gpgpu.h"
#include "bar0.h"
#include "cp/cp_ctrl.h"
#include "cp/cp_window.h"
#include "cp/hqd.h"
#include "cp/doorbell.h"
#include "compute/dispatch.h"
#include "mem/vram.h"
#include "mem/phys.h"
#include "mem/gpuvm.h"
#include "mem/dma.h"
#include "irq/ih.h"
#include "irq/irq.h"

/**
 * struct GPGPUState - 设备实例
 * @parent_obj: PCI 父对象，必须位于首部。
 * @bar0: BAR0 容器，各模块寄存器组作为子区域挂入。
 * @bar0_regions: BAR0 的寄存器组子区域。
 * @num_cus: 属性：DEV_CAPS 报告的计算单元数。
 * @warps_per_cu: 属性：DEV_CAPS 报告的每单元 warp 数。
 * @vram_size: 属性：VRAM 字节数，2 的幂，4 KiB 到 2 GiB（不含）。
 * @gpuvm: 属性：用户地址是否经 GPUVM 翻译，设备寿命内不变。
 * @global_ctrl: GLOBAL_CTRL 中锁存的 ENABLE 位。
 * @global_status: GLOBAL_STATUS 的 READY/BUSY/ERROR 位。
 * @error_status: ERROR_STATUS 锁存的错误位。
 * @cp: CP 控制。
 * @window: CP 访存窗口配置。
 * @hqd: HQD 槽寄存器。
 * @doorbell: BAR4。
 * @dispatch: 执行器配置、任务与结果。
 * @vram: VRAM 存储与 BAR2。
 * @phys: Outbound 配置与在途物理访问计数。
 * @vm: GPUVM 上下文、TLB 与故障。
 * @dma: DMA 描述符与完成定时器。
 * @ih: IH Ring。
 * @irq: 中断使能与锁存。
 */
struct GPGPUState {
	PCIDevice parent_obj;
	MemoryRegion bar0;
	GPGPUBar0Region bar0_regions[GPGPU_BAR0_REGIONS];
	uint32_t num_cus;
	uint32_t warps_per_cu;
	uint64_t vram_size;
	bool gpuvm;
	uint32_t global_ctrl;
	uint32_t global_status;
	uint32_t error_status;
	GPGPUCPState cp;
	GPGPUCPWindowState window;
	GPGPUHQDState hqd;
	GPGPUDoorbellState doorbell;
	GPGPUDispatchState dispatch;
	GPGPUVRAMState vram;
	GPGPUPhysState phys;
	GPGPUVMState vm;
	GPGPUDMAState dma;
	GPGPUIHState ih;
	GPGPUIRQState irq;
};

#endif /* HW_GPGPU_STATE_H */
