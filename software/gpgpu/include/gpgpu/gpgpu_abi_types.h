/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU 共享协议：基本类型与布局检查
 *
 * 本目录下的协议头同时被 QEMU 设备模型、RV64 CP 固件、Linux 驱动和
 * 用户态库包含，因此只使用编译器内建类型，不依赖任何一方的头文件。
 * 所有多字节字段均为小端。
 */
#ifndef GPGPU_ABI_TYPES_H
#define GPGPU_ABI_TYPES_H

typedef __UINT16_TYPE__ gpgpu_q_u16;
typedef __UINT32_TYPE__ gpgpu_q_u32;
typedef __UINT64_TYPE__ gpgpu_q_u64;
typedef __INT64_TYPE__ gpgpu_q_s64;

/*
 * GPGPU_LAYOUT_ASSERT() - 编译期核对共享对象的大小、对齐和关键字段偏移
 *
 * 四方各自编译同一份布局，任何一方改动都会在编译时暴露。
 */
#define GPGPU_LAYOUT_ASSERT(type, size, member, offset)             \
	_Static_assert(sizeof(type) == (size), #type " size");      \
	_Static_assert(__alignof__(type) >= 8, #type " alignment"); \
	_Static_assert(__builtin_offsetof(type, member) == (offset), #type "." #member " offset")

#endif /* GPGPU_ABI_TYPES_H */
