# SPDX-License-Identifier: GPL-2.0-or-later
# __global__ void relu(const { u32 input, out, n; } *args)
# 对有限 FP32 输入，根据符号位决定是否输出 +0。
.include "gpgpu_kernel.inc"
relu:
	csrr t1, BLOCK_ID_X
	csrr t2, BLOCK_DIM_X
	mul t1, t1, t2
	csrr t2, THREAD_ID_X
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
