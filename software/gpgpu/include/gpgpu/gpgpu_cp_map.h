/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：RV64 CP 的私有地址空间
 *
 * 这些地址只在 CP 固件看到的 4 GiB 物理空间内有效，不是 BAR0 偏移：
 *
 *   0x0000_0000  VRAM 别名（固件代码、数据与栈位于末尾 4 MiB）
 *   0x8000_0000  64 KiB 访存窗口：按当前窗口配置访问 GPU 物理地址或用户地址
 *   0xfffd_0000  本地寄存器窗口：Dispatch、IH、VM 只读信息的 CP 视图
 *   0xfffe_0000  访存窗口配置寄存器
 *   0xffff_0000  HQD 直接访问窗口，每槽 0x100 字节
 *
 * 设备在板级代码中按此布局建立映射，固件按此布局访问。
 */
#ifndef GPGPU_CP_MAP_H
#define GPGPU_CP_MAP_H

/* 固件在 VRAM 中的保留区起点，也是驱动可分配 VRAM 的上界。 */
#define GPGPU_FIRMWARE_BASE 0x03c00000u
/* 固件保留区字节数，包含代码、数据与栈。 */
#define GPGPU_FIRMWARE_SIZE 0x00400000u
/* 固件栈区起点；代码与数据不得越过此地址。 */
#define GPGPU_FIRMWARE_STACK_BASE 0x03ff0000u
/* 固件初始栈顶，向低地址增长。 */
#define GPGPU_FIRMWARE_STACK_TOP 0x04000000u
/* 驱动可分配的 VRAM 字节数（固件区之前）；驱动另行保留首页。 */
#define GPGPU_VRAM_USABLE_SIZE GPGPU_FIRMWARE_BASE

/* 访存窗口：CP 对窗口内偏移的读写，转发到“窗口基址 + 偏移”。 */
#define GPGPU_CP_WINDOW_BASE 0x80000000u
/* 访存窗口字节数，也是窗口基址的对齐单位。 */
#define GPGPU_CP_WINDOW_SIZE 0x00010000u

/* 访存窗口配置寄存器组的 CP 本地基址，每项 32 位。 */
#define GPGPU_CP_WINDOW_REGS 0xfffe0000u
/* 窗口目标基址低 32 位，低 16 位读零。 */
#define GPGPU_CP_WINDOW_BASE_LO 0x00
/* 窗口目标基址高 32 位。 */
#define GPGPU_CP_WINDOW_BASE_HI 0x04
/* 访问域：GPGPU_CP_DOMAIN_MANAGEMENT 或 GPGPU_CP_DOMAIN_USER。 */
#define GPGPU_CP_WINDOW_DOMAIN 0x08
/* 用户域访问使用的 VMID。 */
#define GPGPU_CP_WINDOW_VMID 0x0c
/* 访问失败时记录故障的 HQD 槽号。 */
#define GPGPU_CP_WINDOW_SLOT 0x10
/* 访问失败时记录的阶段，固件使用 GpgpuQueueError 编号。 */
#define GPGPU_CP_WINDOW_PHASE 0x14
/* 配置寄存器组字节数。 */
#define GPGPU_CP_WINDOW_REGS_SIZE 0x18

/* 管理域：窗口目标是 GPU 物理地址，用于 MQD、管理 Ring 与结果区。 */
#define GPGPU_CP_DOMAIN_MANAGEMENT 0U
/* 用户域：窗口目标是用户地址，GPUVM 开启时翻译，关闭时直通。 */
#define GPGPU_CP_DOMAIN_USER 1U

/* 本地寄存器窗口基址；窗口内偏移与 BAR0 偏移相同。 */
#define GPGPU_CP_REG_BASE 0xfffd0000u
/* 本地寄存器窗口字节数。 */
#define GPGPU_CP_REG_SIZE 0x1000u

/* HQD 直接访问窗口基址；槽 N 字段 F 位于 BASE + N * STRIDE + F。 */
#define GPGPU_CP_HQD_BASE 0xffff0000u
/* 相邻 HQD 槽的字节间隔，与 Host 选槽窗口大小相同。 */
#define GPGPU_HQD_STRIDE 0x100u

#endif /* GPGPU_CP_MAP_H */
