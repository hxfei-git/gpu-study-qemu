# SPDX-License-Identifier: GPL-2.0-or-later
# __global__ void relu(const { u32 input, out, n; } *args)
# For finite FP32 inputs, the sign bit determines whether to emit +0.
.include "gpgpu_kernel.inc"
relu:
	lui t0, GPGPU_CTRL_HI
	lw t1, BLOCK_X(t0)
	lw t2, BLOCK_DIM_X(t0)
	mul t1, t1, t2
	lw t2, THREAD_X(t0)
	add t1, t1, t2
	lw t2, 8(a0)
	bgeu t1, t2, done
	slli t1, t1, 2
	lw t2, 0(a0)
	add t2, t2, t1
	lw t3, 0(t2)
	blt t3, zero, negative
	jal zero, store
negative:
	addi t3, zero, 0
store:
	lw t2, 4(a0)
	add t2, t2, t1
	sw t3, 0(t2)
done:
	ebreak
