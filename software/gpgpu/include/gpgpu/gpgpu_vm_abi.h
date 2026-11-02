/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：GPUVM 地址与页表项
 *
 * 用户 GPU 虚拟地址有效 48 位，四级 4 KiB 页表：
 *
 *   GPUVA 位   63..48    47..39   38..30   29..21   20..12   11..0
 *   用途       必须为 0   L0 索引   L1 索引   L2 索引   L3 索引   页内偏移
 *
 * GPU 物理地址仍为 32 位；GPUVM 关闭时用户地址直接是 GPU 物理地址。
 * VM 控制寄存器见 gpgpu_regs.h。
 */
#ifndef GPGPU_VM_ABI_H
#define GPGPU_VM_ABI_H

/* 页内偏移位数。 */
#define GPGPU_VM_PAGE_SHIFT 12U
/* GPUVM 页大小，单位字节。 */
#define GPGPU_VM_PAGE_SIZE (1U << GPGPU_VM_PAGE_SHIFT)
/* 页表级数：L0 为根，L3 为叶子；每张表占一页、512 项。 */
#define GPGPU_VM_LEVELS 4U
/* 每级索引位数。 */
#define GPGPU_VM_INDEX_BITS 9U
/* 每级索引掩码，取值 0～511。 */
#define GPGPU_VM_INDEX_MASK ((1U << GPGPU_VM_INDEX_BITS) - 1U)
/* 第 level 级（0 为根）索引在 GPUVA 中的起始位：依次为 39、30、21、12。 */
#define GPGPU_VM_LEVEL_SHIFT(level) \
	(GPGPU_VM_PAGE_SHIFT + GPGPU_VM_INDEX_BITS * (GPGPU_VM_LEVELS - 1U - (level)))
/* GPUVA 有效位数，等于 ARM64 48 位配置下的用户地址位数。 */
#define GPGPU_VM_VA_BITS (GPGPU_VM_PAGE_SHIFT + GPGPU_VM_LEVELS * GPGPU_VM_INDEX_BITS)
/* 用户 GPUVA 上界（不含），即 2^48；位 63..48 必须为 0。 */
#define GPGPU_VM_VA_LIMIT (1ULL << GPGPU_VM_VA_BITS)
/* GPU 物理地址位数：VRAM、空洞与 Outbound 都在 4 GiB 内。 */
#define GPGPU_PHYS_ADDRESS_BITS 32U
/* GPU 物理地址上界（不含）。 */
#define GPGPU_PHYS_ADDRESS_LIMIT (1ULL << GPGPU_PHYS_ADDRESS_BITS)
/* 用户 VMID 数量；VMID 0 保留给管理域，用户使用 1、2。 */
#define GPGPU_VM_USERS 2U

/*
 * 页表项地址域 [31:12]：SYSTEM 为 1 时是 Host IOVA 页，设备经 Outbound
 * 访问 GPU 物理页“窗口起点 + IOVA”；为 0 时是 VRAM 偏移。位 63..32
 * 保留，有效项中出现即为页表错误。
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
