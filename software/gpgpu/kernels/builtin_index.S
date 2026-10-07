# SPDX-License-Identifier: GPL-2.0-or-later
# Regression kernel: out[flattened block/thread index] = index + gridDim.z.
# Exercises all xyz builtins and the transition to a second warp.
.include "gpgpu_kernel.inc"
builtin_index:
	lui t0, GPGPU_CTRL_HI
	lw t1, BLOCK_Z(t0)
	lw t2, GRID_DIM_Y(t0)
	mul t1, t1, t2
	lw t2, BLOCK_Y(t0)
	add t1, t1, t2
	lw t2, GRID_DIM_X(t0)
	mul t1, t1, t2
	lw t2, BLOCK_X(t0)
	add s0, t1, t2
	lw s1, BLOCK_DIM_X(t0)
	lw s2, BLOCK_DIM_Y(t0)
	lw s3, BLOCK_DIM_Z(t0)
	mul t1, s1, s2
	mul t1, t1, s3
	mul s0, s0, t1
	lw t1, THREAD_Z(t0)
	mul t1, t1, s2
	lw t2, THREAD_Y(t0)
	add t1, t1, t2
	mul t1, t1, s1
	lw t2, THREAD_X(t0)
	add t1, t1, t2
	add s0, s0, t1
	slli t1, s0, 2
	lw t2, 0(a0)
	add t2, t2, t1
	lw t3, GRID_DIM_Z(t0)
	add s0, s0, t3
	sw s0, 0(t2)
	ebreak
