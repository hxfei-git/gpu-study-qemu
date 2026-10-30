/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件链接脚本。固件占 VRAM 末尾 4 MiB（0x03c0_0000～0x03ff_ffff），
 * 由 Host 驱动经 BAR2 装载：代码、只读数据、数据和 BSS 从 0x03c0_0000
 * 起依次排列，不得进入最后 64 KiB 的栈区（0x03ff_0000 起）；栈顶为
 * VRAM 末尾 0x0400_0000。地址须与 gpgpu_cp_map.h 中的 GPGPU_FIRMWARE_*
 * 保持一致。
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { firmware (rwx) : ORIGIN = 0x03c00000, LENGTH = 0x003f0000 }
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
    __stack_top = 0x04000000;
    ASSERT(__bss_end <= 0x03ff0000, "CP 固件超过固定保留范围")
    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
