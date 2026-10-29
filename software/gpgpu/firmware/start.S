/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CP 固件入口、异常入口与恢复型访存指令
 *
 * 复位向量指向 _start。异常使用独立栈，普通栈本身出错时也能报告致命错误。
 * cp_access_* 是仅有的几条可以触发并恢复 access fault 的指令，见 mem.c。
 */
.option norvc
.section .text.start,"ax"
.global _start
/* 1. 建立栈；2. 清零 BSS；3. 安装异常栈与入口，关闭中断；4. 进入主循环。 */
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

/*
 * 异常入口：换到异常栈，保存调用者保存寄存器，调用 cp_trap(mcause, mepc, mtval)，
 * 把返回值写入 mepc 后返回。不支持嵌套异常。
 */
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

/*
 * 恢复型 load：a0 为窗口地址，a1 为输出指针，成功返回 1。
 * 访存故障时 cp_trap() 把 PC 改到 cp_access_failed，返回 0。
 */
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
/* 恢复型 store：a0 为窗口地址，a1 为待写值；只执行一次，失败不重试。 */
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
/* 恢复点：以 a0 = 0 返回访存函数的调用者。 */
cp_access_failed:
	li a0, 0
	ret

.section .bss,"aw",@nobits
.balign 16
cp_trap_stack:
	.space 4096
cp_trap_stack_top:
