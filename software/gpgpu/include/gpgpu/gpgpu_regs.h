/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：BAR0 寄存器
 *
 * BAR0 是 1 MiB 控制空间，寄存器为对齐的 32 位小端值，按 0x100 字节分组：
 *
 *   0x000 设备信息        0x100 全局控制        0x200 中断
 *   0x300 Dispatch（CP）  0x400 DMA             0x500 CP 控制
 *   0x600 HQD 选槽        0x700 HQD 窗口        0x800 IH
 *   0x900 GPUVM 控制      0xa00 GPUVM 故障      0xb00 Outbound
 *
 * 0x300 组与 IH_SUBMIT 只能由 CP 经本地寄存器窗口写入（见 gpgpu_cp_map.h），
 * Host 写入锁存 INVALID_CMD。BAR2 为 VRAM，BAR4 为 Doorbell。
 */
#ifndef GPGPU_REGS_H
#define GPGPU_REGS_H

/* ---- PCI 身份与 BAR 尺寸 ---- */

/* 教学设备的 PCI 厂商编号。 */
#define GPGPU_VENDOR_ID 0x1234
/* 教学设备的 PCI 设备编号。 */
#define GPGPU_DEVICE_ID 0x1337
/* BAR0 控制空间字节数，末尾 8 KiB 放 MSI-X 表和 PBA。 */
#define GPGPU_CTRL_BAR_SIZE (1 * 1024 * 1024)
/* BAR4 Doorbell 空间字节数；每个入口 8 字节。 */
#define GPGPU_DOORBELL_BAR_SIZE 65536u

/* ---- 0x000 设备信息（只读） ---- */

/* 设备签名，读回 GPGPU_DEV_ID_VALUE。 */
#define GPGPU_REG_DEV_ID 0x0000
/* 设备版本，读回 GPGPU_DEV_VERSION_VALUE（见 gpgpu_version.h）。 */
#define GPGPU_REG_DEV_VERSION 0x0004
/* 设备能力：[7:0] 计算单元数，[15:8] 每单元 warp 数，[23:16] warp 宽度。 */
#define GPGPU_REG_DEV_CAPS 0x0008
/* VRAM 字节数低 32 位。 */
#define GPGPU_REG_VRAM_SIZE_LO 0x000c
/* VRAM 字节数高 32 位。 */
#define GPGPU_REG_VRAM_SIZE_HI 0x0010
/* DEV_ID 读回的签名，ASCII "GPPU"。 */
#define GPGPU_DEV_ID_VALUE 0x47505055

/* ---- 0x100 全局控制 ---- */

/* 读写：ENABLE 锁存，写 RESET 复位设备寄存器（不锁存）。 */
#define GPGPU_REG_GLOBAL_CTRL 0x0100
/* 只读：GPGPU_STATUS_* 标志。 */
#define GPGPU_REG_GLOBAL_STATUS 0x0104
/* 锁存的 GPGPU_ERR_* 错误，写 1 清除对应位。 */
#define GPGPU_REG_ERROR_STATUS 0x0108

/* GLOBAL_CTRL：设备使能，允许计算与 DMA。 */
#define GPGPU_CTRL_ENABLE (1u << 0)
/* GLOBAL_CTRL：复位命令，保留 VRAM。 */
#define GPGPU_CTRL_RESET (1u << 1)

/* GLOBAL_STATUS：设备已就绪。 */
#define GPGPU_STATUS_READY (1u << 0)
/* GLOBAL_STATUS：计算执行器正忙。 */
#define GPGPU_STATUS_BUSY (1u << 1)
/* GLOBAL_STATUS：ERROR_STATUS 非零。 */
#define GPGPU_STATUS_ERROR (1u << 2)

/* ERROR_STATUS：寄存器访问、命令或配置非法。 */
#define GPGPU_ERR_INVALID_CMD (1u << 0)
/* ERROR_STATUS：BAR2 访问越界。 */
#define GPGPU_ERR_VRAM_FAULT (1u << 1)
/* ERROR_STATUS：DMA 描述符非法或传输失败。 */
#define GPGPU_ERR_DMA_FAULT (1u << 3)
/* ERROR_STATUS：IH 通道配置、协议或提交失败。 */
#define GPGPU_ERR_IH_FAULT (1u << 4)
/* ERROR_STATUS：CP 固件报告不可恢复异常。 */
#define GPGPU_ERR_FIRMWARE (1u << 5)

/* ---- 0x200 中断 ---- */

/* 读写：事件使能掩码，只保留 GPGPU_IRQ_ALL 中的位。 */
#define GPGPU_REG_IRQ_ENABLE 0x0200
/* 只读：已锁存、待确认的事件。 */
#define GPGPU_REG_IRQ_STATUS 0x0204
/* 写 1 清除 IRQ_STATUS 对应位；IH 仍有未消费记录时会再次锁存。 */
#define GPGPU_REG_IRQ_ACK 0x0208

/* IRQ 事件：DMA 传输完成。 */
#define GPGPU_IRQ_DMA_DONE (1u << 1)
/* IRQ 事件：锁存了新的全局错误。 */
#define GPGPU_IRQ_ERROR (1u << 2)
/* IRQ 事件：IH Ring 中有未消费记录。 */
#define GPGPU_IRQ_IH (1u << 3)
/* 设备定义的全部 IRQ 事件。 */
#define GPGPU_IRQ_ALL (GPGPU_IRQ_DMA_DONE | GPGPU_IRQ_ERROR | GPGPU_IRQ_IH)

/* MSI-X 向量数；向量 0 不使用。 */
#define GPGPU_MSIX_VECTORS 4
/* DMA 完成使用的 MSI-X 向量。 */
#define GPGPU_MSIX_VEC_DMA 1
/* 全局错误使用的 MSI-X 向量。 */
#define GPGPU_MSIX_VEC_ERROR 2
/* IH 非空使用的 MSI-X 向量。 */
#define GPGPU_MSIX_VEC_IH 3

/* ---- 0x300 Dispatch：CP 写配置并提交，Host 只读 ---- */

/* Kernel 代码入口低 32 位：GPUVM 开启时为 GPUVA，否则为 GPU 物理地址。 */
#define GPGPU_REG_KERNEL_ADDR_LO 0x0300
/* Kernel 代码入口高 32 位，须为零。 */
#define GPGPU_REG_KERNEL_ADDR_HI 0x0304
/* 参数块地址低 32 位，按 8 字节对齐，与代码入口同一地址空间。 */
#define GPGPU_REG_KERNEL_ARGS_LO 0x0308
/* 参数块地址高 32 位，须为零。 */
#define GPGPU_REG_KERNEL_ARGS_HI 0x030c
/* X 方向工作组数（不是线程数）。 */
#define GPGPU_REG_GRID_DIM_X 0x0310
/* Y 方向工作组数。 */
#define GPGPU_REG_GRID_DIM_Y 0x0314
/* Z 方向工作组数。 */
#define GPGPU_REG_GRID_DIM_Z 0x0318
/* 工作组 X 方向线程数。 */
#define GPGPU_REG_BLOCK_DIM_X 0x031c
/* 工作组 Y 方向线程数。 */
#define GPGPU_REG_BLOCK_DIM_Y 0x0320
/* 工作组 Z 方向线程数。 */
#define GPGPU_REG_BLOCK_DIM_Z 0x0324
/* 每工作组共享内存字节数，本版只接受 0。 */
#define GPGPU_REG_SHARED_MEM_SIZE 0x0328
/* 代码字节数，非零且为 4 的倍数，限定取指范围。 */
#define GPGPU_REG_KERNEL_CODE_SIZE 0x032c
/* 写 1 提交当前配置；只在 DISPATCH_STATUS 为 IDLE 时接受。 */
#define GPGPU_REG_DISPATCH 0x0330
/* 参数块字节数，4 到 GPGPU_KERNEL_ARGS_MAX，按 4 字节对齐。 */
#define GPGPU_REG_KERNEL_ARGS_SIZE 0x0334
/* 只读：GpgpuDispatchStatus。 */
#define GPGPU_REG_DISPATCH_STATUS 0x0338
/* 只读：GpgpuDispatchError，终态时有效。 */
#define GPGPU_REG_DISPATCH_ERROR 0x033c
/* 终态时写 1 确认结果，执行器回到 IDLE 并清除 DISPATCH_ERROR。 */
#define GPGPU_REG_DISPATCH_ACK 0x0340
/* 任务所属用户 VMID，访存翻译与故障归属使用。 */
#define GPGPU_REG_DISPATCH_VMID 0x0344
/* 任务来源 HQD 槽号，须为已激活的用户槽。 */
#define GPGPU_REG_DISPATCH_SLOT 0x0348

/* 单次派发参数块字节数上限。 */
#define GPGPU_KERNEL_ARGS_MAX 65536u

/**
 * enum GpgpuDispatchStatus - DISPATCH_STATUS 的取值，终态须由 CP 确认后回到空闲
 * @GPGPU_DISPATCH_IDLE: 允许配置和提交新任务。
 * @GPGPU_DISPATCH_BUSY: 任务正在分段执行，拒绝覆盖配置。
 * @GPGPU_DISPATCH_DONE: 任务及相关访问已成功结束，等待 ACK。
 * @GPGPU_DISPATCH_FAILED: 任务已停止并保存错误，等待 ACK。
 */
enum GpgpuDispatchStatus {
	GPGPU_DISPATCH_IDLE,
	GPGPU_DISPATCH_BUSY,
	GPGPU_DISPATCH_DONE,
	GPGPU_DISPATCH_FAILED,
};

/**
 * enum GpgpuDispatchError - DISPATCH_ERROR 的细分执行错误
 * @GPGPU_EXEC_ERR_NONE: 未记录执行错误。
 * @GPGPU_EXEC_ERR_INVALID_CONFIG: 提交时配置、队列绑定或网格规模非法。
 * @GPGPU_EXEC_ERR_CODE_RANGE: 取指越出代码范围或取指访问失败。
 * @GPGPU_EXEC_ERR_INSTRUCTION: 遇到非法或未支持的指令。
 * @GPGPU_EXEC_ERR_MEMORY: 线程的数据访问失败。
 * @GPGPU_EXEC_ERR_BUDGET: 单个 warp 耗尽执行预算。
 */
enum GpgpuDispatchError {
	GPGPU_EXEC_ERR_NONE,
	GPGPU_EXEC_ERR_INVALID_CONFIG,
	GPGPU_EXEC_ERR_CODE_RANGE,
	GPGPU_EXEC_ERR_INSTRUCTION,
	GPGPU_EXEC_ERR_MEMORY,
	GPGPU_EXEC_ERR_BUDGET,
};

/* ---- 0x400 DMA：Host RAM 与 VRAM 之间的同步搬运 ---- */

/* 源地址低 32 位：H2D 为 Host 端 GPU 物理地址，D2H 为 VRAM 偏移。 */
#define GPGPU_REG_DMA_SRC_LO 0x0400
/* 源地址高 32 位。 */
#define GPGPU_REG_DMA_SRC_HI 0x0404
/* 目标地址低 32 位：H2D 为 VRAM 偏移，D2H 为 Host 端 GPU 物理地址。 */
#define GPGPU_REG_DMA_DST_LO 0x0408
/* 目标地址高 32 位。 */
#define GPGPU_REG_DMA_DST_HI 0x040c
/* 传输字节数，非零。 */
#define GPGPU_REG_DMA_SIZE 0x0410
/* GPGPU_DMA_* 控制位；写入 START 时立即执行。 */
#define GPGPU_REG_DMA_CTRL 0x0414
/* 只读：GPGPU_DMA_IDLE/BUSY/COMPLETE/ERROR。 */
#define GPGPU_REG_DMA_STATUS 0x0418

/* DMA_CTRL：启动传输，处理后自动清除。 */
#define GPGPU_DMA_START (1u << 0)
/* DMA_CTRL：方向位，置位为 VRAM 到 Host（D2H），清零为 H2D。 */
#define GPGPU_DMA_DIR_FROM_VRAM (1u << 1)
/* DMA_CTRL：完成时发出 GPGPU_IRQ_DMA_DONE。 */
#define GPGPU_DMA_IRQ_ENABLE (1u << 2)

/* DMA_STATUS：空闲，尚未提交。 */
#define GPGPU_DMA_IDLE 0u
/* DMA_STATUS：数据已搬运，完成通知尚未发布。 */
#define GPGPU_DMA_BUSY (1u << 0)
/* DMA_STATUS：搬运与完成通知均已结束。 */
#define GPGPU_DMA_COMPLETE (1u << 1)
/* DMA_STATUS：描述符非法或传输失败。 */
#define GPGPU_DMA_ERROR (1u << 2)

/* ---- 0x500 CP 控制（未连接原生 CP 时读零、写忽略） ---- */

/* RESET_HOLD 读写，PRESENT 只读。 */
#define GPGPU_REG_CP_CTRL 0x0500
/* CP 复位向量，VRAM 字节偏移；只在保持复位时可写。 */
#define GPGPU_REG_CP_RESET_VECTOR 0x0504

/* CP_CTRL：保持复位；清零后 CP 从复位向量开始执行。 */
#define GPGPU_CP_RESET_HOLD (1u << 0)
/* CP_CTRL：已接入原生 CP。 */
#define GPGPU_CP_PRESENT (1u << 1)

/* ---- 0x600/0x700 HQD：Host 经选槽窗口访问 4 个硬件队列槽 ---- */

/* 选择 0x700 窗口对应的槽号，0 到 GPGPU_HQD_SLOTS - 1。 */
#define GPGPU_REG_HQD_SELECT 0x0600
/* 所选槽的字段窗口；窗口内偏移即下面的 GPGPU_HQD_* 字段偏移。 */
#define GPGPU_REG_HQD_WINDOW 0x0700

/* HQD 槽总数。 */
#define GPGPU_HQD_SLOTS 4
/* 槽 0：KIQ，由 Host 在 CP 冷启动前配置，管理 HIQ。 */
#define GPGPU_HQD_SLOT_KIQ 0
/* 槽 1：HIQ，由 KIQ 映射，管理用户队列。 */
#define GPGPU_HQD_SLOT_HIQ 1
/* 第一个用户 AQL 槽；用户槽为 2、3。 */
#define GPGPU_HQD_USER_FIRST 2

/* MQD GPU 物理地址低 32 位。 */
#define GPGPU_HQD_MQD_LO 0x000
/* MQD GPU 物理地址高 32 位。 */
#define GPGPU_HQD_MQD_HI 0x004
/* 激活标志；清零只停止取包，不代表槽已释放。 */
#define GPGPU_HQD_ACTIVE 0x008
/* 所属 VMID：管理槽为 0，用户槽为 1 或 2。 */
#define GPGPU_HQD_VMID 0x00c
/* Ring 地址低 32 位。 */
#define GPGPU_HQD_RING_LO 0x020
/* Ring 地址高 32 位。 */
#define GPGPU_HQD_RING_HI 0x024
/* CP 下一待消费包的 32 位编号 R。 */
#define GPGPU_HQD_RPTR 0x028
/* 共享 R 写回地址低 32 位。 */
#define GPGPU_HQD_RPTR_ADDR_LO 0x02c
/* 共享 R 写回地址高 32 位。 */
#define GPGPU_HQD_RPTR_ADDR_HI 0x030
/* 共享 W 轮询地址低 32 位。 */
#define GPGPU_HQD_WPTR_ADDR_LO 0x034
/* 共享 W 轮询地址高 32 位。 */
#define GPGPU_HQD_WPTR_ADDR_HI 0x038
/* Doorbell：ENABLE 位与 BAR4 字节偏移。 */
#define GPGPU_HQD_DOORBELL 0x03c
/* Ring 字节容量，2 的幂，范围见 gpgpu_queue_abi.h 的 GPGPU_QUEUE_RING_MIN/MAX。 */
#define GPGPU_HQD_CONTROL 0x044
/* 首次队列错误 GpgpuQueueError；Host 只读，CP 停用槽后可清除。 */
#define GPGPU_HQD_ERROR 0x0cc
/* 包格式：0 为管理命令，1 为用户 AQL。 */
#define GPGPU_HQD_AQL 0x0d4

/* HQD_DOORBELL：Doorbell 使能。 */
#define GPGPU_DOORBELL_ENABLE (1u << 30)
/* HQD_DOORBELL：BAR4 字节偏移字段，另须 8 字节对齐。 */
#define GPGPU_DOORBELL_OFFSET_MASK 0x0ffffffcu

/* ---- 0x800 IH：设备写 Host RAM 中的通知记录 Ring ---- */

/* Ring GPU 物理基址低 32 位，按 4 KiB 对齐；停用时可写。 */
#define GPGPU_REG_IH_BASE_LO 0x0800
/* Ring GPU 物理基址高 32 位。 */
#define GPGPU_REG_IH_BASE_HI 0x0804
/* Ring 字节容量，2 的幂，至少一条记录；停用时可写。 */
#define GPGPU_REG_IH_SIZE 0x0808
/* 只读：设备下一条待写记录的 32 位编号 W。 */
#define GPGPU_REG_IH_WPTR 0x080c
/* Host 写回下一条待消费记录编号 R，不能越过 W。 */
#define GPGPU_REG_IH_RPTR 0x0810
/* 只读：BUSY、首次故障码与最近一次提交结果。 */
#define GPGPU_REG_IH_STATUS 0x0814
/* GPGPU_IH_ENABLE 与 GPGPU_IH_RESET。 */
#define GPGPU_REG_IH_CTRL 0x0818
/* 仅 CP：写入固件私有 VRAM 中一条记录的地址，设备同步提交。 */
#define GPGPU_REG_IH_SUBMIT 0x081c

/* IH_CTRL：使能；使能期间禁止改写 Ring 配置。 */
#define GPGPU_IH_ENABLE (1u << 0)
/* IH_CTRL：清零 W/R 与故障，只在停用且不忙时接受。 */
#define GPGPU_IH_RESET (1u << 1)
/* IH_STATUS：正在写入一条记录。 */
#define GPGPU_IH_BUSY (1u << 0)
/* IH_STATUS：已锁存首次通道故障，之后拒绝提交。 */
#define GPGPU_IH_FAULT (1u << 1)
/* IH_STATUS：首次故障码 GpgpuIhError 的起始位，占 8 位。 */
#define GPGPU_IH_ERROR_SHIFT 8
/* IH_STATUS：最近一次提交结果 GpgpuIhError 的起始位，占 8 位。 */
#define GPGPU_IH_RESULT_SHIFT 16

/* ---- 0x900 GPUVM 控制 ---- */

/* 只读：GPGPU_VM_VERSION。 */
#define GPGPU_REG_VM_VERSION 0x0900
/* 只读：1 表示设备以 gpuvm=on 启动。 */
#define GPGPU_REG_VM_MODE 0x0904
/* CP 启动时写回 GPGPU_VM_VERSION 完成握手；Host 只读。 */
#define GPGPU_REG_VM_FIRMWARE 0x0908
/* 写用户 VMID 触发该 VM 的同步 TLB 失效。 */
#define GPGPU_REG_VM_INVALIDATE 0x0910
/* 只读：最近一次失效的 BUSY/ACK/ERROR。 */
#define GPGPU_REG_VM_STATUS 0x0914
/* 用户 VMID 的上下文寄存器组，每组 0x20 字节。 */
#define GPGPU_REG_VM_CONTEXT(vmid) (0x0940 + (vmid) * 0x20)
/* 上下文内：页表根 GPU 物理地址低 32 位，按页对齐。 */
#define GPGPU_VM_ROOT_LO 0x00
/* 上下文内：页表根高 32 位，须为零。 */
#define GPGPU_VM_ROOT_HI 0x04
/* 上下文内：1 启用；启用后根只读。 */
#define GPGPU_VM_ENABLE 0x08

/* VM_STATUS：失效进行中。 */
#define GPGPU_VM_BUSY 1U
/* VM_STATUS：失效已完成。 */
#define GPGPU_VM_ACK 2U
/* VM_STATUS：VMID 非法，或该 VM 仍有活跃队列或在途访问。 */
#define GPGPU_VM_ERROR 4U

/* ---- 0xa00 GPUVM 故障：每 HQD 槽保存首次用户访问故障 ---- */

/* 槽的故障寄存器组，每组 0x40 字节；读取不清除。 */
#define GPGPU_REG_VM_FAULT(slot) (0x0a00 + (slot) * 0x40)
/* 组内：GPGPU_VM_FAULT_* 原因，零表示无故障。 */
#define GPGPU_VM_FAULT_REASON 0x00
/* 组内：请求的 VMID。 */
#define GPGPU_VM_FAULT_VMID 0x04
/* 组内：原始地址低 32 位。 */
#define GPGPU_VM_FAULT_ADDR_LO 0x08
/* 组内：原始地址高 32 位。 */
#define GPGPU_VM_FAULT_ADDR_HI 0x0c
/* 组内：访问类型 GPGPU_VM_READ/WRITE/EXEC。 */
#define GPGPU_VM_FAULT_ACCESS 0x10
/* 组内：访问阶段，CP 使用 GpgpuQueueError，执行器使用 GPGPU_VM_PHASE_*。 */
#define GPGPU_VM_FAULT_PHASE 0x14
/* 组内：槽停用后写 1 清除故障。 */
#define GPGPU_VM_FAULT_CLEAR 0x18

/* 故障原因：VMID 无效或上下文未启用。 */
#define GPGPU_VM_FAULT_CONTEXT 1U
/* 故障原因：地址越界或未对齐。 */
#define GPGPU_VM_FAULT_RANGE 2U
/* 故障原因：页表项无效。 */
#define GPGPU_VM_FAULT_UNMAPPED 3U
/* 故障原因：表页不可访问或表项含保留位。 */
#define GPGPU_VM_FAULT_TABLE 4U
/* 故障原因：页权限不允许本次访问。 */
#define GPGPU_VM_FAULT_PERMISSION 5U
/* 故障原因：翻译后的物理访问失败。 */
#define GPGPU_VM_FAULT_STORAGE 6U

/* 执行器访问阶段：取指。 */
#define GPGPU_VM_PHASE_FETCH 0x100U
/* 执行器访问阶段：读参数。 */
#define GPGPU_VM_PHASE_ARGS 0x101U
/* 执行器访问阶段：读写数据。 */
#define GPGPU_VM_PHASE_DATA 0x102U

/* ---- 0xb00 Outbound：GPU 物理地址 [VRAM 容量, 4 GiB) 映射到 Host IOVA ---- */

/* 窗口在 GPU 物理空间的起点，须等于 VRAM 容量。 */
#define GPGPU_REG_OUTBOUND_GPU_BASE 0x0b00
/* 窗口字节数低 32 位。 */
#define GPGPU_REG_OUTBOUND_SIZE_LO 0x0b04
/* 窗口字节数高 32 位。 */
#define GPGPU_REG_OUTBOUND_SIZE_HI 0x0b08
/* 窗口起点对应的 Host IOVA 低 32 位，须为零。 */
#define GPGPU_REG_OUTBOUND_HOST_LO 0x0b0c
/* 窗口起点对应的 Host IOVA 高 32 位。 */
#define GPGPU_REG_OUTBOUND_HOST_HI 0x0b10
/* GPGPU_OUTBOUND_ENABLE；只在 CP 保持复位且无活跃访问时可写。 */
#define GPGPU_REG_OUTBOUND_CONTROL 0x0b14
/* 只读：ACK、BUSY、ERROR。 */
#define GPGPU_REG_OUTBOUND_STATUS 0x0b18

/* OUTBOUND_CONTROL：窗口使能。 */
#define GPGPU_OUTBOUND_ENABLE 1U
/* OUTBOUND_STATUS：最近一次 CONTROL 写入被接受。 */
#define GPGPU_OUTBOUND_ACK 1U
/* OUTBOUND_STATUS：仍有经窗口的在途访问。 */
#define GPGPU_OUTBOUND_BUSY 2U
/* OUTBOUND_STATUS：最近一次写入被拒绝。 */
#define GPGPU_OUTBOUND_ERROR 4U

#endif /* GPGPU_REGS_H */
