# 仓库约束

## 项目结构与模块划分

QEMU Camp 2026 包含 RISC-V CPU、SoC、GPGPU 和 Rust 实验。
指令模拟位于 `target/riscv/`，G233 板级实现为 `hw/riscv/g233.c`，
设备模型位于 `hw/`，头文件位于 `include/`。
GPGPU 实现在 `hw/gpgpu/`，Rust 设备实现在 `rust/hw/`。

实验测试位于 `tests/gevico/tcg/` 和 `tests/gevico/qtest/`；
GPGPU 测试位于 `tests/qtest/gpgpu-test.c`。
其他测试位于 `tests/unit/` 和 `tests/functional/`。
构建与内部实现说明见 `README.md`、`README_zh.md` 和 `docs/devel/`。
固件位于 `pc-bios/`，构建产物统一放在 `build/`。

## 构建、测试与开发命令

在仓库根目录运行命令。按 `README.md` 安装 QEMU 构建依赖、
RISC-V 裸机编译器 `riscv64-unknown-elf-gcc`、Rust 和 `bindgen-cli`。

- `make -f Makefile.camp configure`：在 `build/` 配置 RISC-V 系统和用户态模拟及 Rust 支持。
- `make -f Makefile.camp build JOBS=4`：使用四个并行任务编译。
- `make -f Makefile.camp test-cpu`：通过 TCG 运行指令测试。
- `make -f Makefile.camp test-soc`：运行 G233 外设 QTest。
- `make -f Makefile.camp test-gpgpu`：运行 GPGPU QOS 子测试。
- `make -f Makefile.camp test-rust`：运行 Rust 单元和设备测试。
- `make -f Makefile.camp test`：运行全部实验测试。
- `build/qemu-system-riscv64 -machine help`：列出可用机器类型。

## 代码修改与语言约束

修改代码时，尽量缩小影响范围和改动量，沿用现有架构、模块职责、接口和执行模型。
只修改完成当前任务必需的内容，不顺手重构、移动代码或改名；
必须调整架构或接口时，先说明原因和必要范围。

与用户的交互、代码注释、文档说明以及提交标题和正文均使用中文。
技术术语可保留通用英文写法；标识符、路径、命令、协议字段、汇编助记符、
机器读取的标记和许可证声明保留原文，不因中文化改变其含义或行为。

## 代码风格与命名

遵循 `.editorconfig` 和 `docs/devel/style.rst`：C 使用四个空格缩进，
Makefile 配方使用制表符，文件使用 LF 换行，每行尽量不超过 80 列。
函数和变量使用 `snake_case`，类型使用 `CamelCase`，宏使用大写。
C 源文件首先包含 `qemu/osdep.h`。
用 `scripts/checkpatch.pl --branch HEAD^..HEAD` 检查最新提交。
Rust 格式检查使用 `make -C build rustfmt`，需要 nightly rustfmt；
Clippy 的 Meson 开发环境配置见 `docs/devel/rust.rst`。

## 测试要求

使用 TCG 裸机测试、基于 GLib 的 QTest/QOS 测试和原生 Rust 单元测试。
指令测试命名为 `test-insn-*.c`，外设测试命名为 `test-*.c`，
Rust 测试函数命名为 `test_*`。新增测试登记到对应的 Meson 或 Makefile 列表。
覆盖修改的行为和回归场景；实验未规定覆盖率百分比。
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
