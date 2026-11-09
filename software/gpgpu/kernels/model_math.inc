/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 模型数学：只使用教学 RV64 FP32 指令的叶子 helper
 *
 * 由 model_eval.S 经 C 预处理器包含，共用 s11 命令指针和常量槽。没有
 * 栈，也没有内部调用；JAL 写 ra，JALR 返回。所有 s 寄存器、a 寄存器、
 * t2..t6 与 f16..f31 保持不变。系数和范围缩减常量由 Host 写入同一
 * 命令块，GPU 不读取 CPU 预先计算的指数、正弦或余弦中间结果。
 */

/*
 * model_exp() - 范围缩减与六次多项式计算 exp
 *
 * 输入、输出均为 f0；覆盖 t0、t1、f0..f7，不覆盖 ra。
 *
 * 1. 有限输入截到 EXP_LO..EXP_HI；本组使用 [-80,80]，避免通过整数
 *    位构造 2^n 时碰到非正规数或无穷。掩码的大负数因而得到极小正数，
 *    不是精确零；这不是覆盖 NaN/Inf 语义的通用 libm。
 * 2. n=round_RNE(x*log2(e))，r=x-n*ln2_hi-n*ln2_lo。
 * 3. 在约 [-ln2/2,ln2/2] 上以 Horner/FMA 计算六次 Taylor 多项式，
 *    再乘 (n+127)<<23 构造的正常 FP32 2^n。
 *
 * 返回后 f0 保存近似指数。调用者负责 softmax 减 max、行求和和归一化。
 */
model_exp:
	flw f1, GPGPU_EVAL_EXP_LO(s11)
	fmax.s f0, f0, f1
	flw f1, GPGPU_EVAL_EXP_HI(s11)
	fmin.s f0, f0, f1
	flw f1, GPGPU_EVAL_LOG2E(s11)
	fmul.s f1, f0, f1
	fcvt.w.s t0, f1, rne
	fcvt.s.w f1, t0
	flw f2, GPGPU_EVAL_LN2_HI(s11)
	fmul.s f2, f1, f2
	fsub.s f0, f0, f2
	flw f2, GPGPU_EVAL_LN2_LO(s11)
	fmul.s f2, f1, f2
	fsub.s f0, f0, f2
	flw f2, GPGPU_EVAL_C6(s11)
	flw f3, GPGPU_EVAL_C5(s11)
	fmadd.s f2, f2, f0, f3
	flw f3, GPGPU_EVAL_C4(s11)
	fmadd.s f2, f2, f0, f3
	flw f3, GPGPU_EVAL_C3(s11)
	fmadd.s f2, f2, f0, f3
	flw f3, GPGPU_EVAL_C2(s11)
	fmadd.s f2, f2, f0, f3
	flw f3, GPGPU_EVAL_ONE(s11)
	fmadd.s f2, f2, f0, f3
	fmadd.s f2, f2, f0, f3
	addi t0, t0, 127
	slli t0, t0, 23
	fmv.w.x f1, t0
	fmul.s f0, f2, f1
	jalr zero, 0(ra)

/*
 * model_sincos() - split 2*pi 缩减后计算 sin13/cos14
 *
 * 输入 f0=x，输出 f0=sin(x)、f1=cos(x)；覆盖 t0、t1、f0..f15，
 * 不覆盖 ra，f16..f31 保留。只针对本组有限位置频率，实际 |x|<=2048；
 * 不声称支持任意大 FP32 参数或完整 NaN/Inf 语义。
 *
 * 1. n=round_RNE(x/(2*pi))；用 TWO_PI_HI=6.28125 与相应低位修正，
 *    把 x 缩到约 [-pi,pi]。本组 n 的高位乘积可精确表示。
 * 2. 超出 [-pi/2,pi/2] 时反射角度；sin 不变，cos 改符号。
 * 3. 在反射区间内，用 z=r*r 和交错正系数 Horner 计算 sin13/cos14。
 *
 * 返回的三角值直接来自 GPU FP32 运算。Host 只提供固定数学常量。
 */
model_sincos:
	flw f1, GPGPU_EVAL_INV_TWO_PI(s11)
	fmul.s f1, f0, f1
	fcvt.w.s t0, f1, rne
	fcvt.s.w f1, t0
	flw f2, GPGPU_EVAL_TWO_PI_HI(s11)
	fmul.s f2, f2, f1
	fsub.s f0, f0, f2
	flw f2, GPGPU_EVAL_TWO_PI_LO(s11)
	fmul.s f2, f2, f1
	fsub.s f0, f0, f2
	flw f15, GPGPU_EVAL_ONE(s11)
	flw f2, GPGPU_EVAL_HALF_PI(s11)
	flt.s t0, f2, f0
	bne t0, zero, model_sincos_reflect_positive
	flw f3, GPGPU_EVAL_ZERO(s11)
	fsub.s f3, f3, f2
	flt.s t0, f0, f3
	bne t0, zero, model_sincos_reflect_negative
	jal zero, model_sincos_polynomial
model_sincos_reflect_positive:
	flw f2, GPGPU_EVAL_PI(s11)
	fsub.s f0, f2, f0
	flw f15, GPGPU_EVAL_NEGONE(s11)
	jal zero, model_sincos_polynomial
model_sincos_reflect_negative:
	flw f2, GPGPU_EVAL_PI(s11)
	flw f3, GPGPU_EVAL_ZERO(s11)
	fsub.s f2, f3, f2
	fsub.s f0, f2, f0
	flw f15, GPGPU_EVAL_NEGONE(s11)
model_sincos_polynomial:
	fmul.s f4, f0, f0
	flw f5, GPGPU_EVAL_INV_FACT13(s11)
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_INV_FACT11(s11)
	fsub.s f5, f6, f5
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_INV_FACT9(s11)
	fsub.s f5, f6, f5
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_INV_FACT7(s11)
	fsub.s f5, f6, f5
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_C5(s11)
	fsub.s f5, f6, f5
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_C3(s11)
	fsub.s f5, f6, f5
	fmul.s f5, f5, f4
	flw f6, GPGPU_EVAL_ONE(s11)
	fsub.s f5, f6, f5
	flw f7, GPGPU_EVAL_INV_FACT14(s11)
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_INV_FACT12(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_INV_FACT10(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_INV_FACT8(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_C6(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_C4(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	flw f8, GPGPU_EVAL_C2(s11)
	fsub.s f7, f8, f7
	fmul.s f7, f7, f4
	fsub.s f7, f6, f7
	fmul.s f1, f7, f15
	fmul.s f0, f0, f5
	jalr zero, 0(ra)
