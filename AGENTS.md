# 仓库约束

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| CPU | Central Processing Unit | 中央处理器 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| RV32 / RV64 | 32 / 64-bit RISC-V | 计算执行器／原生 CP 的 RISC-V 位宽 |
| DRM / GEM | Direct Rendering Manager / Graphics Execution Manager | Linux 图形设备框架／内存对象管理 |
| GPUVM / GPUVA | GPU Virtual Memory / GPU Virtual Address | GPU 虚拟内存／GPU 虚拟地址 |
| HSA / HSAKMT | Heterogeneous System Architecture / HSA Kernel Mode Thunk | 异构系统架构／HSA 内核接口封装层 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| IH | Interrupt Handler | 本实验的完成与错误通知模块 |
| API / UAPI | Application Programming Interface / User-space API | 应用编程接口／内核向用户态提供的接口 |
| ABI | Application Binary Interface | 应用二进制接口 |
| MMIO | Memory-Mapped I/O | 内存映射输入输出 |
| PCI / PCIe | Peripheral Component Interconnect / PCI Express | 外设互连／高速外设互连总线 |
| DMA | Direct Memory Access | 直接内存访问 |
| VRAM | Video RAM | 设备显存 |
| ARM64 | Arm 64-bit Architecture | Host 客体使用的 64 位 Arm 架构 |
| TAP | Test Anything Protocol | 测试结果输出协议 |
| SPDX | Software Package Data Exchange | 软件许可证标识规范 |
| LF | Line Feed | 换行符 |
| VS Code | Visual Studio Code | 本项目使用的编辑器 |
| WSL | Windows Subsystem for Linux | Windows 的 Linux 子系统 |
| URI | Uniform Resource Identifier | 统一资源标识符 |

## 项目结构与模块划分

本仓库维护 QEMU GPGPU 教学设备、配套软件和实验文档。主要修改范围为
`hw/gpgpu/`、`hw/riscv/gpgpu-cp.c`、`software/gpgpu/`、`tests/qtest/gpgpu-*.c`
和 `docs/`；其余目录保留 QEMU 的基础功能，不再承担训练营的其他实验。

设备模型按 `cp/`、`compute/`、`mem/` 和 `irq/` 分模块，软件栈沿
应用 → 运行时 → HSAKMT 风格薄层 → DRM 驱动 → 设备与固件调用。
共享协议头位于 `software/gpgpu/include/gpgpu/`，不是顶层 `include/`。
各层入口见[软件栈第 0 节](<./software/gpgpu/README.md#0-代码结构与分层>)。
构建与运行产物统一放在 `build/`，不纳入 Git。

## 构建、测试与开发命令

命令在仓库根目录运行。依赖和快速开始见 [README](./README.md)，
单个 QTest 的选择方法与双进程运行方式见 [CLAUDE.md](./CLAUDE.md)。

- `make -f Makefile.gpgpu configure`：独立配置两个 QEMU，不需要客体内核。
- `make -f Makefile.gpgpu build JOBS=4`：构建 QEMU、QTest 和 RV64 固件；首次自动配置。
- `make -f Makefile.gpgpu test-gpgpu-qtest`：运行寄存器与真实 CP 固件回归，核对 TAP。
- `make -f Makefile.gpgpu test-gpgpu-stack`：在开发客体运行 GPUVM 开、关的计算示例。
- `make -C software/gpgpu all check`：构建用户库与示例，运行教学汇编器回归。
- `make -C software/gpgpu check-bootstrap`：验证基础环境缓存与已有磁盘保护。

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
作为父仓库子模块时，先在本仓库提交代码与实验文档，再在父仓库提交子模块指针。
推送与远端设置修改需要单独授权。

## 文档与交互

用连贯文字解释概念，不用职责表代替调用关系。实验文档位于本仓库 `docs/`，
修改前先阅读相关规格与软件栈说明。原课程资料保存在 `docs/课程资料/`：
`QEMU_2026_讲义.html` 和 `QEMU_2026_实验.html`。保留课程原文与许可，
按当时版本阅读；当前实现仍以规格和软件栈说明为准。

作为 `gpu_study` 子模块时，中文文档遵循父仓库 `AGENTS.md` 与《文档的常见问题》
中的写作、证据和自然化要求。文档标题后紧接缩写表，先讲任务和关系再给源码，
保留 `[SOURCE]`、`[SPEC]`、`[INFERENCE]`、`[BOUNDARY]`、`[DESIGN]` 的区别。
历史分析绑定固定提交，不把旧文件不存在当成需要恢复兼容接口。

预览环境为 Windows VS Code 连接 WSL。当前本地源码定位使用
`vscode://vscode-remote/wsl+Ubuntu/绝对路径:行:列`，中文和空格做 URL 编码；
不得追加本地 `#L…`。历史源码用固定提交的 GitHub 链接和真实行号。
普通文档使用相对链接，含空格或中文时沿用 `[文字](<./路径>)` 写法。
交付时区分路径、行号检查与实际预览点击，不以解析通过代替跳转验证。

## 信息归属

- 当前阶段、任务、下一步与阻塞项只写在 [docs/实验进度.md](<./docs/实验进度.md>)，
  使用定性状态，不复制测试数字。
- 当前代码版本的构建、验证结果和日志只写在
  [software/gpgpu/README.md 第 5 节](<./software/gpgpu/README.md#5-构建运行与本次结果>)；
  历史工作与验收放第 6 节。
- 阶段依赖、范围和完成标准在[实验大纲](<./docs/QEMU GPGPU 实验大纲.md>)；
  已确认设计在 CP/MEC 与 GPUVM 规格，确认过程只保留 CP/MEC 规格第 9 节。
- 寄存器、位域、访问权限和复位值在[硬件手册](<./docs/GPGPU_硬件手册.html>)；
  组件升级差异在 [PROTOCOL_V3.md](<./software/gpgpu/PROTOCOL_V3.md>)。
- 学习进度与个人安排由父仓库《学习与实验进度》维护。
  其他位置只放链接，不再保存第二份进度或确认摘要。
