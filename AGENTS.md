# 仓库约束

## 项目结构与模块划分

QEMU Camp 2026 包含 RISC-V CPU、SoC、GPGPU 和 Rust 实验。
指令模拟位于 `target/riscv/`，G233 板级实现为 `hw/riscv/g233.c`，
设备模型位于 `hw/`，头文件位于 `include/`。
GPGPU 实现在 `hw/gpgpu/`，Rust 设备实现在 `rust/hw/`。

实验测试位于 `tests/gevico/tcg/` 和 `tests/gevico/qtest/`；
GPGPU 测试位于 `tests/qtest/gpgpu-regs-test.c` 与 `tests/qtest/gpgpu-cp-test.c`。
其他测试位于 `tests/unit/` 和 `tests/functional/`。
构建与内部实现说明见 `README.md`、`README_zh.md` 和 `docs/devel/`。
固件位于 `pc-bios/`，构建产物统一放在 `build/`。

## 构建、测试与开发命令

在仓库根目录运行命令。构建需要 QEMU 的宿主机依赖、
`libjson-c-dev`、`libcmocka-dev` 和固件使用的 `riscv64-unknown-elf-gcc`；
GPGPU 不需要 Rust 和 `bindgen-cli`。

- `make -f Makefile.gpgpu configure`：在 `build/arm64-qemu/` 配置 aarch64 Host 与 riscv64 CP，不需要客体内核。
- `make -f Makefile.gpgpu build JOBS=4`：构建两个 QEMU、两个 QTest 程序和 RV64 CP 固件；首次构建自动配置。
- `make -f Makefile.gpgpu test-gpgpu-qtest`：运行 GPGPU 寄存器与 CP 队列 QTest。
- `make -f Makefile.gpgpu test-gpgpu-stack`：在 Linux 客体中验证 GPUVM 开、关两种模式，需要先准备客体环境。
- `build/arm64-qemu/qemu-system-riscv64 -machine help`：列出可用机器类型。

## 脚本、校验与验证的精简

脚本、测试和验证项只覆盖最核心的行为。检查写得过多、过细，调整一个本该修改的参数也会连带报错，
重复的检查还要在多处同步维护。

本节约束仓库中保存的脚本、测试用例与断言、脚本对环境和参数的前置检查，以及计划中的验证项。
产品代码中的输入检查与错误处理、其他规则要求的交付前核对，以及已完成验证的实际记录，
不在本节范围内。

- 默认不新增、不修改脚本、测试用例和验证项。检查优先并入已有的构建、测试入口或验证项，
  或在终端用单条命令完成。只有检查保护核心功能，且现有入口和验证项无法覆盖时，才新增；
  当前任务必须修改时，只改必需的部分。新增或修改的内容保持短小，一个脚本、用例或验证项
  只检查一件事，理由在回复中说明；
- 每项检查都要能说出它防止什么错误。只断言对外可见的结果、接口约定和用户明确要求的结论，
  不为措辞、排版、内部中间步骤、日志内容或可调参数的具体取值写断言。计划中的验证项
  同样只写核心结果，不写死会随参数或用例增减而变化的数量；
- 参数、常量和路径引用其唯一定义处，或从中推导，不在脚本、测试和验证项中硬编码副本；
  同一事实只在一处检查；
- 一次性的核对直接在终端运行，在回复中报告结果，不留在仓库中；
- 已有的脚本、用例或验证项妨碍正常的参数调整，或与其他检查重复时，直接删减或合并，
  不继续追加特例，并在回复中说明。

## 代码修改与语言约束

修改代码时，尽量缩小影响范围和改动量，沿用现有架构、模块职责、接口和执行模型。
只修改完成当前任务必需的内容，不顺手重构、移动代码或改名；
必须调整架构或接口时，先说明原因和必要范围。

与用户的交互、代码注释、文档说明以及提交标题和正文均使用中文。
技术术语可保留通用英文写法；标识符、路径、命令、协议字段、汇编助记符、
机器读取的标记和许可证声明保留原文，不因中文化改变其含义或行为。

## 代码风格与命名

新增和修改的 C 代码遵循本节规则。各小节先写规则，再给示例；示例中的
`ring_*`、`GPGPURing*`、`GPGPU_RING_*` 等名称只用于说明写法，仓库中并不存在。

### 适用范围与格式

`hw/gpgpu/` 和 `software/` 下的自有 C 代码采用 Linux 风格，以目录内的
`.editorconfig` 和 `.clang-format` 为准：使用 8 列制表符，函数左花括号另起一行，
结构体每行只声明一个成员，每行尽量不超过 100 列，续行与左括号对齐。
其他目录遵循 `.editorconfig` 和 `docs/devel/style.rst`：C 使用四个空格缩进，
每行尽量不超过 80 列。缩进和列宽按目录区分，其余规则对全部新增 C 代码适用。
Makefile 配方使用制表符，文件使用 LF 换行。固定版本的外部内核补丁、配置和
打包文件保持原貌，不参与批量格式化。

类型命名沿用所在层的写法。QEMU 设备、CP 固件和用户库使用
`typedef struct CamelCase`；Linux 驱动使用 `struct gpgpu_xxx` 和 `u32`、`u64`，
不加 `typedef`。Python、Rust 和汇编保持各自的格式规则，入口处写明用途。

### 文件结构

- 第一行是 SPDX 许可证声明，`.c` 用 `//`，`.h` 用 `/* */`；
- 随后用块注释写“模块：职责”；头文件还要说明谁调用本模块、数据经过哪些层，
  路径有分支时画出简图；
- QEMU 设备的 C 源文件首先包含 `qemu/osdep.h`，再依次包含 `qemu/*`、`hw/gpgpu/*`
  和本目录头文件；
- `.c` 文件开头放私有宏和常量表，被调用的静态函数放在调用者之前，不写前置声明；
  寄存器读写放在访问路径之后，握手与复位放在文件末尾。

```c
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Ring：IH 记录的写入、发布与故障记录
 *
 * CP 提交记录和设备复位都经过本模块：
 *
 *   enable=1   记录 ─校验─▶ Host Ring ─▶ 发布 W ─▶ 调用者发中断
 *   enable=0   拒绝提交，记录 CONTEXT 故障
 */
#ifndef HW_GPGPU_IRQ_RING_H
#define HW_GPGPU_IRQ_RING_H
```

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ring：IH 记录的写入、发布与故障记录
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "ring.h"
```

### 宏与常量

每个宏和枚举值都用注释写清含义和单位。步长、范围从已有寄存器宏或其他宏推导，
不直接写魔数。模块私有宏使用模块短前缀，跨文件或协议宏使用 `GPGPU_` 前缀。
按编号查找的固定数据用 `static const` 数组，注释说明下标与含义的对应关系。

```c
/* 每条 Ring 的记录条数，等于 W 低 7 位能表示的槽数。 */
#define RING_ENTRIES 128
/* 每条记录的字节数。 */
#define RING_ENTRY_SIZE 32
/* 每条 Ring 的寄存器组在 BAR0 中占用的字节数。 */
#define RING_STRIDE (GPGPU_REG_RING(1) - GPGPU_REG_RING(0))

/* 各记录类型的有效载荷字节数，下标为 GPGPU_RING_TYPE_*。 */
static const unsigned int ring_payload_size[] = { 0, 8, 16 };
```

### 命名

函数和变量使用 `snake_case`，宏使用大写。

- 对外函数写成 `gpgpu_<模块>_<动作>`，静态函数使用模块短前缀或直接描述动作；
- 返回 `bool` 的判断函数以 `_valid`、`_ok`、`_enabled`、`_busy` 结尾；
- 根据寄存器偏移查归属的函数以 `_of` 结尾，返回编号，经输出参数给出组内偏移；
- 参数名在同一模块内保持一致：`s` 为设备实例，`ctx` 为上下文，`reg` 为寄存器偏移，
  `field` 为组内偏移。

```c
void gpgpu_ring_reset(GPGPUState *s);
static bool ring_addr_valid(const GPGPURing *ring, uint64_t addr);
static bool ring_busy(GPGPUState *s, unsigned int id);
static unsigned int ring_of(hwaddr reg, hwaddr *field);
```

### 注释

注释写原因、约束和失败结果，不逐句复述代码。按函数复杂度分三级。

第一级是对外接口和结构体。kernel-doc 写在头文件，`.c` 中不再重复。
参数说明写清取值范围、地址类型和调用者需持有的锁；结构体成员写明单位、
对齐和特殊值。

```c
/**
 * struct GPGPURing - 一条 IH Ring 的状态
 * @base: Ring 的 Host 物理地址，按 4 KiB 对齐。
 * @rptr_addr: Host 回报消费进度的 8 字节槽地址，按 8 字节对齐。
 * @wptr: 下一条待写记录的编号，32 位回绕。
 * @enabled: Host 配置完成后置位；置位期间 @base 只读。
 */
typedef struct GPGPURing {
	uint64_t base;
	uint64_t rptr_addr;
	uint32_t wptr;
	bool enabled;
} GPGPURing;

/**
 * gpgpu_ring_push() - 向 Ring 写入并发布一条记录
 * @s: 设备实例，调用者持有主锁。
 * @id: Ring 编号；越界时返回 false。
 * @rec: 32 字节记录，按小端写入。
 *
 * Ring 已满时不覆盖旧记录。
 *
 * Return: 记录已写入且 W 已发布时为 true。
 */
bool gpgpu_ring_push(GPGPUState *s, unsigned int id, const void *rec);
```

第二级是一两句话能说清的静态函数，上方只写一行 `/* */`，说明它检查什么或返回什么。

```c
/* 记录地址必须按 32 字节对齐，且整条记录落在 Ring 内。 */
static bool ring_addr_valid(const GPGPURing *ring, uint64_t addr)
```

第三级是包含多个步骤，或涉及地址拆分、锁、在途计数、发布顺序、对象寿命的静态函数。
函数头用 `/*` 开头，沿用 `name() - 一句话` 和 `Return:` 的写法，依次说明：

1. 作用和调用者，以及为什么要分步；
2. 涉及位段或地址换算时，给出位段图和带具体数值的例子；
3. 编号步骤，每步写动作、原因和失败结果；
4. 本函数不负责、交给谁完成的工作；
5. `Return:`。

函数体内用 `/* 1. … */` 标出相同编号，与函数头一一对应。

```c
/*
 * ring_submit() - 校验并发布一条记录
 *
 * CP 提交 IH 记录时调用。Host 只读取编号小于 W 的记录，所以必须先写完
 * 记录内容，再推进 W，否则 Host 可能读到半条记录。记录位置由 W 换算：
 *
 *   W 位     31..7       6..0
 *   用途     回绕次数    槽号（Ring 共 128 条）
 *
 * 例：W = 0x0000_0085 写入槽 5，地址为 base + 5 × 32 = base + 0xa0。
 *
 * 1. 检查 Ring 已启用、未满，否则记 CONTEXT 故障；
 * 2. 写入记录内容，失败记 STORAGE 故障，W 保持不变；
 * 3. 内容写成功后推进 W。
 *
 * 函数不发中断，由调用者在返回成功后通知 Host。
 *
 * Return: 0 表示成功，否则为 GPGPU_RING_FAULT_*。
 */
static uint32_t ring_submit(GPGPUState *s, GPGPURing *ring, void *rec)
{
	uint64_t addr;

	/* 1. 未启用或已满时不写任何内容。 */
	if (!ring->enabled || ring_full(ring))
		return GPGPU_RING_FAULT_CONTEXT;

	/* 2. 按 W 的低 7 位定位槽号，写入整条记录。 */
	addr = ring->base + (ring->wptr % RING_ENTRIES) * RING_ENTRY_SIZE;
	if (gpgpu_phys_rw(s, addr, rec, RING_ENTRY_SIZE, true) != MEMTX_OK)
		return GPGPU_RING_FAULT_STORAGE;

	/* 3. 记录已写入，Host 此后可以读取这一条。 */
	ring->wptr++;
	return 0;
}
```

注释中的长十六进制数按 4 位分组书写，如 `0x8000_1234`；代码中的常量保留 C 语法。

### 控制流

- 函数入口先做保护性检查，不满足条件时立即返回；
- 单语句的 `if`、`for` 体不加花括号；
- 多个失败点共用同一段收尾时，用 `goto fail`、`goto invalid` 一类标签集中处理，
  标签前留一个空行；
- 检查、处理、收尾三段之间用空行分开；
- 局部变量声明在所在块的开头，与第一条语句之间空一行；循环计数器在 `for` 中声明；
  只在循环内使用的变量声明在循环体内；
- 判断零值和空指针写 `!x`；
- 整体给结构体赋值用复合字面量，清零用 `memset`；
- `switch` 的 `case` 不缩进，`default` 分支给出明确的返回值；
- 只读访问的结构体指针加 `const`；
- 优先复用已有辅助函数，如 `MIN`、`is_power_of_2`，不重写相同逻辑。

```c
static bool ring_write(GPGPUState *s, unsigned int id, uint64_t addr, void *buf, size_t len)
{
	uint32_t reason;

	if (!ring_addr_valid(&s->ring[id], addr)) {
		reason = GPGPU_RING_FAULT_RANGE;
		goto fail;
	}
	if (gpgpu_phys_rw(s, addr, buf, len, true) != MEMTX_OK) {
		reason = GPGPU_RING_FAULT_STORAGE;
		goto fail;
	}
	return true;

fail:
	ring_record_fault(s, id, addr, reason);
	return false;
}

static uint32_t ring_reg_read(const GPGPURing *ring, hwaddr field)
{
	switch (field) {
	case GPGPU_RING_WPTR:
		return ring->wptr;
	case GPGPU_RING_ENABLE:
		return ring->enabled;
	default:
		return 0;
	}
}
```

### 状态与错误

- 模块内部的检查函数用 `0` 表示成功、非零值表示具体原因；对外接口返回 `bool`，
  Linux 驱动返回负 errno；
- 故障只保留首次记录，失败前已完成的部分不回退，并在注释中写明；
- 设备读取共享内存中的多字节值时，先读到字节缓冲区，再用 `ldq_le_p`、`ldl_le_p`
  等小端辅助函数解码。

```c
/* 已有故障时保持首次记录，后续故障不覆盖。 */
static void ring_record_fault(GPGPUState *s, unsigned int id, uint64_t addr, uint32_t reason)
{
	if (id >= GPGPU_RING_COUNT || s->ring_fault[id].reason)
		return;
	s->ring_fault[id] = (GPGPURingFault) {
		.address = addr,
		.reason = reason,
	};
}

static uint32_t ring_read_rptr(GPGPUState *s, const GPGPURing *ring, uint64_t *rptr)
{
	uint8_t raw[8];

	if (gpgpu_phys_rw(s, ring->rptr_addr, raw, sizeof(raw), false) != MEMTX_OK)
		return GPGPU_RING_FAULT_STORAGE;
	*rptr = ldq_le_p(raw);
	return 0;
}
```

### 检查

用 `scripts/checkpatch.pl --branch HEAD^..HEAD` 检查最新提交。`hw/gpgpu/` 和
`software/` 使用 Linux 的 `checkpatch.pl` 和 `scripts/kernel-doc -none`
核对风格及结构体说明；QEMU 检查器的四空格规则不适用于这两个目录。
Rust 格式检查使用 `make -C build rustfmt`，需要 nightly rustfmt；
Clippy 的 Meson 开发环境配置见 `docs/devel/rust.rst`。
工具检查通过后，再按本节各小节逐项核对新增代码。

## 测试要求

使用 TCG 裸机测试、基于 GLib 的 QTest/QOS 测试和原生 Rust 单元测试。
指令测试命名为 `test-insn-*.c`，外设测试命名为 `test-*.c`，
Rust 测试函数命名为 `test_*`。新增测试登记到对应的 Meson 或 Makefile 列表。
只覆盖修改涉及的核心行为；实验未规定覆盖率百分比。
运行受影响的测试集，并核对通过数量和日志：评分脚本与 CI 可能在有失败项时仍返回成功。

## 提交与拉取请求要求

已有历史包含普通描述及 `ci:`、`tests/gevico:` 等前缀。
后续提交标题优先采用“子系统：具体修改”的中文形式，例如“测试：修复 SPI 片选”。
正文解释修改原因；向上游提交时添加 `Signed-off-by`。
拉取请求说明修改行为，关联问题，并列出测试命令和通过数量。
符合条件的 `main` 分支推送会触发实验 CI；请求审阅前先完成本地验证。

## 智能体交互要求

用连贯文字解释概念。避免用对比表，尤其不要以职责表或字段映射表代替概念说明。

解释 QEMU 概念或修改实验代码时，查阅父目录中的参考材料。
以下路径相对于仓库根目录：

- `../00_QEMU GPGPU 四阶段实验大纲.md`
- `../QEMU_2026_GPGPU_适配.html`
- `../QEMU_2026_实验.html`
- `../QEMU_2026_讲义.html`

若 `00_QEMU` 大纲不存在，使用 `../QEMU GPGPU 实验大纲.md`。

## 当前 GPGPU 实验

根据用户反馈，第一阶段已完成，并已搭配 A57、支持 SMMUv3 系统 IOMMU。
第二阶段的设备队列入口、RV64 CP 固件、Linux 驱动和 HSAKMT 风格接口库
已在固定提交 `6325a87e4c500f4930c4e0b502a1ec241f83bf26` 接入。
实验计划统一维护在[QEMU GPGPU 四阶段实验大纲](<../QEMU GPGPU 实验大纲.md>)；
已确认行为见[CP/MEC 第一版规格](<../CP_MEC 第一版规格.md>)，寄存器及协议查阅
见独立的 [GPGPU 硬件手册](<../QEMU_2026_GPGPU_硬件手册.html>)。

当前执行模型是 RV64 原生 CP 运行裸机固件、RV32 计算执行器运行应用 Kernel。
旧物理队列模式使用双进程设备、`IOMMU=none、VMID=0`；GPUVM 模式支持
`none/smmuv3`，用户 VMID 为 1、2，每 VM 一条 Queue，全设备两个用户队列名额。
CP Host 窗口支持对齐的 1/2/4/8 字节访问；计算线程通过
`0x9000_0000～0x9000_ffff` 的只读窗口读取 Host 参数，`a0` 仍为 32 位。
同步路径的参数继续位于 VRAM。末尾 4 MiB VRAM 固定供固件使用，
驱动用 `gpu_buddy` 管理前 60 MiB，并保留首个页。

HQD、8 字节 Doorbell、IH、分段执行器及状态确认均已接入。
成功路径依次发布 Signal、通知槽、IH 记录、`INVALID` Header 和消费进度；
默认等待先主动检查约 `200 μs`，再阻塞并复查 Signal，支持多个等待者。
错误保留首次记录，未知是否停止访问的对象继续保留；抢占、运行中
复位恢复、共享内存与组内屏障、完整 ROCr 运行时仍属后续范围。

2026-10-19 已提交的验收记录包含 CP QTest 16/16、Linux 队列 5/5、
设备卸载 1/1，以及单／双进程与 `none/smmuv3` 四种同步组合回归。
每种同步组合包含装卸载 5/5、三个示例的 17,953 个输出值、运行时检查
68/68 和卸载重载 2/2。日志位置与复现入口见
[软件栈说明](software/gpgpu/README.md#62-第二阶段验证状态历史)。

同日规格复核发现管理完成后的未知结果码未校验，创建失败分支可能提前释放
MQD 及相关引用。本轮已修复，并通过三个独立注入场景核对异常保留和设备停止后
的安全回收。修复前证据及当前验证入口统一见
[大纲复核记录](<../QEMU GPGPU 实验大纲.md#stage-two-spec-review>)。

`hxfei-git/gpu-study-qemu` 的开发统一使用 `main` 分支，
`hxfei-qemu` 保留早期开发历史。软件栈位于 `software/gpgpu/`；
修改接口前阅读其中的 `README.md` 和 `WORK_ITEMS.md`。
设备、固件、驱动与用户库共用的协议头集中在 `include/gpgpu/`：寄存器为
`gpgpu_regs.h`，内存对象为 `gpgpu_queue_abi.h`，DRM 接口为 `uapi/gpgpu_drm.h`；
协议当前为版本 3，与版本 2 的差异见 `software/gpgpu/PROTOCOL_V3.md`。
用户库位于 `lib/`：`lib/kmt/` 是 HSAKMT 风格薄层，`lib/rt/` 是装载 Kernel、
组包并等待的小运行时。驱动只提供 DRM render node，GPUVM 开关由设备属性
`-global gpgpu.gpuvm=on|off` 决定；旧的 `queue_mode` 参数与 `/dev/gpgpu0`
已删除。各层代码结构见软件栈 README 第 0 节。

第三阶段已实现 DRM/GEM 固定存储、32 位 GPUVA、三级 4 KiB 页表、两个用户
VMID、同步 TLB 失效及安全回收。保留 RV32 控制页
`0x8000_0000～0x8000_0fff`；先准备全部对象，再建 Queue，队列存活期间冻结映射。
不接入 TTM、迁移、驱逐、换页或运行中恢复。CPU 映射和 GEM 对象独立保护存储与
设备寿命；未知完成结果或失效未确认时保留资源。GPUVM 内核输出独立放在
`build/gpgpu-kernel-gpuvm/`，保留原环境。

第三阶段 CP 检查 18/18；GPUVM 在 `none/smmuv3` 下各 5/5、卸载各 1/1；
未知结果保留 3/3，内核无告警。旧队列和四种同步组合也已回归。
固定实现提交、复现命令和实际日志见[软件栈说明](software/gpgpu/README.md#stage-three-validation)。

运行 `make -f Makefile.gpgpu test-gpgpu-stack` 验证 ARM64 Linux 软件栈，
运行 `make -f Makefile.gpgpu test-gpgpu-qtest` 验证设备寄存器与 RV64 CP 队列，
运行 `make -C software/gpgpu check` 验证 RV32 汇编器；
Linux `6.18.57-0-gpgpu-virt` 重建命令见软件栈 README。
核对测试数量与日志，不能仅凭评分脚本退出状态判断全部通过。

2026-10-07 的 17,953 个示例输出值、68 项运行时检查及两种架构各 21 项
设备 QTest 属于第一阶段历史验收。解释器支持 BF16、FP8 和 FP4 转换，
尚未实现 FP16 转换；设备不支持迁移。
