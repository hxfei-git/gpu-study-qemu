# SPDX-License-Identifier: GPL-2.0-or-later
# __global__ void matmul(const { u32 a, b, out, rows, cols, inner; } *args)
# 矩阵按行存储：A[rows][inner]、B[inner][cols]、C[rows][cols]。
.include "gpgpu_kernel.inc"
matmul:
	lui t0, GPGPU_CTRL_HI
	lw t1, BLOCK_X(t0)
	lw t2, BLOCK_DIM_X(t0)
	mul t1, t1, t2
	lw t2, THREAD_X(t0)
	add s0, t1, t2
	lw t1, BLOCK_Y(t0)
	lw t2, BLOCK_DIM_Y(t0)
	mul t1, t1, t2
	lw t2, THREAD_Y(t0)
	add s1, t1, t2
	lw s2, 16(a0)
	bgeu s0, s2, done
	lw t1, 12(a0)
	bgeu s1, t1, done
	lw s3, 20(a0)
	lw s4, 0(a0)
	mul t1, s1, s3
	slli t1, t1, 2
	add s4, s4, t1
	lw s5, 4(a0)
	slli t1, s0, 2
	add s5, s5, t1
	slli s6, s2, 2
	addi s7, zero, 0
# 设备初始化每个线程的浮点寄存器，因此入口处 f0 为 +0。
loop:
	bgeu s7, s3, store
	flw f1, 0(s4)
	flw f2, 0(s5)
	fmul.s f3, f1, f2
	fadd.s f0, f0, f3
	addi s4, s4, 4
	add s5, s5, s6
	addi s7, s7, 1
	jal zero, loop
store:
	mul t1, s1, s2
	add t1, t1, s0
	slli t1, t1, 2
	lw t2, 8(a0)
	add t2, t2, t1
	fsw f0, 0(t2)
done:
	ebreak
