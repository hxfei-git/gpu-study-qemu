/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：GPUVM 地址与页表项
 *
 * 用户 GPU 虚拟地址为 32 位，三级 4 KiB 页表；GPUVM 关闭时用户地址直接是
 * GPU 物理地址。VM 控制寄存器见 gpgpu_regs.h。
 */
#ifndef GPGPU_VM_ABI_H
#define GPGPU_VM_ABI_H

/* GPUVM 页大小，单位字节。 */
#define GPGPU_VM_PAGE_SIZE 4096U
/* 用户 VMID 数量；VMID 0 保留给管理域，用户使用 1、2。 */
#define GPGPU_VM_USERS 2U
/* GPU 地址空间上界（不含）：GPUVA 与 GPU 物理地址都在 32 位内。 */
#define GPGPU_VM_ADDRESS_LIMIT 0x100000000ULL
/* 驱动为每个 VM 保留的通知槽页 GPUVA，位于地址空间末页。 */
#define GPGPU_VM_NOTIFY_VA 0xfffff000U

/*
 * 页表项地址域 [31:12]：SYSTEM 为 1 时是 Host IOVA 页，设备经 Outbound
 * 访问 GPU 物理页“窗口起点 + IOVA”；为 0 时是 VRAM 偏移。
 */
#define GPGPU_VM_ADDR_MASK 0xfffff000ULL
/* 页表项有效位。 */
#define GPGPU_VM_VALID (1ULL << 0)
/* 地址域保存 Host IOVA；中间项与叶子项都可以设置，bit 2 仍保留。 */
#define GPGPU_VM_SYSTEM (1ULL << 1)
/* 叶子项取指权限，也用作访问类型 EXEC。 */
#define GPGPU_VM_EXEC (1ULL << 4)
/* 叶子项读权限，也用作访问类型 READ。 */
#define GPGPU_VM_READ (1ULL << 5)
/* 叶子项写权限，也用作访问类型 WRITE。 */
#define GPGPU_VM_WRITE (1ULL << 6)
/* 叶子项全部权限位。 */
#define GPGPU_VM_PERMS (GPGPU_VM_READ | GPGPU_VM_WRITE | GPGPU_VM_EXEC)
/* 中间项允许出现的全部标志位。 */
#define GPGPU_VM_DIR_FLAGS (GPGPU_VM_VALID | GPGPU_VM_SYSTEM)
/* 叶子项允许出现的全部标志位；用户映射的权限只能取 GPGPU_VM_PERMS。 */
#define GPGPU_VM_FLAGS (GPGPU_VM_DIR_FLAGS | GPGPU_VM_PERMS)

#endif /* GPGPU_VM_ABI_H */
