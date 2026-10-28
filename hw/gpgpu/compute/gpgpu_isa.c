// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU GPGPU：单个 lane 的 RV32 指令执行
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "gpgpu_isa.h"
#include "gpgpu_compute_internal.h"
#include "gpgpu_memory.h"

/* 启动状态和线程身份仅支持 CSRRS rs1=x0 读取。 */
#define CSR_THREAD_ID_X 0xCC0
#define CSR_BLOCK_ID_X 0xCC3
#define CSR_GRID_DIM_Z 0xCCB
#define CSR_MHARTID 0xF14

/* 先检查编号，即使目标为 x0 也不能跳过非法 CSR 的执行错误。 */
static bool isa_read_startup_csr(const GPGPUWarp *warp, const GPGPULane *lane,
			       uint32_t csr, uint32_t *value)
{
	if (csr >= CSR_THREAD_ID_X && csr < CSR_BLOCK_ID_X)
		*value = lane->startup_vgpr[csr - CSR_THREAD_ID_X];
	else if (csr >= CSR_BLOCK_ID_X && csr <= CSR_GRID_DIM_Z)
		*value = warp->startup_sgpr[csr - CSR_BLOCK_ID_X];
	else if (csr == CSR_MHARTID)
		*value = lane->mhartid;
	else
		return false;
	return true;
}

/*
 * isa_set_rounding_mode() - 设置本条浮点指令的舍入模式
 *
 * 解析动态 frm 并转换为 SoftFloat 编码；保留编码返回 false。
 */
static bool isa_set_rounding_mode(GPGPULane *lane, uint32_t rm)
{
	FloatRoundMode mode;

	if (rm == 7)
		rm = (lane->fcsr >> 5) & 7;

	switch (rm) {
	case 0:
		mode = float_round_nearest_even;
		break;
	case 1:
		mode = float_round_to_zero;
		break;
	case 2:
		mode = float_round_down;
		break;
	case 3:
		mode = float_round_up;
		break;
	case 4:
		mode = float_round_ties_away;
		break;
	default:
		return false;
	}

	set_float_rounding_mode(mode, &lane->fp_status);
	set_float_exception_flags(0, &lane->fp_status);
	return true;
}

/*
 * isa_update_fflags() - 累计本条指令产生的浮点异常
 *
 * 将 SoftFloat 标志转换到 RISC-V fflags 位，保留此前异常。
 */
static void isa_update_fflags(GPGPULane *lane)
{
	int flags = get_float_exception_flags(&lane->fp_status);

	lane->fcsr |= ((flags & float_flag_inexact) ? 1U << 0 : 0) |
		((flags & float_flag_underflow) ? 1U << 1 : 0) |
		((flags & float_flag_overflow) ? 1U << 2 : 0) |
		((flags & float_flag_divbyzero) ? 1U << 3 : 0) |
		((flags & float_flag_invalid) ? 1U << 4 : 0);
}

/*
 * isa_float32_to_e2m1() - 将 FP32 饱和转换为 E2M1
 *
 * 按正数编码查找相邻值和中点，结合符号及舍入模式确定结果与异常。
 */
static float4_e2m1 isa_float32_to_e2m1(float32 value, float_status *status)
{
	static const uint32_t values[] = {
		0x00000000,
		0x3F000000,
		0x3F800000,
		0x3FC00000,
		0x40000000,
		0x40400000,
		0x40800000,
		0x40C00000,
		/* 用于溢出舍入的虚拟后继值 8.0。 */
		0x41000000,
	};
	static const uint32_t midpoints[] = {
		0x3E800000, 0x3F400000, 0x3FA00000, 0x3FE00000,
		0x40200000, 0x40600000, 0x40A00000, 0x40E00000,
	};
	uint32_t magnitude = value & 0x7FFFFFFF;
	uint32_t sign = (value >> 28) & 8;
	uint32_t upper = 0;
	uint32_t result;
	uint16_t flags = float_flag_inexact;
	bool round_up;

	/* E2M1 没有 NaN 或无穷编码；饱和时保留输入符号。 */
	if (magnitude >= 0x7F800000) {
		if (magnitude > 0x7F800000)
			float_raise(float_flag_invalid, status);
		return sign | 7;
	}
	if (magnitude >= values[8]) {
		float_raise(float_flag_overflow | float_flag_inexact, status);
		return sign | 7;
	}

	while (magnitude > values[upper])
		upper++;
	if (magnitude == values[upper])
		return sign | upper;

	switch (get_float_rounding_mode(status)) {
	case float_round_nearest_even:
		round_up = magnitude > midpoints[upper - 1] ||
			(magnitude == midpoints[upper - 1] && !(upper & 1));
		break;
	case float_round_ties_away:
		round_up = magnitude >= midpoints[upper - 1];
		break;
	case float_round_to_zero:
		round_up = false;
		break;
	case float_round_down:
		round_up = sign != 0;
		break;
	case float_round_up:
		round_up = sign == 0;
		break;
	default:
		g_assert_not_reached();
	}

	result = upper - 1 + round_up;
	if (result > 7) {
		result = 7;
		flags |= float_flag_overflow;
	} else if (result < 2) {
		/* 与其他浮点格式一致，在舍入后判断下溢。 */
		flags |= float_flag_underflow;
	}
	float_raise(flags, status);
	return sign | result;
}

/* 取指和访存错误写入 ctx->result，由 Core 原样交给 Dispatch。 */
bool gpgpu_isa_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id)
{
	GPGPULane *lane = &warp->lanes[lane_id];
	uint32_t inst;
	uint32_t opcode;
	uint32_t rd;
	uint32_t funct3;
	uint32_t rs1;
	uint32_t rs2;
	uint32_t funct7;
	uint32_t csr;

	if (!gpgpu_memory_fetch(ctx, lane->pc, &inst))
		return false;
	opcode = inst & 0x7F;
	rd = (inst >> 7) & 0x1F;
	funct3 = (inst >> 12) & 0x7;
	rs1 = (inst >> 15) & 0x1F;
	rs2 = (inst >> 20) & 0x1F;
	funct7 = inst >> 25;
	csr = inst >> 20;

	switch (opcode) {
	case 0x73: /* SYSTEM */
		switch (funct3) {
		case 0x0: /* EBREAK */
			if (inst != 0x00100073)
				return gpgpu_exec_fail(ctx, 0);
			lane->active = false;
			warp->active_mask &= ~(1U << lane_id);
			break;

		case 0x2: { /* CSRRS */
			uint32_t value;

			/* rs1 按寄存器编码判断，其他寄存器即使保存零也属于写请求。 */
			if (rs1 != 0 || !isa_read_startup_csr(warp, lane, csr, &value))
				return gpgpu_exec_fail(ctx, 0);
			if (rd != 0)
				lane->gpr[rd] = value;
			lane->pc += 4;
			break;
		}

		default:
			return gpgpu_exec_fail(ctx, 0);
		}
		break;

	case 0x13: /* OP-IMM */
		switch (funct3) {
		case 0x0: { /* ADDI */
			int32_t imm = (int32_t)inst >> 20;

			if (rd != 0)
				lane->gpr[rd] = lane->gpr[rs1] + (uint32_t)imm;
			lane->pc += 4;
			break;
		}

		case 0x1: { /* SLLI */
			uint32_t shamt = (inst >> 20) & 0x1F;

			if (funct7 != 0)
				return gpgpu_exec_fail(ctx, 0);
			if (rd != 0)
				lane->gpr[rd] = lane->gpr[rs1] << shamt;
			lane->pc += 4;
			break;
		}

		case 0x7: { /* ANDI */
			int32_t imm = (int32_t)inst >> 20;

			if (rd != 0)
				lane->gpr[rd] = lane->gpr[rs1] & (uint32_t)imm;
			lane->pc += 4;
			break;
		}

		default:
			return gpgpu_exec_fail(ctx, 0);
		}
		break;

	case 0x37: /* LUI */
		if (rd != 0)
			lane->gpr[rd] = inst & 0xFFFFF000;
		lane->pc += 4;
		break;

	case 0x33: { /* ADD / SUB / MUL */
		uint32_t value;

		if (funct3 != 0)
			return gpgpu_exec_fail(ctx, 0);
		switch (funct7) {
		case 0x00:
			value = lane->gpr[rs1] + lane->gpr[rs2];
			break;
		case 0x20:
			value = lane->gpr[rs1] - lane->gpr[rs2];
			break;
		case 0x01:
			/* 有无符号 MUL 的乘积低半部相同。 */
			value = lane->gpr[rs1] * lane->gpr[rs2];
			break;
		default:
			return gpgpu_exec_fail(ctx, 0);
		}
		if (rd != 0)
			lane->gpr[rd] = value;
		lane->pc += 4;
		break;
	}

	case 0x03:   /* LW */
	case 0x07: { /* FLW */
		int32_t offset = (int32_t)inst >> 20;
		uint32_t addr = lane->gpr[rs1] + (uint32_t)offset;
		uint32_t value;

		if (funct3 != 2)
			return gpgpu_exec_fail(ctx, 0);
		if (!gpgpu_memory_load_word(ctx, addr, &value))
			return false;
		if (opcode == 0x07)
			lane->fpr[rd] = value;
		else if (rd != 0)
			lane->gpr[rd] = value;
		lane->pc += 4;
		break;
	}

	case 0x63: { /* 条件分支；每个 lane 保存各自的 PC。 */
		uint32_t raw_imm = ((inst >> 31) << 12) | (((inst >> 7) & 1) << 11) |
			(((inst >> 25) & 0x3F) << 5) | (((inst >> 8) & 0xF) << 1);
		int32_t offset = (int32_t)(raw_imm << 19) >> 19;
		uint32_t left = lane->gpr[rs1];
		uint32_t right = lane->gpr[rs2];
		bool taken;

		switch (funct3) {
		case 0:
			taken = left == right;
			break;
		case 1:
			taken = left != right;
			break;
		case 4:
			taken = (int32_t)left < (int32_t)right;
			break;
		case 5:
			taken = (int32_t)left >= (int32_t)right;
			break;
		case 6:
			taken = left < right;
			break;
		case 7:
			taken = left >= right;
			break;
		default:
			return gpgpu_exec_fail(ctx, 0);
		}
		lane->pc += taken ? (uint32_t)offset : 4;
		break;
	}

	case 0x6F: { /* JAL */
		uint32_t raw_imm = ((inst >> 31) << 20) | (((inst >> 12) & 0xFF) << 12) |
			(((inst >> 20) & 1) << 11) | (((inst >> 21) & 0x3FF) << 1);
		int32_t offset = (int32_t)(raw_imm << 11) >> 11;

		if (rd != 0)
			lane->gpr[rd] = lane->pc + 4;
		lane->pc += (uint32_t)offset;
		break;
	}

	case 0x23: /* STORE */
	case 0x27: /* FSW */
		switch (funct3) {
		case 0x2: { /* SW */
			uint32_t raw_imm = (((inst >> 25) & 0x7F) << 5) | ((inst >> 7) & 0x1F);
			int32_t offset = (int32_t)(raw_imm << 20) >> 20;
			uint32_t addr = lane->gpr[rs1] + (uint32_t)offset;
			uint32_t value = opcode == 0x27 ? lane->fpr[rs2] : lane->gpr[rs2];

			if (!gpgpu_memory_store_word(ctx, addr, value))
				return false;
			lane->pc += 4;
			break;
		}

		default:
			return gpgpu_exec_fail(ctx, 0);
		}
		break;

	case 0x53:		      /* OP-FP */
		if (funct7 == 0x78) { /* FMV.W.X */
			if (rs2 != 0 || funct3 != 0)
				return gpgpu_exec_fail(ctx, 0);
			lane->fpr[rd] = lane->gpr[rs1];
			lane->pc += 4;
			break;
		}

		switch (funct7) {
		case 0x00: /* FADD.S */
		case 0x08: /* FMUL.S */
			break;
		case 0x22: /* BF16 格式转换 */
		case 0x26: /* E2M1 格式转换 */
			if (rs2 > 1)
				return gpgpu_exec_fail(ctx, 0);
			break;
		case 0x24: /* E4M3 / E5M2 格式转换 */
			if (rs2 > 3)
				return gpgpu_exec_fail(ctx, 0);
			break;
		case 0x60: /* FCVT.W.S */
		case 0x68: /* FCVT.S.W */
			if (rs2 != 0)
				return gpgpu_exec_fail(ctx, 0);
			break;
		default:
			return gpgpu_exec_fail(ctx, 0);
		}

		if (!isa_set_rounding_mode(lane, funct3))
			return gpgpu_exec_fail(ctx, 0);

		switch (funct7) {
		case 0x00: /* FADD.S */
			lane->fpr[rd] =
				float32_add(lane->fpr[rs1], lane->fpr[rs2], &lane->fp_status);
			break;
		case 0x08: /* FMUL.S */
			lane->fpr[rd] =
				float32_mul(lane->fpr[rs1], lane->fpr[rs2], &lane->fp_status);
			break;
		case 0x22: /* BF16 格式转换 */
			if (rs2 == 0) {
				lane->fpr[rd] = bfloat16_to_float32((bfloat16)lane->fpr[rs1],
								    &lane->fp_status);
			} else {
				lane->fpr[rd] =
					float32_to_bfloat16(lane->fpr[rs1], &lane->fp_status);
			}
			break;
		case 0x24: /* E4M3 / E5M2 格式转换 */
			if (rs2 == 0 || rs2 == 2) {
				bfloat16 value;

				if (rs2 == 0) {
					value = float8_e4m3_to_bfloat16((float8_e4m3)lane->fpr[rs1],
									&lane->fp_status);
				} else {
					value = float8_e5m2_to_bfloat16((float8_e5m2)lane->fpr[rs1],
									&lane->fp_status);
				}
				lane->fpr[rd] = bfloat16_to_float32(value, &lane->fp_status);
			} else if (rs2 == 1) {
				float32 value = lane->fpr[rs1];
				float8_e4m3 result =
					float32_to_float8_e4m3(value, true, &lane->fp_status);

				/*
				 * E4M3 的 NaN 编码要求指数与尾数位全部置位。
				 */
				lane->fpr[rd] = float32_is_any_nan(value) ? 0x7F : result;
			} else {
				/*
				 * 有限值溢出时饱和；输入本来是无穷时保留无穷编码。
				 */
				lane->fpr[rd] = float32_to_float8_e5m2(
					lane->fpr[rs1], !float32_is_infinity(lane->fpr[rs1]),
					&lane->fp_status);
			}
			break;
		case 0x26: /* E2M1 格式转换 */
			if (rs2 == 0) {
				float8_e4m3 value = float4_e2m1_to_float8_e4m3(lane->fpr[rs1] & 0xF,
									       &lane->fp_status);
				bfloat16 bf16 = float8_e4m3_to_bfloat16(value, &lane->fp_status);

				lane->fpr[rd] = bfloat16_to_float32(bf16, &lane->fp_status);
			} else {
				lane->fpr[rd] =
					isa_float32_to_e2m1(lane->fpr[rs1], &lane->fp_status);
			}
			break;
		case 0x60: { /* FCVT.W.S */
			int32_t value = float32_to_int32(lane->fpr[rs1], &lane->fp_status);

			if (rd != 0)
				lane->gpr[rd] = (uint32_t)value;
			break;
		}
		case 0x68: /* FCVT.S.W */
			lane->fpr[rd] = int32_to_float32((int32_t)lane->gpr[rs1], &lane->fp_status);
			break;
		}

		isa_update_fflags(lane);
		lane->pc += 4;
		break;

	default:
		return gpgpu_exec_fail(ctx, 0);
	}
	return true;
}
