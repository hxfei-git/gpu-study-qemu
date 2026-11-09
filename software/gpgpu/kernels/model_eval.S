/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 模型算子：按共享命令块选择的无栈 RV64 Kernel
 *
 * Host 上传逻辑 C 紧凑数组；source_stride 不参与文件寻址。每次派发的
 * a0 指向 RT 的两个 64 位槽：COMMAND 为 512 字节命令块 GPU 地址，BASE
 * 为 tile 的首个全局工作项。Lane 计算
 *
 *   idx = BASE + BLOCK_ID_X * BLOCK_DIM_X + THREAD_ID_X
 *       -> idx < COUNT -> 本 Lane 的标量元素或张量块 -> 完整 OUT 数组
 *
 * 输入和输出指针始终指向完整数组，不再加 tile 偏移。归约的 COUNT 是
 * outer*inner，idx 是归约行编号；其他标量路径的 COUNT 是输出元素数。
 * 块路径的 COUNT 是实际块数，idx 为线性块号，AUX1 为完整输出元素数，
 * AUX2=BACKEND_BLOCK；只有 Linear/BMM/Conv/Copy/Concat/Nearest 解释此标志。
 * 复合算子保留原标量路径与 AUX scratch 含义，不把 scratch 地址当后端。
 * 仅用 X 维派发，不用栈、共享内存、shuffle、barrier 或原子操作。Host
 * 负责检查正维度、数组大小、embedding token 范围和下列布局前提。
 *
 * Dn 均指共享头的 GPGPU_EVAL_Dn；未提及的槽不用填写。浮点属性低 32 位
 * 保存 FP32 原始位。除明确标出的 INT64 生成/复制模式，输入、输出均为
 * FP32；W8 权重为有符号 INT8，embedding 的 token 为 INT64。
 *
 * LINEAR_F32 / LINEAR_W8：
 *   IN0=X[M,K]，IN1=W[N,K]；W8 的 IN2=scales[N]；IN3=可空 bias[N]。
 *   D0=M，D1=N，D2=K。W8 每个 q 先转 FP32、乘所属行 scale，再参与乘加。
 * BMM：IN0=A[B,M,K]，IN1=B[B,K,N]；D0=B，D1=M，D2=N，D3=K。
 *   D10=1 时，D4..D6=A 的三个元素 stride，D7..D9=B 的三个元素 stride；
 *   D10=0 时按逻辑 C 紧凑布局读取。Host 不替 Kernel 完成重排。
 * EMBEDDING_W8：IN0=ids，IN1=q[V,H]，IN2=scales[V]；D0=H。
 * ADD / MUL / SUB / DIV：IN0、IN1 为两输入；D0、D1 是各自元素数。
 *   以 idx%元素数重复连续子数组，只支持本组真实签名需要的周期广播：
 *   相同 shape、跨注意力头重复的 mask、重复的逐 anchor stride。
 * MUL_SCALAR / ADD_SCALAR：IN0=x；D0=FP32 标量。
 * SILU / SIGMOID / SIN / COS：IN0=x；不使用维度槽。
 * REDUCE_MAX / REDUCE_SUM / REDUCE_RMS / DFL_PROJECT：
 *   IN0=x[outer,width,inner]；D0=width，D1=inner，RMS 的 D2=epsilon。
 *   行 idx 的首元素为 (idx/inner)*width*inner + idx%inner，相邻元素
 *   间隔 inner。RMS 输出 1/sqrt(sum(x*x)/width+epsilon)；DFL 输出
 *   sum(x[j]*j)，j 从 0 到 width-1，OUT 均为 [outer,inner]。
 * EXP_SHIFT / DIV_ROW：IN0=x[outer,width,inner]，IN1=对应行的 max/sum；
 *   D0=width，D1=inner；输出仍为完整逐元素数组。
 * RMS_APPLY：IN0=x，IN1=gamma[width]，IN2=行倒数范数；D0=width，D1=inner。
 * BATCHNORM：IN0=x[N,C,spatial]，IN1=gamma，IN2=beta，IN3=mean，IN4=var；
 *   D0=C，D1=spatial(H*W)，D2=epsilon；只实现冻结参数的推理路径。
 * CONV2D：IN0=x[N,Cin,Hin,Win]，IN1=w[Cout,Cin/groups,Kh,Kw]，
 *   IN2=可空 bias[Cout]；D0=Cin，D1=Hin，D2=Win，D3=Cout，D4=Hout，
 *   D5=Wout，D6=Kh，D7=Kw，D8=Sh，D9=Sw，D10=Ph，D11=Pw，D12=Dh，
 *   D13=Dw，D14=groups。支持普通卷积和 depthwise，padding 为零。
 * MAXPOOL2D：IN0=x[N,C,Hin,Win]；D0=C，D1=Hin，D2=Win，D3=Hout，
 *   D4=Wout，D5=Kh，D6=Kw，D7=Sh，D8=Sw，D9=Ph，D10=Pw，D11=Dh，D12=Dw。
 *   padding 为负无穷；Host 由实际 ceil_mode 计算 Hout/Wout。
 * NEAREST2D：IN0=x[N,C,Hin,Win]；D0=Hin，D1=Win，D2=Hout，D3=Wout。
 *   使用 floor(oy*Hin/Hout)、floor(ox*Win/Wout)，覆盖本组整数 x2 放大。
 * CONCAT：IN0..IN5 为至多六个输入；D0=inner(拼接轴之后的元素数)，
 *   D1=输出拼接轴宽度，D2=输入个数，D3..D8=各输入的拼接轴宽度。
 *   stack 等价于在新轴上 concat，每个输入的轴宽为 1。
 * COPY：IN0=x；D0=COPY_F32 / COPY_I64_TO_F32 / COPY_I64。
 *   D1=rank，0 表示连续；否则 D2 起依次保存 rank 个 shape，再保存
 *   rank 个元素 stride，rank<=7。输出按逻辑 C 顺序连续写入。
 *   INT64 转 FP32 使用 FCVT.S.W，输入必须在 signed32 范围内；本组
 *   实际位置编号不超过 2048。FP32 与 INT64 纯复制保持原始位。
 * ROPE：IN0=x[B,H,S,D]，IN1=cos[B,S,D]，IN2=sin[B,S,D]；
 *   D0=H，D1=S，D2=D(正偶数)。D7=1 时 D3..D6 保存 x 的 B/H/S/D
 *   元素 stride；D7=0 时 x 连续。cos/sin 始终连续，split-half 旋转
 *   先分别乘，再相加。所有 stride 非负，允许为零。
 * BOX_DECODE：IN0=distances[B,4,A]，IN1=anchors[B,2,A]，
 *   IN2=可空 stride[A]；D0=A，D1=0 表示 xywh，1 表示 xyxy。
 *   无 stride 时不缩放；本组独立 boxdecode 签名使用此模式。
 * ARANGE：D0=start，D1=step，D2=TYPE_F32 / TYPE_I64。
 * FULL：D0=fill，D2=TYPE_F32 / TYPE_I64。
 *   I64 模式属性保存完整整数并以 SD 输出；F32 模式属性保存浮点原始位。
 *
 * 寄存器约定：s0 固定为全局 idx，s10 为标量路径本项 FP32 输出地址，s11
 * 固定为命令指针。块指令自行从 OUT 定位输出，不使用 s10。算子可改其余
 * 寄存器。数学 helper 只用 ra 返回，无嵌套
 * 调用；具体 clobber 见 model_math.inc。调用者需跨 helper 保存的值放在
 * s 寄存器或 f16..f31。未知操作直接结束，不声称已产生有效输出。
 */
#include "../include/gpgpu/gpgpu_eval_abi.h"

/* 启动只读 CSR 编号，与现有 gpgpu_kernel.inc 的 X 维定义一致。 */
.equ MODEL_THREAD_ID_X, 0xCC0
.equ MODEL_BLOCK_ID_X, 0xCC3
.equ MODEL_BLOCK_DIM_X, 0xCC6

model_eval:
	ld s11, GPGPU_EVAL_RT_COMMAND(a0)
	ld s0, GPGPU_EVAL_RT_BASE(a0)
	csrr t0, MODEL_BLOCK_ID_X
	csrr t1, MODEL_BLOCK_DIM_X
	mul t0, t0, t1
	csrr t1, MODEL_THREAD_ID_X
	add t0, t0, t1
	add s0, s0, t0
	ld t0, GPGPU_EVAL_COUNT(s11)
	bgeu s0, t0, model_done
	ld s10, GPGPU_EVAL_OUT(s11)
	slli t0, s0, 2
	add s10, s10, t0
	ld t0, GPGPU_EVAL_OP(s11)

/* 远跳转使用 JAL；条件分支只跳向相邻的分派项，不受 4 KiB 范围限制。 */
	addi t1, zero, GPGPU_EVAL_LINEAR_F32
	bne t0, t1, model_dispatch_linear_w8
	jal zero, model_linear
model_dispatch_linear_w8:
	addi t1, zero, GPGPU_EVAL_LINEAR_W8
	bne t0, t1, model_dispatch_bmm
	jal zero, model_linear
model_dispatch_bmm:
	addi t1, zero, GPGPU_EVAL_BMM
	bne t0, t1, model_dispatch_embedding
	jal zero, model_bmm
model_dispatch_embedding:
	addi t1, zero, GPGPU_EVAL_EMBEDDING_W8
	bne t0, t1, model_dispatch_add
	jal zero, model_embedding
model_dispatch_add:
	addi t1, zero, GPGPU_EVAL_ADD
	bne t0, t1, model_dispatch_mul
	jal zero, model_binary
model_dispatch_mul:
	addi t1, zero, GPGPU_EVAL_MUL
	bne t0, t1, model_dispatch_mul_scalar
	jal zero, model_binary
model_dispatch_mul_scalar:
	addi t1, zero, GPGPU_EVAL_MUL_SCALAR
	bne t0, t1, model_dispatch_silu
	jal zero, model_scalar
model_dispatch_silu:
	addi t1, zero, GPGPU_EVAL_SILU
	bne t0, t1, model_dispatch_sigmoid
	jal zero, model_activation
model_dispatch_sigmoid:
	addi t1, zero, GPGPU_EVAL_SIGMOID
	bne t0, t1, model_dispatch_reduce_max
	jal zero, model_activation
model_dispatch_reduce_max:
	addi t1, zero, GPGPU_EVAL_REDUCE_MAX
	bne t0, t1, model_dispatch_reduce_sum
	jal zero, model_reduce
model_dispatch_reduce_sum:
	addi t1, zero, GPGPU_EVAL_REDUCE_SUM
	bne t0, t1, model_dispatch_reduce_rms
	jal zero, model_reduce
model_dispatch_reduce_rms:
	addi t1, zero, GPGPU_EVAL_REDUCE_RMS
	bne t0, t1, model_dispatch_exp_shift
	jal zero, model_reduce
model_dispatch_exp_shift:
	addi t1, zero, GPGPU_EVAL_EXP_SHIFT
	bne t0, t1, model_dispatch_div_row
	jal zero, model_row_apply
model_dispatch_div_row:
	addi t1, zero, GPGPU_EVAL_DIV_ROW
	bne t0, t1, model_dispatch_rms_apply
	jal zero, model_row_apply
model_dispatch_rms_apply:
	addi t1, zero, GPGPU_EVAL_RMS_APPLY
	bne t0, t1, model_dispatch_batchnorm
	jal zero, model_rms_apply
model_dispatch_batchnorm:
	addi t1, zero, GPGPU_EVAL_BATCHNORM
	bne t0, t1, model_dispatch_conv2d
	jal zero, model_batchnorm
model_dispatch_conv2d:
	addi t1, zero, GPGPU_EVAL_CONV2D
	bne t0, t1, model_dispatch_maxpool2d
	jal zero, model_conv2d
model_dispatch_maxpool2d:
	addi t1, zero, GPGPU_EVAL_MAXPOOL2D
	bne t0, t1, model_dispatch_nearest2d
	jal zero, model_maxpool2d
model_dispatch_nearest2d:
	addi t1, zero, GPGPU_EVAL_NEAREST2D
	bne t0, t1, model_dispatch_concat
	jal zero, model_nearest2d
model_dispatch_concat:
	addi t1, zero, GPGPU_EVAL_CONCAT
	bne t0, t1, model_dispatch_copy
	jal zero, model_concat
model_dispatch_copy:
	addi t1, zero, GPGPU_EVAL_COPY
	bne t0, t1, model_dispatch_rope
	jal zero, model_copy
model_dispatch_rope:
	addi t1, zero, GPGPU_EVAL_ROPE
	bne t0, t1, model_dispatch_dfl
	jal zero, model_rope
model_dispatch_dfl:
	addi t1, zero, GPGPU_EVAL_DFL_PROJECT
	bne t0, t1, model_dispatch_boxdecode
	jal zero, model_reduce
model_dispatch_boxdecode:
	addi t1, zero, GPGPU_EVAL_BOX_DECODE
	bne t0, t1, model_dispatch_add_scalar
	jal zero, model_boxdecode
model_dispatch_add_scalar:
	addi t1, zero, GPGPU_EVAL_ADD_SCALAR
	bne t0, t1, model_dispatch_sub
	jal zero, model_scalar
model_dispatch_sub:
	addi t1, zero, GPGPU_EVAL_SUB
	bne t0, t1, model_dispatch_div
	jal zero, model_binary
model_dispatch_div:
	addi t1, zero, GPGPU_EVAL_DIV
	bne t0, t1, model_dispatch_sin
	jal zero, model_binary
model_dispatch_sin:
	addi t1, zero, GPGPU_EVAL_SIN
	bne t0, t1, model_dispatch_cos
	jal zero, model_trig
model_dispatch_cos:
	addi t1, zero, GPGPU_EVAL_COS
	bne t0, t1, model_dispatch_arange
	jal zero, model_trig
model_dispatch_arange:
	addi t1, zero, GPGPU_EVAL_ARANGE
	bne t0, t1, model_dispatch_full
	jal zero, model_arange
model_dispatch_full:
	addi t1, zero, GPGPU_EVAL_FULL
	bne t0, t1, model_done
	jal zero, model_full

model_done:
	ebreak
model_store_f32:
	fsw f0, 0(s10)
	ebreak
/* INT64 输出不能复用 s10 的四字节元素偏移。t0 保存要写的完整整数。 */
model_store_i64:
	ld t1, GPGPU_EVAL_OUT(s11)
	slli t2, s0, 3
	add t1, t1, t2
	sd t0, 0(t1)
	ebreak

/* 仅这些基本算子读取后端标志；标量主体的寻址与乘加顺序保持不变。 */
model_linear:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_linear_scalar
	jal zero, model_tensor_linear
/* 一个标量 Lane 只归约一个输出元素。 */
model_linear_scalar:
	ld s1, GPGPU_EVAL_D1(s11)
	ld s2, GPGPU_EVAL_D2(s11)
	remu s3, s0, s1
	divu s4, s0, s1
	mul t0, s4, s2
	slli t0, t0, 2
	ld s5, GPGPU_EVAL_IN0(s11)
	add s5, s5, t0
	ld s6, GPGPU_EVAL_IN1(s11)
	ld s9, GPGPU_EVAL_IN3(s11)
	flw f0, GPGPU_EVAL_ZERO(s11)
	addi a0, zero, 0
	mul t0, s3, s2
	ld t1, GPGPU_EVAL_OP(s11)
	addi t2, zero, GPGPU_EVAL_LINEAR_W8
	beq t1, t2, model_linear_w8_setup
	slli t0, t0, 2
	add s6, s6, t0
	beq s2, zero, model_linear_bias
model_linear_f32_loop:
	flw f1, 0(s5)
	flw f2, 0(s6)
	fmadd.s f0, f1, f2, f0
	addi s5, s5, 4
	addi s6, s6, 4
	addi a0, a0, 1
	bltu a0, s2, model_linear_f32_loop
	jal zero, model_linear_bias
model_linear_w8_setup:
	add s6, s6, t0
	ld s7, GPGPU_EVAL_IN2(s11)
	slli t0, s3, 2
	add s7, s7, t0
	flw f4, 0(s7)
	beq s2, zero, model_linear_bias
model_linear_w8_loop:
	lb t0, 0(s6)
	fcvt.s.w f2, t0
	fmul.s f2, f2, f4
	flw f1, 0(s5)
	fmadd.s f0, f1, f2, f0
	addi s5, s5, 4
	addi s6, s6, 1
	addi a0, a0, 1
	bltu a0, s2, model_linear_w8_loop
model_linear_bias:
	beq s9, zero, model_linear_finish
	slli t0, s3, 2
	add s9, s9, t0
	flw f1, 0(s9)
	fadd.s f0, f0, f1
model_linear_finish:
	jal zero, model_store_f32

model_bmm:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_bmm_scalar
	jal zero, model_tensor_bmm
model_bmm_scalar:
	ld s1, GPGPU_EVAL_D2(s11)
	ld s2, GPGPU_EVAL_D3(s11)
	ld s3, GPGPU_EVAL_D1(s11)
	remu s4, s0, s1
	divu s5, s0, s1
	divu s6, s5, s3
	ld t0, GPGPU_EVAL_D10(s11)
	bne t0, zero, model_bmm_strided
	mul t0, s5, s2
	slli t0, t0, 2
	ld s7, GPGPU_EVAL_IN0(s11)
	add s7, s7, t0
	mul t0, s6, s2
	mul t0, t0, s1
	add t0, t0, s4
	slli t0, t0, 2
	ld s8, GPGPU_EVAL_IN1(s11)
	add s8, s8, t0
	slli s9, s1, 2
	addi s5, zero, 4
	jal zero, model_bmm_setup_done
model_bmm_strided:
	ld t0, GPGPU_EVAL_D4(s11)
	mul t0, t0, s6
	remu t1, s5, s3
	ld t2, GPGPU_EVAL_D5(s11)
	mul t1, t1, t2
	add t0, t0, t1
	slli t0, t0, 2
	ld s7, GPGPU_EVAL_IN0(s11)
	add s7, s7, t0
	ld s5, GPGPU_EVAL_D6(s11)
	slli s5, s5, 2
	ld t0, GPGPU_EVAL_D7(s11)
	mul t0, t0, s6
	ld t1, GPGPU_EVAL_D9(s11)
	mul t1, t1, s4
	add t0, t0, t1
	slli t0, t0, 2
	ld s8, GPGPU_EVAL_IN1(s11)
	add s8, s8, t0
	ld s9, GPGPU_EVAL_D8(s11)
	slli s9, s9, 2
model_bmm_setup_done:
	addi a0, zero, 0
	flw f0, GPGPU_EVAL_ZERO(s11)
	beq s2, zero, model_bmm_finish
model_bmm_loop:
	flw f1, 0(s7)
	flw f2, 0(s8)
	fmadd.s f0, f1, f2, f0
	add s7, s7, s5
	add s8, s8, s9
	addi a0, a0, 1
	bltu a0, s2, model_bmm_loop
model_bmm_finish:
	jal zero, model_store_f32

model_embedding:
	ld s1, GPGPU_EVAL_D0(s11)
	divu t0, s0, s1
	remu t1, s0, s1
	slli t0, t0, 3
	ld t2, GPGPU_EVAL_IN0(s11)
	add t2, t2, t0
	ld t0, 0(t2)
	mul t2, t0, s1
	add t2, t2, t1
	ld t3, GPGPU_EVAL_IN1(s11)
	add t3, t3, t2
	lb t1, 0(t3)
	fcvt.s.w f0, t1
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN2(s11)
	add t1, t1, t0
	flw f1, 0(t1)
	fmul.s f0, f0, f1
	jal zero, model_store_f32

/* 周期广播只寻址真实紧凑子数组，不把 CPU 原始 stride 当作文件 stride。 */
model_binary:
	ld t0, GPGPU_EVAL_D0(s11)
	ld t1, GPGPU_EVAL_D1(s11)
	remu t0, s0, t0
	remu t1, s0, t1
	slli t0, t0, 2
	slli t1, t1, 2
	ld t2, GPGPU_EVAL_IN0(s11)
	ld t3, GPGPU_EVAL_IN1(s11)
	add t2, t2, t0
	add t3, t3, t1
	flw f0, 0(t2)
	flw f1, 0(t3)
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_MUL
	beq t0, t1, model_binary_mul
	addi t1, zero, GPGPU_EVAL_SUB
	beq t0, t1, model_binary_sub
	addi t1, zero, GPGPU_EVAL_DIV
	beq t0, t1, model_binary_div
	fadd.s f0, f0, f1
	jal zero, model_store_f32
model_binary_mul:
	fmul.s f0, f0, f1
	jal zero, model_store_f32
model_binary_sub:
	fsub.s f0, f0, f1
	jal zero, model_store_f32
model_binary_div:
	fdiv.s f0, f0, f1
	jal zero, model_store_f32

model_scalar:
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f0, 0(t0)
	flw f1, GPGPU_EVAL_D0(s11)
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_ADD_SCALAR
	beq t0, t1, model_scalar_add
	fmul.s f0, f0, f1
	jal zero, model_store_f32
model_scalar_add:
	fadd.s f0, f0, f1
	jal zero, model_store_f32

/* f16 跨 exp helper 保留原输入；helper 不覆盖 s0/s10/s11。 */
model_activation:
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f16, 0(t0)
	flw f0, GPGPU_EVAL_ZERO(s11)
	fsub.s f0, f0, f16
	jal ra, model_exp
	flw f1, GPGPU_EVAL_ONE(s11)
	fadd.s f0, f0, f1
	fdiv.s f0, f1, f0
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_SILU
	bne t0, t1, model_activation_finish
	fmul.s f0, f16, f0
model_activation_finish:
	jal zero, model_store_f32

/*
 * 归约先按 row=outer*inner+inner_idx 找到首元素，再沿 inner 间隔读取。
 * Softmax 的指数单独逐元素派发，避免一行重复计算完整 softmax。
 */
model_reduce:
	ld s1, GPGPU_EVAL_D0(s11)
	ld t0, GPGPU_EVAL_D1(s11)
	divu t1, s0, t0
	remu t2, s0, t0
	mul t1, t1, s1
	mul t1, t1, t0
	add t1, t1, t2
	slli t1, t1, 2
	slli s2, t0, 2
	ld s3, GPGPU_EVAL_IN0(s11)
	add s3, s3, t1
	addi a0, zero, 0
	ld s4, GPGPU_EVAL_OP(s11)
	addi t0, zero, GPGPU_EVAL_REDUCE_MAX
	beq s4, t0, model_reduce_max_setup
	flw f0, GPGPU_EVAL_ZERO(s11)
	addi t0, zero, GPGPU_EVAL_REDUCE_RMS
	beq s4, t0, model_reduce_rms_loop
	addi t0, zero, GPGPU_EVAL_DFL_PROJECT
	beq s4, t0, model_reduce_dfl_loop
model_reduce_sum_loop:
	flw f1, 0(s3)
	fadd.s f0, f0, f1
	add s3, s3, s2
	addi a0, a0, 1
	bltu a0, s1, model_reduce_sum_loop
	jal zero, model_store_f32
model_reduce_max_setup:
	flw f0, GPGPU_EVAL_NEGINF(s11)
model_reduce_max_loop:
	flw f1, 0(s3)
	fmax.s f0, f0, f1
	add s3, s3, s2
	addi a0, a0, 1
	bltu a0, s1, model_reduce_max_loop
	jal zero, model_store_f32
model_reduce_rms_loop:
	flw f1, 0(s3)
	fmul.s f1, f1, f1
	fadd.s f0, f0, f1
	add s3, s3, s2
	addi a0, a0, 1
	bltu a0, s1, model_reduce_rms_loop
	fcvt.s.w f1, s1
	fdiv.s f0, f0, f1
	flw f1, GPGPU_EVAL_D2(s11)
	fadd.s f0, f0, f1
	fsqrt.s f0, f0
	flw f1, GPGPU_EVAL_ONE(s11)
	fdiv.s f0, f1, f0
	jal zero, model_store_f32
model_reduce_dfl_loop:
	flw f1, 0(s3)
	fcvt.s.w f2, a0
	fmadd.s f0, f1, f2, f0
	add s3, s3, s2
	addi a0, a0, 1
	bltu a0, s1, model_reduce_dfl_loop
	jal zero, model_store_f32

model_row_apply:
	ld t0, GPGPU_EVAL_D0(s11)
	ld t1, GPGPU_EVAL_D1(s11)
	mul t0, t0, t1
	divu t0, s0, t0
	mul t0, t0, t1
	remu t1, s0, t1
	add t0, t0, t1
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN1(s11)
	add t1, t1, t0
	flw f1, 0(t1)
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f0, 0(t0)
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_DIV_ROW
	beq t0, t1, model_row_div
	fsub.s f0, f0, f1
	jal ra, model_exp
	jal zero, model_store_f32
model_row_div:
	fdiv.s f0, f0, f1
	jal zero, model_store_f32

model_rms_apply:
	ld s1, GPGPU_EVAL_D0(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	divu t0, s0, s2
	remu t0, t0, s1
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN1(s11)
	add t1, t1, t0
	flw f2, 0(t1)
	mul t0, s1, s2
	divu t0, s0, t0
	mul t0, t0, s2
	remu t1, s0, s2
	add t0, t0, t1
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN2(s11)
	add t1, t1, t0
	flw f1, 0(t1)
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f0, 0(t0)
	fmul.s f0, f0, f1
	fmul.s f0, f0, f2
	jal zero, model_store_f32

model_batchnorm:
	ld t0, GPGPU_EVAL_D0(s11)
	ld t1, GPGPU_EVAL_D1(s11)
	divu t1, s0, t1
	remu t1, t1, t0
	slli t3, t1, 2
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f0, 0(t0)
	ld t0, GPGPU_EVAL_IN3(s11)
	add t0, t0, t3
	flw f1, 0(t0)
	fsub.s f0, f0, f1
	ld t0, GPGPU_EVAL_IN4(s11)
	add t0, t0, t3
	flw f1, 0(t0)
	flw f2, GPGPU_EVAL_D2(s11)
	fadd.s f1, f1, f2
	fsqrt.s f1, f1
	fdiv.s f0, f0, f1
	ld t0, GPGPU_EVAL_IN1(s11)
	add t0, t0, t3
	flw f1, 0(t0)
	fmul.s f0, f0, f1
	ld t0, GPGPU_EVAL_IN2(s11)
	add t0, t0, t3
	flw f1, 0(t0)
	fadd.s f0, f0, f1
	jal zero, model_store_f32

/*
 * 卷积按 ic、ky、kx 遍历一行 OIHW 权重。s8 保存当前输入通道平面，s9
 * 是单调前进的权重指针。整行 padding 直接跳过 Kw 项；横向越界只跳过
 * 对应乘加，不读取越界输入。负坐标以无符号比较一起排除。
 *
 * s1/s2=Win/Hin，s3/s4=Kh/Kw，s5/s6=Dh/Dw，s7=Cin/groups；
 * a0/a1=输入窗口左上 y/x，a2/a3/a4=ic/ky/kx，a5/a6=当前 y/x，
 * a7=当前输入行，t2=输入平面字节数，t3=权重行字节数，t5=oc。
 */
model_conv2d:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_conv2d_scalar
	jal zero, model_tensor_conv
model_conv2d_scalar:
	ld t0, GPGPU_EVAL_D5(s11)
	remu t1, s0, t0
	divu t6, s0, t0
	ld t0, GPGPU_EVAL_D4(s11)
	remu a0, t6, t0
	divu t6, t6, t0
	ld t4, GPGPU_EVAL_D3(s11)
	remu t5, t6, t4
	divu t6, t6, t4
	ld a1, GPGPU_EVAL_D9(s11)
	mul a1, a1, t1
	ld t0, GPGPU_EVAL_D11(s11)
	sub a1, a1, t0
	ld t0, GPGPU_EVAL_D8(s11)
	mul a0, a0, t0
	ld t0, GPGPU_EVAL_D10(s11)
	sub a0, a0, t0
	ld s1, GPGPU_EVAL_D2(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	ld s3, GPGPU_EVAL_D6(s11)
	ld s4, GPGPU_EVAL_D7(s11)
	ld s5, GPGPU_EVAL_D12(s11)
	ld s6, GPGPU_EVAL_D13(s11)
	ld t4, GPGPU_EVAL_D0(s11)
	ld t0, GPGPU_EVAL_D14(s11)
	divu s7, t4, t0
	ld t1, GPGPU_EVAL_D3(s11)
	divu t1, t1, t0
	divu t0, t5, t1
	mul t6, t6, t4
	mul t0, t0, s7
	add t6, t6, t0
	mul t2, s1, s2
	slli t2, t2, 2
	mul t6, t6, t2
	ld s8, GPGPU_EVAL_IN0(s11)
	add s8, s8, t6
	mul t0, t5, s7
	mul t0, t0, s3
	mul t0, t0, s4
	slli t0, t0, 2
	ld s9, GPGPU_EVAL_IN1(s11)
	add s9, s9, t0
	slli t3, s4, 2
	addi a2, zero, 0
	flw f0, GPGPU_EVAL_ZERO(s11)
model_conv_ic:
	addi a3, zero, 0
	addi a5, a0, 0
model_conv_ky:
	bgeu a5, s2, model_conv_skip_row
	mul a7, a5, s1
	slli a7, a7, 2
	add a7, a7, s8
	addi a6, a1, 0
	addi a4, zero, 0
model_conv_kx:
	bgeu a6, s1, model_conv_skip_tap
	slli t0, a6, 2
	add t0, t0, a7
	flw f1, 0(t0)
	flw f2, 0(s9)
	fmadd.s f0, f1, f2, f0
model_conv_skip_tap:
	addi s9, s9, 4
	add a6, a6, s6
	addi a4, a4, 1
	bltu a4, s4, model_conv_kx
	jal zero, model_conv_next_row
model_conv_skip_row:
	add s9, s9, t3
model_conv_next_row:
	add a5, a5, s5
	addi a3, a3, 1
	bltu a3, s3, model_conv_ky
	add s8, s8, t2
	addi a2, a2, 1
	bltu a2, s7, model_conv_ic
	ld t0, GPGPU_EVAL_IN2(s11)
	beq t0, zero, model_conv_finish
	slli t1, t5, 2
	add t0, t0, t1
	flw f1, 0(t0)
	fadd.s f0, f0, f1
model_conv_finish:
	jal zero, model_store_f32

model_maxpool2d:
	ld t0, GPGPU_EVAL_D4(s11)
	remu t1, s0, t0
	divu t2, s0, t0
	ld t0, GPGPU_EVAL_D3(s11)
	remu s8, t2, t0
	divu t2, t2, t0
	ld t0, GPGPU_EVAL_D8(s11)
	mul s9, t1, t0
	ld t0, GPGPU_EVAL_D10(s11)
	sub s9, s9, t0
	ld t0, GPGPU_EVAL_D7(s11)
	mul s8, s8, t0
	ld t0, GPGPU_EVAL_D9(s11)
	sub s8, s8, t0
	ld s1, GPGPU_EVAL_D2(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	ld s3, GPGPU_EVAL_D5(s11)
	ld s4, GPGPU_EVAL_D6(s11)
	ld s5, GPGPU_EVAL_D11(s11)
	ld s6, GPGPU_EVAL_D12(s11)
	mul t2, t2, s1
	mul t2, t2, s2
	slli t2, t2, 2
	ld s7, GPGPU_EVAL_IN0(s11)
	add s7, s7, t2
	addi a0, zero, 0
	addi a2, s8, 0
	flw f0, GPGPU_EVAL_NEGINF(s11)
model_pool_ky:
	bgeu a2, s2, model_pool_next_row
	mul a4, a2, s1
	slli a4, a4, 2
	add a4, a4, s7
	addi a1, zero, 0
	addi a3, s9, 0
model_pool_kx:
	bgeu a3, s1, model_pool_next_tap
	slli t0, a3, 2
	add t0, t0, a4
	flw f1, 0(t0)
	fmax.s f0, f0, f1
model_pool_next_tap:
	add a3, a3, s6
	addi a1, a1, 1
	bltu a1, s4, model_pool_kx
model_pool_next_row:
	add a2, a2, s5
	addi a0, a0, 1
	bltu a0, s3, model_pool_ky
	jal zero, model_store_f32

model_nearest2d:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_nearest2d_scalar
	jal zero, model_tensor_nearest
model_nearest2d_scalar:
	ld s1, GPGPU_EVAL_D0(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	ld s3, GPGPU_EVAL_D2(s11)
	ld s4, GPGPU_EVAL_D3(s11)
	remu t0, s0, s4
	divu t1, s0, s4
	remu t2, t1, s3
	divu t1, t1, s3
	mul t0, t0, s2
	divu t0, t0, s4
	mul t2, t2, s1
	divu t2, t2, s3
	mul t1, t1, s1
	add t1, t1, t2
	mul t1, t1, s2
	add t1, t1, t0
	slli t1, t1, 2
	ld t0, GPGPU_EVAL_IN0(s11)
	add t0, t0, t1
	flw f0, 0(t0)
	jal zero, model_store_f32

/* 所有输入共享 outer/inner；只在拼接轴上的宽度不同。 */
model_concat:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_concat_scalar
	jal zero, model_tensor_concat
model_concat_scalar:
	ld s1, GPGPU_EVAL_D0(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	ld s3, GPGPU_EVAL_D2(s11)
	remu s4, s0, s1
	divu t0, s0, s1
	remu s5, t0, s2
	divu s6, t0, s2
	addi s7, zero, 0
	addi s8, s11, GPGPU_EVAL_D3
	addi s9, s11, GPGPU_EVAL_IN0
model_concat_select:
	bgeu s7, s3, model_concat_invalid
	ld t0, 0(s8)
	bltu s5, t0, model_concat_selected
	sub s5, s5, t0
	addi s8, s8, GPGPU_EVAL_SLOT_BYTES
	addi s9, s9, GPGPU_EVAL_SLOT_BYTES
	addi s7, s7, 1
	jal zero, model_concat_select
model_concat_selected:
	mul t0, s6, t0
	add t0, t0, s5
	mul t0, t0, s1
	add t0, t0, s4
	slli t0, t0, 2
	ld t1, 0(s9)
	add t1, t1, t0
	flw f0, 0(t1)
	jal zero, model_store_f32
model_concat_invalid:
	jal zero, model_done

model_copy:
	ld t0, GPGPU_EVAL_AUX2(s11)
	addi t1, zero, GPGPU_EVAL_BACKEND_BLOCK
	bne t0, t1, model_copy_scalar
	jal zero, model_tensor_copy
model_copy_scalar:
	ld s1, GPGPU_EVAL_D0(s11)
	ld s4, GPGPU_EVAL_D1(s11)
	addi s3, s0, 0
	beq s4, zero, model_copy_offset_done
	addi s2, s0, 0
	addi s3, zero, 0
	addi s5, s11, GPGPU_EVAL_D2
	addi t0, s4, -1
	slli t0, t0, 3
	add s5, s5, t0
	slli t0, s4, 3
	add s6, s5, t0
model_copy_offset_loop:
	ld t0, 0(s5)
	remu t1, s2, t0
	divu s2, s2, t0
	ld t0, 0(s6)
	mul t1, t1, t0
	add s3, s3, t1
	addi s5, s5, -GPGPU_EVAL_SLOT_BYTES
	addi s6, s6, -GPGPU_EVAL_SLOT_BYTES
	addi s4, s4, -1
	bne s4, zero, model_copy_offset_loop
model_copy_offset_done:
	addi t0, zero, GPGPU_EVAL_COPY_F32
	beq s1, t0, model_copy_f32
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s3, 3
	add t0, t0, t1
	ld t0, 0(t0)
	addi t1, zero, GPGPU_EVAL_COPY_I64
	beq s1, t1, model_copy_i64
	addi t1, zero, GPGPU_EVAL_COPY_I64_TO_F32
	bne s1, t1, model_copy_invalid
	fcvt.s.w f0, t0
	jal zero, model_store_f32
model_copy_f32:
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s3, 2
	add t0, t0, t1
	flw f0, 0(t0)
	jal zero, model_store_f32
model_copy_i64:
	jal zero, model_store_i64
model_copy_invalid:
	jal zero, model_done

model_rope:
	ld s1, GPGPU_EVAL_D0(s11)
	ld s2, GPGPU_EVAL_D1(s11)
	ld s3, GPGPU_EVAL_D2(s11)
	remu s4, s0, s3
	divu t0, s0, s3
	remu a0, t0, s2
	divu t0, t0, s2
	remu a1, t0, s1
	divu a2, t0, s1
	mul t0, a2, s2
	add t0, t0, a0
	mul t0, t0, s3
	add t0, t0, s4
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN1(s11)
	add t1, t1, t0
	flw f1, 0(t1)
	ld t1, GPGPU_EVAL_IN2(s11)
	add t1, t1, t0
	flw f2, 0(t1)
	addi s7, s0, 0
	addi s8, zero, 1
	ld t0, GPGPU_EVAL_D7(s11)
	beq t0, zero, model_rope_offset_done
	ld t0, GPGPU_EVAL_D3(s11)
	mul s7, a2, t0
	ld t0, GPGPU_EVAL_D4(s11)
	mul t0, a1, t0
	add s7, s7, t0
	ld t0, GPGPU_EVAL_D5(s11)
	mul t0, a0, t0
	add s7, s7, t0
	ld s8, GPGPU_EVAL_D6(s11)
	mul t0, s4, s8
	add s7, s7, t0
model_rope_offset_done:
	ld s5, GPGPU_EVAL_IN0(s11)
	slli t0, s7, 2
	add t0, t0, s5
	flw f0, 0(t0)
	fmul.s f0, f0, f1
	srli s6, s3, 1
	mul s8, s8, s6
	bltu s4, s6, model_rope_first_half
	sub t0, s7, s8
	slli t0, t0, 2
	add t0, t0, s5
	flw f1, 0(t0)
	fmul.s f1, f1, f2
	fadd.s f0, f0, f1
	jal zero, model_store_f32
model_rope_first_half:
	add t0, s7, s8
	slli t0, t0, 2
	add t0, t0, s5
	flw f1, 0(t0)
	fmul.s f1, f1, f2
	fsub.s f0, f0, f1
	jal zero, model_store_f32

/* 先形成 x1/y1 和 x2/y2，再求中心与宽高，保留真实算子的运算顺序。 */
model_boxdecode:
	ld s1, GPGPU_EVAL_D0(s11)
	remu s2, s0, s1
	divu t0, s0, s1
	andi s3, t0, 3
	srli s4, t0, 2
	andi s5, s3, 1
	slli t0, s4, 1
	add t0, t0, s5
	mul t0, t0, s1
	add t0, t0, s2
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN1(s11)
	add t1, t1, t0
	flw f1, 0(t1)
	slli t0, s4, 2
	add t0, t0, s5
	mul t0, t0, s1
	add t0, t0, s2
	slli t0, t0, 2
	ld t1, GPGPU_EVAL_IN0(s11)
	add t1, t1, t0
	flw f2, 0(t1)
	slli t0, s1, 3
	add t1, t1, t0
	flw f3, 0(t1)
	fsub.s f2, f1, f2
	fadd.s f3, f1, f3
	ld t0, GPGPU_EVAL_D1(s11)
	bne t0, zero, model_box_xyxy
	addi t0, zero, 2
	bgeu s3, t0, model_box_size
	fadd.s f0, f2, f3
	flw f1, GPGPU_EVAL_TWO(s11)
	fdiv.s f0, f0, f1
	jal zero, model_box_scale
model_box_size:
	fsub.s f0, f3, f2
	jal zero, model_box_scale
model_box_xyxy:
	addi t0, zero, 2
	bgeu s3, t0, model_box_right_bottom
	fmv.x.w t0, f2
	fmv.w.x f0, t0
	jal zero, model_box_scale
model_box_right_bottom:
	fmv.x.w t0, f3
	fmv.w.x f0, t0
model_box_scale:
	ld t0, GPGPU_EVAL_IN2(s11)
	beq t0, zero, model_box_finish
	slli t1, s2, 2
	add t0, t0, t1
	flw f1, 0(t0)
	fmul.s f0, f0, f1
model_box_finish:
	jal zero, model_store_f32

model_trig:
	ld t0, GPGPU_EVAL_IN0(s11)
	slli t1, s0, 2
	add t0, t0, t1
	flw f0, 0(t0)
	jal ra, model_sincos
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_COS
	bne t0, t1, model_trig_finish
	fmv.x.w t0, f1
	fmv.w.x f0, t0
model_trig_finish:
	jal zero, model_store_f32

model_arange:
	ld t0, GPGPU_EVAL_D2(s11)
	addi t1, zero, GPGPU_EVAL_TYPE_I64
	beq t0, t1, model_arange_i64
	fcvt.s.w f0, s0
	flw f1, GPGPU_EVAL_D1(s11)
	flw f2, GPGPU_EVAL_D0(s11)
	fmadd.s f0, f0, f1, f2
	jal zero, model_store_f32
model_arange_i64:
	ld t0, GPGPU_EVAL_D0(s11)
	ld t1, GPGPU_EVAL_D1(s11)
	mul t1, s0, t1
	add t0, t0, t1
	jal zero, model_store_i64

model_full:
	ld t0, GPGPU_EVAL_D2(s11)
	addi t1, zero, GPGPU_EVAL_TYPE_I64
	beq t0, t1, model_full_i64
	flw f0, GPGPU_EVAL_D0(s11)
	jal zero, model_store_f32
model_full_i64:
	ld t0, GPGPU_EVAL_D0(s11)
	jal zero, model_store_i64

/*
 * 块矩阵路径：s0 是线性块号，s11 是通用命令 GPU 地址，a0 是 K 起点。
 * 每个工作项从 K=0 依次推进 TILE_K；指令负责首块清零、后续累加与末块 bias。
 * Conv 的归约宽度是 Cin/groups*Kh*Kw，gather 在指令内部完成，不建 im2col。
 * 全局 COUNT guard 已排除补齐 Lane；块内行列尾部由指令按真实维度排除。
 */
model_tensor_linear:
	ld s1, GPGPU_EVAL_D2(s11)
	addi a0, zero, 0
	ld t0, GPGPU_EVAL_OP(s11)
	addi t1, zero, GPGPU_EVAL_LINEAR_W8
	beq t0, t1, model_tensor_linear_w8_loop
model_tensor_linear_f32_loop:
	tmat.f32 s11, s0, a0
	addi a0, a0, GPGPU_TENSOR_TILE_K
	bltu a0, s1, model_tensor_linear_f32_loop
	ebreak
model_tensor_linear_w8_loop:
	tmat.w8 s11, s0, a0
	addi a0, a0, GPGPU_TENSOR_TILE_K
	bltu a0, s1, model_tensor_linear_w8_loop
	ebreak

model_tensor_bmm:
	ld s1, GPGPU_EVAL_D3(s11)
	addi a0, zero, 0
model_tensor_bmm_loop:
	tmat.bmm s11, s0, a0
	addi a0, a0, GPGPU_TENSOR_TILE_K
	bltu a0, s1, model_tensor_bmm_loop
	ebreak

model_tensor_conv:
	ld s1, GPGPU_EVAL_D0(s11)
	ld t0, GPGPU_EVAL_D14(s11)
	divu s1, s1, t0
	ld t0, GPGPU_EVAL_D6(s11)
	mul s1, s1, t0
	ld t0, GPGPU_EVAL_D7(s11)
	mul s1, s1, t0
	addi a0, zero, 0
model_tensor_conv_loop:
	tmat.conv s11, s0, a0
	addi a0, a0, GPGPU_TENSOR_TILE_K
	bltu a0, s1, model_tensor_conv_loop
	ebreak

/* 布局每项只发一条指令；rs3 由汇编器固定编码为 x0。 */
model_tensor_copy:
	tvec.copy s11, s0
	ebreak
model_tensor_concat:
	tvec.concat s11, s0
	ebreak
model_tensor_nearest:
	tvec.nearest s11, s0
	ebreak

#include "model_math.inc"
