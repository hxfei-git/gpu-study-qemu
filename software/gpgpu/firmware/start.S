/* SPDX-License-Identifier: GPL-2.0-or-later */
.option norvc
.section .text.start,"ax"
.global _start
/* 建立普通栈、清零 BSS 并安装异常入口，随后进入不返回的 CP 主循环。 */
_start:
	la sp, __stack_top
	la t0, __bss_start
	la t1, __bss_end
1:
	bgeu t0, t1, 2f
	sd zero, 0(t0)
	addi t0, t0, 8
	j 1b
2:
	la t0, cp_trap_stack_top
	csrw mscratch, t0
	la t0, cp_trap_entry
	csrw mtvec, t0
	csrw mie, zero
	call cp_main
	j .

/* 单独异常栈使普通栈访存异常也能留下设备错误；不允许嵌套异常。 */
.align 2
cp_trap_entry:
	csrrw sp, mscratch, sp
	addi sp, sp, -144
	sd ra, 0(sp)
	sd t0, 8(sp)
	sd t1, 16(sp)
	sd t2, 24(sp)
	sd t3, 32(sp)
	sd t4, 40(sp)
	sd t5, 48(sp)
	sd t6, 56(sp)
	sd a0, 64(sp)
	sd a1, 72(sp)
	sd a2, 80(sp)
	sd a3, 88(sp)
	sd a4, 96(sp)
	sd a5, 104(sp)
	sd a6, 112(sp)
	sd a7, 120(sp)
	csrr a0, mcause
	csrr a1, mepc
	csrr a2, mtval
	call cp_trap
	csrw mepc, a0
	ld ra, 0(sp)
	ld t0, 8(sp)
	ld t1, 16(sp)
	ld t2, 24(sp)
	ld t3, 32(sp)
	ld t4, 40(sp)
	ld t5, 48(sp)
	ld t6, 56(sp)
	ld a0, 64(sp)
	ld a1, 72(sp)
	ld a2, 80(sp)
	ld a3, 88(sp)
	ld a4, 96(sp)
	ld a5, 104(sp)
	ld a6, 112(sp)
	ld a7, 120(sp)
	addi sp, sp, 144
	csrrw sp, mscratch, sp
	mret

/* 仅这四条 Host 窗口指令可恢复；其他异常全部停止固件调度。 */
/* a0 为窗口地址、a1 为输出指针；成功返回 1，访存异常转失败入口。 */
.macro load_access name, instruction, store
.global cp_access_\name, cp_fault_\name
cp_access_\name:
cp_fault_\name:
	\instruction a2, 0(a0)
	\store a2, 0(a1)
	li a0, 1
	ret
.endm
load_access load32, lwu, sw
load_access load64, ld, sd
/* a0 为窗口地址、a1 为待写值；只执行一次 store，失败不重试。 */
.macro store_access name, instruction
.global cp_access_\name, cp_fault_\name
cp_access_\name:
cp_fault_\name:
	\instruction a1, 0(a0)
	li a0, 1
	ret
.endm
store_access store32, sw
store_access store64, sd
.global cp_access_failed
/* 陷阱处理器将已知访存故障的 PC 改到这里，以 a0=0 返回调用者。 */
cp_access_failed:
	li a0, 0
	ret

.section .bss,"aw",@nobits
.balign 16
cp_trap_stack:
	.space 4096
cp_trap_stack_top:
