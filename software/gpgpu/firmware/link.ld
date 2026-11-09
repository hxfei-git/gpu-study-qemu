/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件链接脚本：地址经预处理从共享协议派生。
 *
 * 固件占 VRAM 末尾 4 MiB，由 Host 经 BAR2 装载；代码、只读数据、数据和
 * BSS 不得进入最后 64 KiB 的栈区。栈从 VRAM 上界向低地址增长。
 */
#include "gpgpu/gpgpu_cp_map.h"
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    firmware (rwx) : ORIGIN = GPGPU_FIRMWARE_BASE,
                     LENGTH = GPGPU_FIRMWARE_SIZE - GPGPU_FIRMWARE_STACK_SIZE
}
SECTIONS {
    . = ORIGIN(firmware);
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > firmware
    .rodata : { *(.rodata .rodata.*) } > firmware
    .data : { *(.data .data.*) *(.sdata .sdata.*) } > firmware
    . = ALIGN(8);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) } > firmware
    . = ALIGN(8);
    __bss_end = .;
    __stack_top = GPGPU_FIRMWARE_STACK_TOP;
    ASSERT(__bss_end <= GPGPU_FIRMWARE_STACK_BASE, "CP 固件超过固定保留范围")
    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
