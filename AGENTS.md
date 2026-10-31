# 仓库约束

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| GPUVM / GPUVA | GPU Virtual Memory / GPU Virtual Address | GPU 虚拟内存／GPU 虚拟地址 |
| IH | Interrupt Handler | 本实验的完成与错误通知模块 |
| AMDGPU | AMD GPU Linux Kernel Driver | AMD GPU Linux 内核驱动 |
| ROCr | ROCm Runtime | ROCm 的异构计算运行时 |
| TAP | Test Anything Protocol | 测试结果输出协议 |
| SPDX | Software Package Data Exchange | 软件许可证标识规范 |
| LF | Line Feed | 换行符 |
| VS Code | Visual Studio Code | 本项目使用的编辑器 |
| WSL | Windows Subsystem for Linux | Windows 的 Linux 子系统 |
| URI / URL | Uniform Resource Identifier / Uniform Resource Locator | 统一资源标识符／统一资源定位符 |
| CI | Continuous Integration | 持续集成 |
| ASCII | American Standard Code for Information Interchange | 美国信息交换标准代码 |

## 项目结构与模块划分

本仓库维护 QEMU GPGPU 教学设备、配套软件和实验文档。主要修改范围为
`hw/gpgpu/`、`hw/riscv/gpgpu-cp.c`、`software/gpgpu/`、`tests/qtest/gpgpu-*.c`
和 `docs/`；其余目录保留 QEMU 的基础功能，不再承担训练营的其他实验。

代码分层、模块划分和共享协议头位置见[软件栈第 0 节](<./software/gpgpu/README.md#0-代码结构与分层>)。
构建与运行产物统一放在 `build/`，不纳入 Git。

## 构建、测试与开发命令

依赖、构建与测试命令、单个 QTest 用例的运行方式和当前验证结果，只写在
[软件栈说明第 5 节](<./software/gpgpu/README.md#build-and-validation>)。

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
其他目录遵循 `.editorconfig` 和 [QEMU 上游风格说明](https://www.qemu.org/docs/master/devel/style.html)：C 使用四个空格缩进，
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
工具检查通过后，再按本节各小节逐项核对新增代码。

## 测试要求

优先运行受影响的现有入口，不因整理文档增加测试。QTest 同时核对退出状态、
TAP 计划与结果，不固定通过数；Linux 全栈核对实际比较结果和内核日志。
已运行的命令、结果及边界只记录在软件栈说明相应节，不能把未运行写成通过。

## 提交与拉取请求要求

提交标题采用“子系统：具体修改”的中文形式，例如“设备：修复 Doorbell 访问宽度”。
正文解释原因和影响；向上游提交时添加 `Signed-off-by`。
拉取请求说明行为变化、关联问题与实际验证，不再引用训练营评分或自动 CI。

本仓库以 `main` 为开发基线，`hxfei-qemu` 仅保留早期历史。整理任务可在临时分支
执行，完成后在无分叉且工作区干净时快进 `main`；不得覆盖用户未提交修改或改写历史。
本仓库独立提交和推送，不需要同步修改父仓库；父仓库是否更新子模块指针，由父仓库
按需决定。推送与远端设置修改需要单独授权。

## 文档与交互

本仓库的中文文档按本节维护，不依赖其他仓库的写作规则。作为 `gpu_study` 子模块
检出时，父仓库规则只约束学习笔记；本仓库内的代码和文档以本文件为准。

实验文档位于 `docs/`，修改前先阅读相关规格与软件栈说明。原课程资料保存在
`docs/课程资料/`：`QEMU_2026_讲义.html` 和 `QEMU_2026_实验.html`。保留课程原文与
许可，按当时版本阅读；当前实现仍以规格和软件栈说明为准。

### 适用范围

规格、设计说明、协议、手册、大纲、项目说明和软件栈说明等长期维护、反复查阅的文档
属于正式文档，要求缩写表和自然化检查。临时草稿、会话计划、一次性的交接与执行记录、
临时排查结果默认不要求这两项，整理为正式文档时再补齐；用户明确要求时按要求执行。
文档性质按实际用途判断，不看目录、文件后缀或是否已提交。

### 现状写法与单一来源

- 文档写现状、约定和结论，不写“某天做了什么”的过程叙述。日期只在本身就是事实时
  保留，例如固定版本、规范封面日期，或结果文件里的完成时间；变更经过和原因写在提交说明里。
- 每件事只在一个文档中维护，归属见下文“信息归属”。其他文档只放链接，不复述内容、
  数量或结论，修改时只改一处。被多处引用的章节使用固定的 ASCII 锚点，改标题时链接不受影响。

### 结构与讲解

- 标题后紧接缩写表，三列为“缩写、英文全称、中文含义”，只收本文用到的技术缩写，
  随正文更新；没有缩写时不建空表。全文一个 `#` 标题，`##` 为编号章节，`###` 为短小节。
- 先交代当前任务：谁在做什么、已有什么、要得到什么；再给完整的关系图或执行路径，
  沿图解释字段、数值和分支。概览不依赖后文才讲的取值和配置细节。
- 用连贯文字和同一个具体例子讲清对象如何配合，不用职责表或字段对照表代替调用关系；
  参数、缩写等需要集中查阅的信息可以用表。贯穿主线的结构体，先给只含核心成员的简化定义。
- 追问暴露的缺失前提补到第一次需要它的位置，不堆在当前段落末尾；跨节移动内容后，
  同步更新大纲、锚点与引用。
- 图示优先用 `text` 或 Mermaid，图前写例子条件，图后解释关键关系；除非用户要求，
  不生成新图片。代码块标注语言。
- 标题直接写出对象、动作或结果，问句标题只用于易混淆点。改标题时保留编号和层级，
  同步更新锚点和链接。
- 超过 4 位的十六进制数在正文、示例和自绘图中从右向左每 4 位加下划线，
  如 `0x8000_1234`；源码、命令、日志和规范原文保持原样。

### 证据与源码

- 用 `[SOURCE]`、`[SPEC]`、`[INFERENCE]`、`[BOUNDARY]`、`[DESIGN]` 区分事实、推断、
  边界与设计。`[SOURCE]`、`[SPEC]` 的引导文字放在 `>` 引用块中，写明文件路径、固定版本
  和真实行号，或规范名称、版本、章节与页码；原始源码仍用普通代码块。
- 正文先讲清概念和过程，源码只用来验证。摘录保留读懂结论所需的函数名、入参、外围分支
  和调用关系；不连续的片段分块，并说明先后关系；英文注释和错误信息后附中文翻译。
  不把教学伪代码写成源码。
- 教学设备是自定义实现。引用 AMDGPU、ROCr 或 MI300 资料作对照时，注明固定版本和
  适用范围，不把本设备的简化写成 MI300X 的硬件结论。
- 历史分析绑定固定提交，不把旧文件不存在当成需要恢复兼容接口。历史提交号依赖远端
  分支 `archive/main-before-contributors-cleanup-20261031`，不要删除该分支，原因见
  软件栈说明第 6 节开头。

### 链接

预览环境为 Windows VS Code 连接 WSL。本地源码定位使用
`vscode://vscode-remote/wsl+Ubuntu/绝对路径:行:列`，中文和空格做 URL 编码，不追加
`#L…`；文件名和每段行号都做成链接，范围链接定位到起始行。指向父仓库 `2.源码/` 的
本地链接只在作为子模块检出时可用。历史源码用固定提交的 GitHub 链接和真实行号。
普通文档使用相对链接，含空格或中文时写成 `[文字](<./路径>)`。交付时区分路径、
行号检查与实际预览点击，不以解析通过代替跳转验证。

### 自然化检查

新建、补充、改写或审阅正式中文文档，包括只改一个段落，都要用 humanizer-zh
（上游 op7418/Humanizer-zh 提交 `f4518a8`）做最终检查：先完成事实、结构、证据和格式，
再检查本次改动的自然语言；保留技术事实、数字、术语、代码、链接和标题层级，删除空泛
开场、重复总结和套话。技能缺失时告知用户，并按同样原则人工检查。

## 信息归属

每类信息只在下列一处维护，其他文档只链接，不复制内容。

- 项目定位、文档入口与许可在 [README](./README.md)。
- 代码分层、模块与协议头位置在
  [软件栈说明第 0 节](<./software/gpgpu/README.md#0-代码结构与分层>)；依赖、构建与测试命令、
  当前代码版本的验证结果、日志位置和本地材料状态在
  [第 5 节](<./software/gpgpu/README.md#build-and-validation>)；历史工作与验收在第 6 节。
- 当前阶段、近期任务、下一步与阻塞项在 [docs/实验进度.md](<./docs/实验进度.md>)，
  使用定性状态，不复制测试数字和材料清单。
- 阶段依赖、范围和完成标准在[实验大纲](<./docs/QEMU GPGPU 实验大纲.md>)，进入当前阶段
  后再细化近期任务，不预先固定全部目标；已确认设计在 CP/MEC 与 GPUVM 规格，
  确认过程只保留 CP/MEC 规格第 9 节。
- 寄存器、位域、访问权限和复位值在[硬件手册](<./docs/GPGPU_硬件手册.html>)；
  组件升级差异在 [PROTOCOL_V3.md](<./software/gpgpu/PROTOCOL_V3.md>)，协议常量以
  `software/gpgpu/include/gpgpu/` 为准。
- 学习进度与个人安排不在本仓库记录。
