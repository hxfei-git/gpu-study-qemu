// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RV64 解释器：解码与按类别执行
 *
 * rv64_decode() 只拆静态字段，rv64_execute() 读取本 lane 的状态并执行。
 * 共享模式在每轮首次遇到一个 PC 时取指和译码，后续同 PC lane 复用字段；
 * 两种模式都按 lane 编号执行，不合并寄存器、舍入或内存副作用。
 * 类别函数仍检查合法性，非法编码返回 GPGPU_EXEC_ERR_INSTRUCTION。
 *
 * 整数寄存器和 PC 为 64 位，立即数先符号扩展到 64 位再参与运算；
 * 32 位结果（LW、LUI、FCVT.W.S）符号扩展后写回，CSR 值零扩展。
 */
#include "qemu/osdep.h"
#include "gpgpu/gpgpu_tensor_abi.h"
#include "compute_internal.h"
#include "lane_mem.h"
#include "rv64.h"
#include "tensor.h"

/* 主 opcode；RV64 的 W 后缀指令（OP-IMM-32、OP-32）不在支持范围内。 */
enum {
	OP_LOAD = 0x03,
	OP_LOAD_FP = 0x07,
	OP_IMM = 0x13,
	OP_AUIPC = 0x17, /* PC 相对高位立即数。 */
	OP_STORE = 0x23,
	OP_STORE_FP = 0x27,
	OP_REG = 0x33,
	OP_LUI = 0x37,
	OP_FMADD = 0x43, /* R4 格式的融合乘加。 */
	OP_FP = 0x53,
	OP_BRANCH = 0x63,
	OP_JALR = 0x67, /* 寄存器间接跳转并写返回地址。 */
	OP_JAL = 0x6f,
	OP_SYSTEM = 0x73,
};

/* EBREAK 的完整编码。 */
#define INSN_EBREAK 0x00100073u
/* LOAD/STORE 的 funct3：有符号字节、32 位字、64 位双字与无符号字节。 */
#define MEM_BYTE 0 /* LB/SB：1 字节。 */
#define MEM_WORD 2
#define MEM_DOUBLE 3
#define MEM_BYTE_UNSIGNED 4 /* LBU：1 字节，零扩展。 */

/* OP-FP 的 funct7。 */
enum {
	FP_ADD_S = 0x00,
	FP_SUB_S = 0x04, /* 单精度减法。 */
	FP_MUL_S = 0x08,
	FP_DIV_S = 0x0c, /* 单精度除法。 */
	FP_MINMAX_S = 0x14, /* funct3 选择单精度最小值或最大值。 */
	FP_CVT_BF16 = 0x22,
	FP_CVT_FP8 = 0x24,
	FP_CVT_E2M1 = 0x26,
	FP_SQRT_S = 0x2c, /* 单精度平方根，rs2 固定为 0。 */
	FP_COMPARE_S = 0x50, /* funct3 选择 FEQ、FLT 或 FLE。 */
	FP_CVT_W_S = 0x60,
	FP_CVT_S_W = 0x68,
	FP_MV_X_W = 0x70, /* 浮点原始位搬到整数，符号扩展到 64 位。 */
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
 * struct Rv64Insn - 解码后的指令字段
 * @raw: 原始指令字。
 * @opcode: [6:0]。
 * @rd: [11:7]。
 * @funct3: [14:12]。
 * @rs1: [19:15]。
 * @rs2: [24:20]。
 * @funct7: [31:25]；R4 时低两位为 fmt，高五位为 rs3。
 * @imm_i: I 型符号扩展立即数，也用作 CSR 编号的来源。
 * @imm_s: S 型符号扩展立即数。
 * @imm_b: B 型符号扩展分支偏移。
 * @imm_j: J 型符号扩展跳转偏移。
 */
typedef struct Rv64Insn {
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
} Rv64Insn;

/**
 * struct Rv64PCGroup - 只在当前 warp 轮内有效的译码
 * @pc: 共享的取指地址，与本任务的代码入口同一地址空间。
 * @insn: 静态字段；不保存任何 lane 的寄存器、CSR 或浮点状态。
 */
typedef struct Rv64PCGroup {
	uint64_t pc;
	Rv64Insn insn;
} Rv64PCGroup;

static Rv64Insn rv64_decode(uint32_t raw)
{
	uint32_t b = ((raw >> 31) << 12) | (((raw >> 7) & 1) << 11) |
		(((raw >> 25) & 0x3f) << 5) | (((raw >> 8) & 0xf) << 1);
	uint32_t j = ((raw >> 31) << 20) | (((raw >> 12) & 0xff) << 12) |
		(((raw >> 20) & 1) << 11) | (((raw >> 21) & 0x3ff) << 1);
	uint32_t s = ((raw >> 25) << 5) | ((raw >> 7) & 0x1f);

	return (Rv64Insn) {
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
static void set_gpr(GPGPULane *lane, uint32_t rd, uint64_t value)
{
	if (rd)
		lane->gpr[rd] = value;
}

/* 当前指令编码非法或不受支持。 */
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
			const Rv64Insn *in)
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

/*
 * OP-IMM：ADDI、位运算与立即数移位。移位量占 [25:20] 共 6 位；
 * [31:26] 只接受 0（逻辑移位）或 0x10（SRAI），不能把 bit 25 当保留位。
 */
static bool exec_imm(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint64_t src = lane->gpr[in->rs1];
	uint64_t value;

	switch (in->funct3) {
	case 0:
		value = src + (int64_t)in->imm_i;
		break;
	case 1:
		if (in->raw >> 26)
			return illegal(ctx);
		value = src << ((in->raw >> 20) & 0x3f);
		break;
	case 4:
		value = src ^ (int64_t)in->imm_i;
		break;
	case 5:
		if ((in->raw >> 26) != 0 && (in->raw >> 26) != 0x10)
			return illegal(ctx);
		if (in->raw >> 26)
			value = (int64_t)src >> ((in->raw >> 20) & 0x3f);
		else
			value = src >> ((in->raw >> 20) & 0x3f);
		break;
	case 6:
		value = src | (int64_t)in->imm_i;
		break;
	case 7:
		value = src & (int64_t)in->imm_i;
		break;
	default:
		return illegal(ctx);
	}
	set_gpr(lane, in->rd, value);
	lane->pc += 4;
	return true;
}

/*
 * OP：ADD、SUB、MUL、位运算与无符号除余。乘积只取低 64 位；
 * DIVU 除零返回全 1，REMU 除零返回被除数，不执行 Host 的除零运算。
 */
static bool exec_reg(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint64_t a = lane->gpr[in->rs1];
	uint64_t b = lane->gpr[in->rs2];
	uint64_t value;

	switch (in->funct3) {
	case 0:
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
		break;
	case 4:
		if (in->funct7)
			return illegal(ctx);
		value = a ^ b;
		break;
	case 5:
		if (in->funct7 != 0x01)
			return illegal(ctx);
		value = b ? a / b : UINT64_MAX;
		break;
	case 6:
		if (in->funct7)
			return illegal(ctx);
		value = a | b;
		break;
	case 7:
		if (!in->funct7)
			value = a & b;
		else if (in->funct7 == 0x01)
			value = b ? a % b : a;
		else
			return illegal(ctx);
		break;
	default:
		return illegal(ctx);
	}
	set_gpr(lane, in->rd, value);
	lane->pc += 4;
	return true;
}

/* JALR 先读取旧 rs1，再写回返回地址；目标 bit 0 固定清零。 */
static bool exec_jalr(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint64_t target;

	if (in->funct3)
		return illegal(ctx);

	target = (lane->gpr[in->rs1] + (int64_t)in->imm_i) & ~UINT64_C(1);
	set_gpr(lane, in->rd, lane->pc + 4);
	lane->pc = target;
	return true;
}

/* 条件分支：按 64 位比较；每个 lane 独立保存 PC，可以走不同分支。 */
static bool exec_branch(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint64_t a = lane->gpr[in->rs1];
	uint64_t b = lane->gpr[in->rs2];
	bool taken;

	switch (in->funct3) {
	case 0:
		taken = a == b;
		break;
	case 1:
		taken = a != b;
		break;
	case 4:
		taken = (int64_t)a < (int64_t)b;
		break;
	case 5:
		taken = (int64_t)a >= (int64_t)b;
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
	lane->pc += taken ? (int64_t)in->imm_b : 4;
	return true;
}

/* ---- 访存指令 ---- */

/* 浮点只接受 32 位访存；整数另支持 LB/LBU/SB 和 LD/SD，不接受半字或 LWU。 */
static bool mem_width_valid(const Rv64Insn *in)
{
	bool fp = in->opcode == OP_LOAD_FP || in->opcode == OP_STORE_FP;

	return in->funct3 == MEM_WORD ||
		(!fp && (in->funct3 == MEM_BYTE || in->funct3 == MEM_DOUBLE ||
			 (in->opcode == OP_LOAD && in->funct3 == MEM_BYTE_UNSIGNED)));
}

/* LB、LW 符号扩展到 64 位；LBU 零扩展；FLW 只取 32 位原始位。 */
static bool exec_load(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	unsigned int size;
	uint64_t value;

	if (!mem_width_valid(in))
		return illegal(ctx);

	/* funct3 的低两位编码宽度的 log2；LB 与 LBU 都取一个字节。 */
	size = 1u << (in->funct3 & 3);
	if (!gpgpu_lane_load(ctx, lane->gpr[in->rs1] + (int64_t)in->imm_i, size, &value))
		return false;
	if (in->opcode == OP_LOAD_FP) {
		lane->fpr[in->rd] = value;
	} else {
		if (in->funct3 == MEM_BYTE)
			value = (uint64_t)(int8_t)value;
		else if (in->funct3 == MEM_WORD)
			value = (uint64_t)(int32_t)value;
		set_gpr(lane, in->rd, value);
	}
	lane->pc += 4;
	return true;
}

/* SB、SW/FSW、SD 分别写低 8、32、64 位。 */
static bool exec_store(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint64_t value = in->opcode == OP_STORE_FP ? lane->fpr[in->rs2] : lane->gpr[in->rs2];

	if (!mem_width_valid(in))
		return illegal(ctx);
	if (!gpgpu_lane_store(ctx, lane->gpr[in->rs1] + (int64_t)in->imm_s,
			      1u << in->funct3, value))
		return false;
	lane->pc += 4;
	return true;
}

/* ---- 浮点指令 ---- */

/*
 * 设置本条指令的舍入模式（rm=7 时取 fcsr.frm）并清除 SoftFloat 异常标志。
 * rm 为保留值 5、6，或 rm=7 而 frm 不是 0～4 时返回 false。
 */
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
 * E2M1 只有 8 个非负值，没有 NaN 和无穷：NaN、无穷和溢出都饱和为
 * 同号最大值 6.0；其余值先找相邻的两个可表示值，再按舍入模式
 * 取上或取下。
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

	/* NaN、无穷与不小于 8.0 的有限值都饱和。 */
	if (magnitude >= 0x7f800000) {
		if (magnitude > 0x7f800000)
			float_raise(float_flag_invalid, status);
		return sign | 7;
	}
	if (magnitude >= levels[8]) {
		float_raise(float_flag_overflow | float_flag_inexact, status);
		return sign | 7;
	}

	/* 找到第一个不小于 magnitude 的级别；恰好相等时无需舍入。 */
	while (magnitude > levels[upper])
		upper++;
	if (magnitude == levels[upper])
		return sign | upper;

	/* magnitude 在 levels[upper - 1] 与 levels[upper] 之间，按舍入模式取一端。 */
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

/*
 * 二元浮点运算的 rs2 是源寄存器；FSQRT.S 与整数转换固定为 0，
 * 低精度转换用 rs2 选择方向。不在表中的 funct7 非法。
 */
static bool fp_rs2_valid(uint32_t funct7, uint32_t rs2)
{
	switch (funct7) {
	case FP_ADD_S:
	case FP_SUB_S:
	case FP_MUL_S:
	case FP_DIV_S:
	case FP_MINMAX_S:
	case FP_COMPARE_S:
		return true;
	case FP_CVT_BF16:
	case FP_CVT_E2M1:
		return rs2 <= 1;
	case FP_CVT_FP8:
		return rs2 <= 3;
	case FP_SQRT_S:
	case FP_CVT_W_S:
	case FP_CVT_S_W:
		return rs2 == 0;
	default:
		return false;
	}
}

/*
 * exec_fp() - 执行单精度运算、比较、类型转换与整数/浮点间位搬运
 *
 * funct3 在算术和转换中是 rm，在 MIN/MAX、比较中却是操作号：
 *
 * 1. 位搬运只检查 rs2 与 funct3 为 0；FMV.X.W 还须把 bit 31 符号扩展；
 * 2. 检查 rs2。MIN/MAX、比较只清 SoftFloat 异常，不读取 frm；其余指令
 *    设置本 lane 的舍入模式，保留的 rm 或动态 frm 视为非法；
 * 3. 按 funct7 计算并写回 rd。MIN/MAX 使用 RISC-V 2.2 的 minimum_number /
 *    maximum_number：NaN 与数值相遇时取数值，sNaN 仍置 invalid；
 *    -0 小于 +0。FEQ 只对 sNaN 置 invalid，FLT/FLE 对任意 NaN 都置位；
 * 4. 累加 fcsr.fflags，再推进 PC；位搬运不改变异常。
 *
 * Return: 合法且执行成功为 true；否则记录非法指令并返回 false。
 */
static bool exec_fp(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	float_status *st = &lane->fp_status;
	uint32_t a = lane->fpr[in->rs1];
	uint32_t b = lane->fpr[in->rs2];

	/* 1. 两种 FMV 都搬运原始位，不经过浮点转换或舍入。 */
	if (in->funct7 == FP_MV_W_X || in->funct7 == FP_MV_X_W) {
		if (in->rs2 || in->funct3)
			return illegal(ctx);
		if (in->funct7 == FP_MV_W_X)
			lane->fpr[in->rd] = (uint32_t)lane->gpr[in->rs1];
		else
			set_gpr(lane, in->rd, (uint64_t)(int32_t)a);
		lane->pc += 4;
		return true;
	}

	/* 2. 操作号不能当作 rm，MIN/MAX、比较也不受 frm 保留值影响。 */
	if (!fp_rs2_valid(in->funct7, in->rs2))
		return illegal(ctx);
	if (in->funct7 == FP_MINMAX_S || in->funct7 == FP_COMPARE_S) {
		if (in->funct3 > (in->funct7 == FP_MINMAX_S ? 1 : 2))
			return illegal(ctx);
		set_float_exception_flags(0, st);
	} else if (!fp_set_rounding(lane, in->funct3)) {
		return illegal(ctx);
	}

	/* 3. 舍入与异常只使用本 lane 的 fp_status。 */
	switch (in->funct7) {
	case FP_ADD_S:
		lane->fpr[in->rd] = float32_add(a, b, st);
		break;
	case FP_SUB_S:
		lane->fpr[in->rd] = float32_sub(a, b, st);
		break;
	case FP_MUL_S:
		lane->fpr[in->rd] = float32_mul(a, b, st);
		break;
	case FP_DIV_S:
		lane->fpr[in->rd] = float32_div(a, b, st);
		break;
	case FP_SQRT_S:
		lane->fpr[in->rd] = float32_sqrt(a, st);
		break;
	case FP_MINMAX_S:
		lane->fpr[in->rd] = in->funct3 ? float32_maximum_number(a, b, st) :
					      float32_minimum_number(a, b, st);
		break;
	case FP_COMPARE_S:
		if (in->funct3 == 2)
			set_gpr(lane, in->rd, float32_eq_quiet(a, b, st));
		else if (in->funct3 == 1)
			set_gpr(lane, in->rd, float32_lt(a, b, st));
		else
			set_gpr(lane, in->rd, float32_le(a, b, st));
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
		set_gpr(lane, in->rd, (uint64_t)(int64_t)float32_to_int32(a, st));
		break;
	case FP_CVT_S_W:
		lane->fpr[in->rd] = int32_to_float32((int32_t)lane->gpr[in->rs1], st);
		break;
	}

	/* 4. fflags 只累加不清除，与 RISC-V 的语义一致。 */
	fp_accumulate_flags(lane);
	lane->pc += 4;
	return true;
}

/*
 * exec_fmadd() - 执行单次舍入的 FMADD.S
 *
 * R4 编码不能按 OP-FP 的 funct7 解释，最高位是第三个源寄存器：
 *
 *   31..27  26..25  24..20  19..15  14..12  11..7  6..0
 *   rs3     fmt     rs2     rs1     rm      rd     0x43
 *
 * 例：fmadd.s f2, f0, f1, f3, rne 编码为 0x1810_0143，
 * rs3 = 3、fmt = 0、rm = 0。
 *
 * 1. 只接受 fmt=0 的单精度和有效 rm；
 * 2. 一次调用 SoftFloat muladd，不能拆成分别舍入的 MUL、ADD；
 * 3. 累加本 lane 的异常并推进 PC。
 *
 * Return: 合法且执行成功为 true；否则记录非法指令并返回 false。
 */
static bool exec_fmadd(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	uint32_t rs3 = in->raw >> 27;

	/* 1. fmt 位不是 rs3 的一部分，其他精度不在子集内。 */
	if ((in->funct7 & 3) || !fp_set_rounding(lane, in->funct3))
		return illegal(ctx);

	/* 2. 先读三个源，再写 rd；rd 可以与任一源相同。 */
	lane->fpr[in->rd] = float32_muladd(lane->fpr[in->rs1], lane->fpr[in->rs2],
					 lane->fpr[rs3], 0, &lane->fp_status);

	/* 3. 保留之前的 fflags，只累加本次结果。 */
	fp_accumulate_flags(lane);
	lane->pc += 4;
	return true;
}

/*
 * exec_tensor() - 执行 custom-0 的有界张量指令
 *
 * R4 的 funct3 是类别，不是浮点 rm；fmt 选择矩阵或布局格式：
 *
 *   31..27  26..25  24..20  19..15  14..12  11..7  6..0
 *   rs3     fmt     rs2     rs1     kind    x0     custom-0
 *
 * 例：tmat.w8 t0,t2,t1 的 rs1/rs2/rs3 为 5/7/6；GPR 保存命令地址、块号、K 起点。
 *
 * 1. 检查 rd、类别、格式和布局的 rs3 编码；
 *    非法编码不读参数，也不记内存故障；
 * 2. 固定本 Lane 的 RNE，由张量单元读取有界输入、计算并按行写回；
 * 3. 汇总计算量和本 Lane 的异常，成功才推进 PC。
 *    张量指令完成整轮后提前让出，不改 Lane 顺序或累计预算。
 *
 * 张量单元已记录首次访存故障，后续 Lane 由 round() 在失败时立即停止。
 * Return: 指令全部写回完成为 true；非法编码或访问失败为 false。
 */
static bool exec_tensor(GPGPUExecContext *ctx, GPGPULane *lane, const Rv64Insn *in)
{
	GPGPUTensorStats stats = { 0 };
	uint32_t rs3 = in->raw >> 27;
	uint32_t format = in->funct7 & 3;
	bool ok;

	/* 1. 布局要求寄存器编号为 x0，不能用其他值恰为零的 GPR 代替。 */
	if (in->rd ||
	    (in->funct3 != GPGPU_TENSOR_KIND_MATRIX && in->funct3 != GPGPU_TENSOR_KIND_LAYOUT) ||
	    (in->funct3 == GPGPU_TENSOR_KIND_MATRIX && format > GPGPU_TENSOR_CONV) ||
	    (in->funct3 == GPGPU_TENSOR_KIND_LAYOUT && (format > GPGPU_TENSOR_NEAREST || rs3)))
		return illegal(ctx);

	/* 2. 固定 rm=0，不改 frm；各 Lane 的状态与副作用独立。 */
	fp_set_rounding(lane, 0);
	ctx->yield_requested = true;
	if (ctx->profile)
		ctx->profile->tensor_instructions++;
	ok = gpgpu_tensor_execute(ctx, lane, in->funct3, format, lane->gpr[in->rs1],
				  lane->gpr[in->rs2], lane->gpr[rs3], ctx->tensor_native,
				  ctx->profile ? &stats : NULL);

	/* 3. 写回失败仍保留计算的异常和统计，不重试或回滚。 */
	if (ctx->profile) {
		ctx->profile->tensor_macs += stats.macs;
		ctx->profile->tensor_native_tiles += stats.native_tiles;
		ctx->profile->tensor_soft_tiles += stats.fallback_tiles;
		ctx->profile->tensor_layout_elements += stats.layout_elements;
	}
	fp_accumulate_flags(lane);
	if (!ok)
		return false;
	lane->pc += 4;
	return true;
}

/* 类别函数只使用当前 lane 的寄存器、PC、舍入和异常，静态字段只读。 */
static bool rv64_execute(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id,
			 const Rv64Insn *in)
{
	GPGPULane *lane = &warp->lanes[lane_id];

	switch (in->opcode) {
	case OP_SYSTEM:
		return exec_system(ctx, warp, lane_id, in);
	case OP_IMM:
		return exec_imm(ctx, lane, in);
	case OP_REG:
		return exec_reg(ctx, lane, in);
	case OP_LUI:
		set_gpr(lane, in->rd, (uint64_t)(int32_t)(in->raw & 0xfffff000));
		lane->pc += 4;
		return true;
	case OP_AUIPC:
		set_gpr(lane, in->rd, lane->pc + (int64_t)(int32_t)(in->raw & 0xfffff000));
		lane->pc += 4;
		return true;
	case OP_BRANCH:
		return exec_branch(ctx, lane, in);
	case OP_JALR:
		return exec_jalr(ctx, lane, in);
	case OP_JAL:
		set_gpr(lane, in->rd, lane->pc + 4);
		lane->pc += (int64_t)in->imm_j;
		return true;
	case OP_LOAD:
	case OP_LOAD_FP:
		return exec_load(ctx, lane, in);
	case OP_STORE:
	case OP_STORE_FP:
		return exec_store(ctx, lane, in);
	case OP_FMADD:
		return exec_fmadd(ctx, lane, in);
	case OP_FP:
		return exec_fp(ctx, lane, in);
	case GPGPU_TENSOR_OPCODE:
		return exec_tensor(ctx, lane, in);
	default:
		return illegal(ctx);
	}
}

/* 取指继续检查权限、范围和物理故障；失败不产生译码。 */
static bool rv64_fetch_decode(GPGPUExecContext *ctx, uint64_t pc, Rv64Insn *in)
{
	uint32_t raw;

	if (ctx->profile)
		ctx->profile->fetches++;
	if (!gpgpu_lane_fetch(ctx, pc, &raw))
		return false;
	*in = rv64_decode(raw);
	if (ctx->profile)
		ctx->profile->decodes++;
	return true;
}

bool gpgpu_rv64_step(GPGPUExecContext *ctx, GPGPUWarp *warp, unsigned int lane_id)
{
	Rv64Insn in;

	if (ctx->profile)
		ctx->profile->lane_steps++;
	return rv64_fetch_decode(ctx, warp->lanes[lane_id].pc, &in) &&
		rv64_execute(ctx, warp, lane_id, &in);
}

bool gpgpu_rv64_round(GPGPUExecContext *ctx, GPGPUWarp *warp)
{
	Rv64PCGroup groups[GPGPU_WARP_SIZE];
	unsigned int group_count = 0;

	/* 1. 未共享且未剖析时保留逐 lane 路径，不额外查 PC 表。 */
	if (!ctx->shared_fetch && !ctx->profile) {
		for (unsigned int id = 0; id < GPGPU_WARP_SIZE; id++)
			if (warp->lanes[id].active && !gpgpu_rv64_step(ctx, warp, id))
				return false;
		return true;
	}
	if (ctx->profile)
		ctx->profile->rounds++;

	/* 2. 按 lane 编号处理，首次遇到 PC 才取指，不预取或重排整组。 */
	for (unsigned int id = 0; id < GPGPU_WARP_SIZE; id++) {
		GPGPULane *lane = &warp->lanes[id];
		unsigned int group;

		if (!lane->active)
			continue;
		if (ctx->shared_fetch && ctx->profile)
			ctx->profile->lane_steps++;
		for (group = 0; group < group_count; group++)
			if (groups[group].pc == lane->pc)
				break;
		if (group == group_count) {
			groups[group].pc = lane->pc;
			if (ctx->profile)
				ctx->profile->pc_groups++;
			if (ctx->shared_fetch &&
			    !rv64_fetch_decode(ctx, lane->pc, &groups[group].insn))
				return false;
			group_count++;
		}

		/* 3. 字段只读；首个失败立即停止，保留先前副作用。 */
		if (ctx->shared_fetch) {
			if (!rv64_execute(ctx, warp, id, &groups[group].insn))
				return false;
		} else if (!gpgpu_rv64_step(ctx, warp, id)) {
			return false;
		}
	}
	return true;
}
