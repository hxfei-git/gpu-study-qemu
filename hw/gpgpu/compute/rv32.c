// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RV32 解释器：解码与按类别执行
 *
 * 每条指令先由 rv32_decode() 拆出字段和立即数，再由 gpgpu_rv32_step() 按
 * opcode 分派到一个类别函数。类别函数只做两件事：检查本类编码是否合法，
 * 合法则执行并推进 PC。非法编码统一返回 GPGPU_EXEC_ERR_INSTRUCTION。
 */
#include "qemu/osdep.h"
#include "compute_internal.h"
#include "lane_mem.h"
#include "rv32.h"

/* RV32 主 opcode。 */
enum {
	OP_LOAD = 0x03,
	OP_LOAD_FP = 0x07,
	OP_IMM = 0x13,
	OP_STORE = 0x23,
	OP_STORE_FP = 0x27,
	OP_REG = 0x33,
	OP_LUI = 0x37,
	OP_FP = 0x53,
	OP_BRANCH = 0x63,
	OP_JAL = 0x6f,
	OP_SYSTEM = 0x73,
};

/* EBREAK 的完整编码。 */
#define INSN_EBREAK 0x00100073u

/* OP-FP 的 funct7。 */
enum {
	FP_ADD_S = 0x00,
	FP_MUL_S = 0x08,
	FP_CVT_BF16 = 0x22,
	FP_CVT_FP8 = 0x24,
	FP_CVT_E2M1 = 0x26,
	FP_CVT_W_S = 0x60,
	FP_CVT_S_W = 0x68,
	FP_MV_W_X = 0x78,
};

/* 只读 CSR：threadIdx 0xcc0～0xcc2；blockIdx、blockDim、gridDim 0xcc3～0xccb。 */
#define CSR_THREAD_ID_X 0xcc0
#define CSR_STARTUP_SGPR_FIRST 0xcc3
#define CSR_STARTUP_SGPR_LAST 0xccb
#define CSR_MHARTID 0xf14

/* fcsr 中 frm 字段的起始位；rm=7 表示使用 frm。 */
#define FCSR_FRM_SHIFT 5
#define RM_DYNAMIC 7

/**
 * struct Rv32Insn - 解码后的指令字段
 * @raw: 原始指令字。
 * @opcode: [6:0]。
 * @rd: [11:7]。
 * @funct3: [14:12]。
 * @rs1: [19:15]。
 * @rs2: [24:20]。
 * @funct7: [31:25]。
 * @imm_i: I 型符号扩展立即数，也用作 CSR 编号的来源。
 * @imm_s: S 型符号扩展立即数。
 * @imm_b: B 型符号扩展分支偏移。
 * @imm_j: J 型符号扩展跳转偏移。
 */
typedef struct Rv32Insn {
	uint32_t raw;
	uint32_t opcode;
	uint32_t rd;
	uint32_t funct3;
	uint32_t rs1;
	uint32_t rs2;
	uint32_t funct7;
	int32_t imm_i;
	int32_t imm_s;
	int32_t imm_b;
	int32_t imm_j;
} Rv32Insn;

static Rv32Insn rv32_decode(uint32_t raw)
{
	uint32_t b = ((raw >> 31) << 12) | (((raw >> 7) & 1) << 11) |
		(((raw >> 25) & 0x3f) << 5) | (((raw >> 8) & 0xf) << 1);
	uint32_t j = ((raw >> 31) << 20) | (((raw >> 12) & 0xff) << 12) |
		(((raw >> 20) & 1) << 11) | (((raw >> 21) & 0x3ff) << 1);
	uint32_t s = ((raw >> 25) << 5) | ((raw >> 7) & 0x1f);

	return (Rv32Insn) {
		.raw = raw,
		.opcode = raw & 0x7f,
		.rd = (raw >> 7) & 0x1f,
		.funct3 = (raw >> 12) & 0x7,
		.rs1 = (raw >> 15) & 0x1f,
		.rs2 = (raw >> 20) & 0x1f,
		.funct7 = raw >> 25,
		.imm_i = (int32_t)raw >> 20,
		.imm_s = (int32_t)(s << 20) >> 20,
		.imm_b = (int32_t)(b << 19) >> 19,
		.imm_j = (int32_t)(j << 11) >> 11,
	};
}

/* 写整数寄存器，x0 保持为 0。 */
static void set_gpr(GPGPULane *lane, uint32_t rd, uint32_t value)
{
	if (rd)
		lane->gpr[rd] = value;
}

static bool illegal(GPGPUExecContext *ctx)
{
	return gpgpu_exec_fail(ctx, GPGPU_EXEC_ERR_INSTRUCTION);
}

/* ---- 系统指令 ---- */

/* 读取只读 CSR：先核对编号，rd 为 x0 时也不能跳过非法编号的错误。 */
static bool read_csr(const GPGPUWarp *warp, const GPGPULane *lane, uint32_t csr, uint32_t *value)
{
	if (csr >= CSR_THREAD_ID_X && csr < CSR_STARTUP_SGPR_FIRST)
		*value = lane->thread_id[csr - CSR_THREAD_ID_X];
	else if (csr >= CSR_STARTUP_SGPR_FIRST && csr <= CSR_STARTUP_SGPR_LAST)
		*value = warp->startup_sgpr[csr - CSR_STARTUP_SGPR_FIRST];
	else if (csr == CSR_MHARTID)
		*value = lane->mhartid;
	else
		return false;
	return true;
}

/*
 * EBREAK：清除 lane 的活跃标记。
 * CSRRS：只接受 rs1=x0 的只读形式；rs1 按编码判断，即使其他寄存器值为 0
 * 也属于写请求。
 */
static bool exec_system(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id,
			const Rv32Insn *in)
{
	GPGPULane *lane = &warp->lanes[lane_id];
	uint32_t value;

	switch (in->funct3) {
	case 0:
		if (in->raw != INSN_EBREAK)
			return illegal(ctx);
		lane->active = false;
		warp->active_mask &= ~(1u << lane_id);
		return true;
	case 2:
		if (in->rs1 || !read_csr(warp, lane, (uint32_t)in->imm_i & 0xfff, &value))
			return illegal(ctx);
		set_gpr(lane, in->rd, value);
		lane->pc += 4;
		return true;
	default:
		return illegal(ctx);
	}
}

/* ---- 整数指令 ---- */

/* OP-IMM：ADDI、SLLI、ANDI。 */
static bool exec_imm(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	uint32_t src = lane->gpr[in->rs1];
	uint32_t value;

	switch (in->funct3) {
	case 0:
		value = src + (uint32_t)in->imm_i;
		break;
	case 1:
		if (in->funct7)
			return illegal(ctx);
		value = src << in->rs2;
		break;
	case 7:
		value = src & (uint32_t)in->imm_i;
		break;
	default:
		return illegal(ctx);
	}
	set_gpr(lane, in->rd, value);
	lane->pc += 4;
	return true;
}

/* OP：ADD、SUB、MUL（有无符号的乘积低 32 位相同）。 */
static bool exec_reg(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	uint32_t a = lane->gpr[in->rs1];
	uint32_t b = lane->gpr[in->rs2];
	uint32_t value;

	if (in->funct3)
		return illegal(ctx);
	switch (in->funct7) {
	case 0x00:
		value = a + b;
		break;
	case 0x20:
		value = a - b;
		break;
	case 0x01:
		value = a * b;
		break;
	default:
		return illegal(ctx);
	}
	set_gpr(lane, in->rd, value);
	lane->pc += 4;
	return true;
}

/* 条件分支：每个 lane 独立保存 PC，不同 lane 可以走不同分支。 */
static bool exec_branch(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	uint32_t a = lane->gpr[in->rs1];
	uint32_t b = lane->gpr[in->rs2];
	bool taken;

	switch (in->funct3) {
	case 0:
		taken = a == b;
		break;
	case 1:
		taken = a != b;
		break;
	case 4:
		taken = (int32_t)a < (int32_t)b;
		break;
	case 5:
		taken = (int32_t)a >= (int32_t)b;
		break;
	case 6:
		taken = a < b;
		break;
	case 7:
		taken = a >= b;
		break;
	default:
		return illegal(ctx);
	}
	lane->pc += taken ? (uint32_t)in->imm_b : 4;
	return true;
}

/* ---- 访存指令 ---- */

/* LW/FLW：只支持 32 位字（funct3=2）。 */
static bool exec_load(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	uint32_t value;

	if (in->funct3 != 2)
		return illegal(ctx);
	if (!gpgpu_lane_load(ctx, lane->gpr[in->rs1] + (uint32_t)in->imm_i, &value))
		return false;
	if (in->opcode == OP_LOAD_FP)
		lane->fpr[in->rd] = value;
	else
		set_gpr(lane, in->rd, value);
	lane->pc += 4;
	return true;
}

/* SW/FSW：只支持 32 位字（funct3=2）。 */
static bool exec_store(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	uint32_t value = in->opcode == OP_STORE_FP ? lane->fpr[in->rs2] : lane->gpr[in->rs2];

	if (in->funct3 != 2)
		return illegal(ctx);
	if (!gpgpu_lane_store(ctx, lane->gpr[in->rs1] + (uint32_t)in->imm_s, value))
		return false;
	lane->pc += 4;
	return true;
}

/* ---- 浮点指令 ---- */

/* 设置本条指令的舍入模式（rm=7 时取 frm）并清除异常标志；保留编码返回 false。 */
static bool fp_set_rounding(GPGPULane *lane, uint32_t rm)
{
	static const FloatRoundMode modes[] = {
		float_round_nearest_even, float_round_to_zero, float_round_down,
		float_round_up,		  float_round_ties_away,
	};

	if (rm == RM_DYNAMIC)
		rm = (lane->fcsr >> FCSR_FRM_SHIFT) & 7;
	if (rm >= ARRAY_SIZE(modes))
		return false;
	set_float_rounding_mode(modes[rm], &lane->fp_status);
	set_float_exception_flags(0, &lane->fp_status);
	return true;
}

/* 把本条指令的 SoftFloat 异常累加到 fflags。 */
static void fp_accumulate_flags(GPGPULane *lane)
{
	int flags = get_float_exception_flags(&lane->fp_status);

	lane->fcsr |= ((flags & float_flag_inexact) ? 1u << 0 : 0) |
		((flags & float_flag_underflow) ? 1u << 1 : 0) |
		((flags & float_flag_overflow) ? 1u << 2 : 0) |
		((flags & float_flag_divbyzero) ? 1u << 3 : 0) |
		((flags & float_flag_invalid) ? 1u << 4 : 0);
}

/*
 * fp32_to_e2m1() - FP32 饱和转换为 E2M1（FP4）
 *
 * E2M1 只有 8 个非负值且没有 NaN/无穷：NaN 与无穷饱和为同号最大值；其余值
 * 先找相邻的两个可表示值，再按舍入模式决定取上或取下。
 */
static float4_e2m1 fp32_to_e2m1(float32 value, float_status *status)
{
	/* E2M1 的 8 个非负值，外加用于判断溢出的虚拟后继 8.0。 */
	static const uint32_t levels[] = {
		0x00000000, 0x3f000000, 0x3f800000, 0x3fc00000, 0x40000000,
		0x40400000, 0x40800000, 0x40c00000, 0x41000000,
	};
	/* 相邻两级之间的中点，用于就近舍入。 */
	static const uint32_t midpoints[] = {
		0x3e800000, 0x3f400000, 0x3fa00000, 0x3fe00000,
		0x40200000, 0x40600000, 0x40a00000, 0x40e00000,
	};
	uint32_t magnitude = value & 0x7fffffff;
	uint32_t sign = (value >> 28) & 8;
	uint16_t flags = float_flag_inexact;
	uint32_t upper = 0;
	uint32_t result;
	bool round_up;

	if (magnitude >= 0x7f800000) {
		if (magnitude > 0x7f800000)
			float_raise(float_flag_invalid, status);
		return sign | 7;
	}
	if (magnitude >= levels[8]) {
		float_raise(float_flag_overflow | float_flag_inexact, status);
		return sign | 7;
	}
	while (magnitude > levels[upper])
		upper++;
	if (magnitude == levels[upper])
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
		/* 与其他浮点格式一致，舍入后判断下溢。 */
		flags |= float_flag_underflow;
	}
	float_raise(flags, status);
	return sign | result;
}

/* FP8 转换：rs2=0 E4M3→FP32，1 FP32→E4M3，2 E5M2→FP32，3 FP32→E5M2。 */
static uint32_t fp_convert_fp8(GPGPULane *lane, uint32_t src, uint32_t rs2)
{
	float_status *st = &lane->fp_status;
	bfloat16 wide;

	switch (rs2) {
	case 0:
		wide = float8_e4m3_to_bfloat16((float8_e4m3)src, st);
		return bfloat16_to_float32(wide, st);
	case 1:
		/* E4M3 的 NaN 编码要求指数与尾数位全为 1。 */
		return float32_is_any_nan(src) ? 0x7f : float32_to_float8_e4m3(src, true, st);
	case 2:
		wide = float8_e5m2_to_bfloat16((float8_e5m2)src, st);
		return bfloat16_to_float32(wide, st);
	default:
		/* 有限值溢出时饱和；输入本身为无穷时保留无穷编码。 */
		return float32_to_float8_e5m2(src, !float32_is_infinity(src), st);
	}
}

/* E2M1 转换：rs2=0 E2M1→FP32（经 E4M3、BF16 扩展），1 FP32→E2M1。 */
static uint32_t fp_convert_e2m1(GPGPULane *lane, uint32_t src, uint32_t rs2)
{
	float_status *st = &lane->fp_status;
	float8_e4m3 e4m3;

	if (rs2)
		return fp32_to_e2m1(src, st);
	e4m3 = float4_e2m1_to_float8_e4m3(src & 0xf, st);
	return bfloat16_to_float32(float8_e4m3_to_bfloat16(e4m3, st), st);
}

/* OP-FP 各 funct7 允许的 rs2 上限；不在表中的 funct7 非法。 */
static bool fp_rs2_valid(uint32_t funct7, uint32_t rs2)
{
	switch (funct7) {
	case FP_ADD_S:
	case FP_MUL_S:
		return true;
	case FP_CVT_BF16:
	case FP_CVT_E2M1:
		return rs2 <= 1;
	case FP_CVT_FP8:
		return rs2 <= 3;
	case FP_CVT_W_S:
	case FP_CVT_S_W:
		return rs2 == 0;
	default:
		return false;
	}
}

/*
 * exec_fp() - OP-FP
 *
 * 1. FMV.W.X 只搬运位，不涉及舍入；
 * 2. 其余指令先检查 rs2 与舍入模式；
 * 3. 计算并写回；4. 累加异常标志。
 */
static bool exec_fp(GPGPUExecContext *ctx, GPGPULane *lane, const Rv32Insn *in)
{
	float_status *st = &lane->fp_status;
	uint32_t a = lane->fpr[in->rs1];

	/* 1. 位搬运。 */
	if (in->funct7 == FP_MV_W_X) {
		if (in->rs2 || in->funct3)
			return illegal(ctx);
		lane->fpr[in->rd] = lane->gpr[in->rs1];
		lane->pc += 4;
		return true;
	}

	/* 2. 编码检查。 */
	if (!fp_rs2_valid(in->funct7, in->rs2) || !fp_set_rounding(lane, in->funct3))
		return illegal(ctx);

	/* 3. 计算。 */
	switch (in->funct7) {
	case FP_ADD_S:
		lane->fpr[in->rd] = float32_add(a, lane->fpr[in->rs2], st);
		break;
	case FP_MUL_S:
		lane->fpr[in->rd] = float32_mul(a, lane->fpr[in->rs2], st);
		break;
	case FP_CVT_BF16:
		lane->fpr[in->rd] = in->rs2 ? float32_to_bfloat16(a, st) :
					      bfloat16_to_float32((bfloat16)a, st);
		break;
	case FP_CVT_FP8:
		lane->fpr[in->rd] = fp_convert_fp8(lane, a, in->rs2);
		break;
	case FP_CVT_E2M1:
		lane->fpr[in->rd] = fp_convert_e2m1(lane, a, in->rs2);
		break;
	case FP_CVT_W_S:
		set_gpr(lane, in->rd, (uint32_t)float32_to_int32(a, st));
		break;
	case FP_CVT_S_W:
		lane->fpr[in->rd] = int32_to_float32((int32_t)lane->gpr[in->rs1], st);
		break;
	}

	/* 4. 异常标志。 */
	fp_accumulate_flags(lane);
	lane->pc += 4;
	return true;
}

/* ---- 入口 ---- */

bool gpgpu_rv32_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id)
{
	GPGPULane *lane = &warp->lanes[lane_id];
	uint32_t raw;
	Rv32Insn in;

	/* 1. 取指。 */
	if (!gpgpu_lane_fetch(ctx, lane->pc, &raw))
		return false;

	/* 2. 解码。 */
	in = rv32_decode(raw);

	/* 3. 按类别执行。 */
	switch (in.opcode) {
	case OP_SYSTEM:
		return exec_system(ctx, warp, lane_id, &in);
	case OP_IMM:
		return exec_imm(ctx, lane, &in);
	case OP_REG:
		return exec_reg(ctx, lane, &in);
	case OP_LUI:
		set_gpr(lane, in.rd, raw & 0xfffff000);
		lane->pc += 4;
		return true;
	case OP_BRANCH:
		return exec_branch(ctx, lane, &in);
	case OP_JAL:
		set_gpr(lane, in.rd, lane->pc + 4);
		lane->pc += (uint32_t)in.imm_j;
		return true;
	case OP_LOAD:
	case OP_LOAD_FP:
		return exec_load(ctx, lane, &in);
	case OP_STORE:
	case OP_STORE_FP:
		return exec_store(ctx, lane, &in);
	case OP_FP:
		return exec_fp(ctx, lane, &in);
	default:
		return illegal(ctx);
	}
}
