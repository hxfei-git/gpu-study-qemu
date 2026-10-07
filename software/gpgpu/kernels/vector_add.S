# SPDX-License-Identifier: GPL-2.0-or-later
# __global__ void vector_add(const { u32 a, b, out, n; } *args)
.include "gpgpu_kernel.inc"
vector_add:
	lui t0, GPGPU_CTRL_HI
	lw t1, BLOCK_X(t0)
	lw t2, BLOCK_DIM_X(t0)
	mul t1, t1, t2
	lw t2, THREAD_X(t0)
	add t1, t1, t2
	lw t2, 12(a0)
	bgeu t1, t2, done
	slli t1, t1, 2
	lw t2, 0(a0)
	add t2, t2, t1
	flw f0, 0(t2)
	lw t2, 4(a0)
	add t2, t2, t1
	flw f1, 0(t2)
	fadd.s f2, f0, f1
	lw t2, 8(a0)
	add t2, t2, t1
	fsw f2, 0(t2)
done:
	ebreak
