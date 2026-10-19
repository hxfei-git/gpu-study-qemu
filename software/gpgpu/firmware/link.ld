/* SPDX-License-Identifier: GPL-2.0-or-later */
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
