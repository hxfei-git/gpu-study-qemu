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
`hw/gpgpu/` 和 `software/` 下的自有 C 代码按用户要求采用 Linux 风格，
以目录内的 `.editorconfig` 和 `.clang-format` 为准：使用 8 列制表符，
函数左花括号另起一行，结构体每行只声明一个成员，每行尽量不超过 100 列。
结构体、成员、枚举值和宏说明用途及单位；接口实现前说明作用、执行思路，
涉及地址空间、锁、引用或发布顺序时一并写明。Python 和汇编入口也应有用途说明。
固定版本的外部内核补丁、配置和打包文件保持原貌，不参与批量格式化。
函数和变量使用 `snake_case`，类型使用 `CamelCase`，宏使用大写。
QEMU 设备的 C 源文件首先包含 `qemu/osdep.h`。
用 `scripts/checkpatch.pl --branch HEAD^..HEAD` 检查最新提交。
上述两个目录使用 Linux 的 `checkpatch.pl` 和 `scripts/kernel-doc -none`
核对风格及结构体说明；QEMU 检查器的四空格规则不适用于这两个目录。
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

若 `00_QEMU` 大纲不存在，使用 `../QEMU GPGPU 实验大纲.md`。

## 当前 GPGPU 实验

根据用户反馈，进阶实验一已完成，并已搭配 A57、支持 SMMUv3 系统 IOMMU。
当前进入第二阶段：用户 AQL Queue、Doorbell、CP 与 Signal。实现前先阅读
[QEMU GPGPU 四阶段实验大纲](<../QEMU GPGPU 实验大纲.md>) 和
[CP/MEC 第一版规格](<../CP_MEC 第一版规格.md>)，实验计划继续统一在大纲维护。
2026-10-11 已完成远端设备与原生 CP 基础接入，2026-10-14 完成
CP Host 访存窗口及回归。2026-10-17 原生 CP 迁移至 RV64，窗口增加
对齐的 8 字节读写并通过原有 8 项 CP 回归；裸机物理访存、地址编码和
RV32 计算执行器保持原约定。当前状态见
[大纲进度说明](<../QEMU GPGPU 实验大纲.md#current-progress>)。第二阶段
队列功能尚未记录实现完成或整体运行验收通过。

2026-10-18 按固定提交 `57543dd54b6b046b888277272fa84af81746b1ac` 核对：
RV64 原生 CP 执行裸机固件，RV32 计算执行器运行应用 Kernel，两者分别保存
寄存器和取指位置。计算线程的 `a0` 仍为 32 位，当前参数仍从 VRAM 读取。
CP Host 窗口已实现；[规格 9.5.2](<../CP_MEC 第一版规格.md#spec-kernel-executor-interface>)
已确认 16 字节描述对象和计算执行器的只读参数窗口。执行器保存完整的 64 位
Host 参数基址和长度，各计算线程以 `a0 = 0x9000_0000` 访问
`0x9000_0000～0x9000_ffff`，每次按实际长度检查 4 字节读取。
该路径尚未实现，CP 分发寄存器和执行结果接口已按
[规格 9.5.3](<../CP_MEC 第一版规格.md#spec-cp-dispatch-interface>) 确定。
CP 解析描述对象，经 `0xfffd_0000 + BAR0 字节偏移` 访问原有分发配置，
新增代码和参数长度寄存器用于范围检查。配置只在空闲时填写，最后写
`DISPATCH=1`；设备统一保存配置，通过本次状态和错误寄存器返回结果，
CP 读取并确认后才可接受下一任务。执行器分段推进并保留线程状态和累计预算，
队列仍逐个完整执行 Dispatch；正常路径由 CP 独占提交和确认，旧 Host 直接
提交路径单独用于回归。映射、五个新增寄存器和分段执行均尚待实现和验证，
完整布局见[规格 6.3 节](<../CP_MEC 第一版规格.md#spec-executor-dispatch>)。
源码依据见[CP 与计算执行器的位宽](<../CP_MEC 软件实现、参考与扩展.md#cp-executor-widths>)。

[规格 9.5.4](<../CP_MEC 第一版规格.md#spec-signal-completion-protocol>) 已确认
24 字节 Signal 布局和通知、等待协议，属于待实现的第二阶段接口。
每个使用中的 Signal 独占事件和通知槽；事件编号在进程内非零且不与存活事件重复。
通知槽初值为 0，CP 完成时写入零扩展到 64 位的事件编号；驱动先保存“已通知”
状态，再清零通知槽并唤醒等待者。登记后复查条件，等待返回不清除事件状态，
同一任务可唤醒多个等待者；旧通知及相关处理结束后才可重新初始化。
完成值仍为 `1 → 0`，默认等待仍先主动检查约 `200 μs` 再阻塞。

[规格 9.5.5](<../CP_MEC 第一版规格.md#spec-task-visibility>) 已确认任务可见性
规则，尚待实现：A57 运行时在 Header 发布前、Doorbell 写入前及读到 Signal 完成后
执行带编译器内存约束的 `dmb sy`；CP 在取包、提交配置和完成通知之间使用
同样带编译器内存约束的 `fence iorw, iorw`。执行器后端结束相关访问后才发布终态，
成功还须完成结果写回；跨线程交接用锁或 release/acquire 同步。CP 保存结果并清理、确认后，
依次发布 Signal、通知槽和 IH 通知，每次访问成功后才继续。屏障不能代替 DMA
完成等待或后端同步，完整流程与验证要求见[规格 6.4 节](<../CP_MEC 第一版规格.md#spec-dispatch-visibility>)。

`hxfei-git/gpu-study-qemu` 的开发统一使用 `main` 分支；
`hxfei-qemu` 保留早期开发历史。第一阶段软件栈位于 `software/gpgpu/`，
包含 Linux PCI 驱动、libgpgpu、RV32 汇编 kernel 和汇编前端、三个 FP32 示例及客体集成测试。
修改接口前阅读 `software/gpgpu/README.md` 和 `WORK_ITEMS.md`。
当前 ABI 位于 `software/gpgpu/include/gpgpu_uapi.h`。

运行 `make -f Makefile.camp test-gpgpu-stack` 验证 ARM64 Linux 驱动与运行时。
首次运行会初始化 Alpine，后续构建复用 `build/gpgpu-linux-module/` 中保存编译环境的磁盘。
运行 `make -C software/gpgpu check-assembler` 验证汇编前端编码。

2026-10-07 的第一阶段验证通过了三个示例（17,953 个输出值）、
68 项驱动与运行时检查，以及 RISC-V 和 ARM64 virt 上各 21 项 GPGPU QTest。
这 21 项包括 17 项评分测试和 4 项回归测试。核对测试数量与日志，
不能仅凭评分脚本的退出状态判断全部通过。

现有代码仍使用第一阶段的独占会话和同步调用。设备指针为 VRAM 偏移；
mmap 映射 64 KiB 一致性 DMA 暂存缓冲区。
第二阶段将按大纲增加用户 AQL Queue、Doorbell、CP 与 Signal，
并保留现有同步路径的回归验证。共享内存与 block 屏障、异步流和 GPU 地址隔离
留待后续实现。阶段规格中的新增寄存器、共享区和接口不能作为已有设备能力。
解释器支持 BF16、FP8 和 FP4 转换，尚未实现 FP16 转换。设备已明确禁止迁移。
