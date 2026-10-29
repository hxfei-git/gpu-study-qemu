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

在仓库根目录运行命令。按 `README.md` 安装 QEMU 构建依赖、
RISC-V 裸机编译器 `riscv64-unknown-elf-gcc`、Rust 和 `bindgen-cli`。

- `make -f Makefile.camp configure`：在 `build/` 配置 RISC-V 系统和用户态模拟及 Rust 支持。
- `make -f Makefile.camp build JOBS=4`：使用四个并行任务编译。
- `make -f Makefile.camp test-cpu`：通过 TCG 运行指令测试。
- `make -f Makefile.camp test-soc`：运行 G233 外设 QTest。
- `make -f Makefile.camp test-gpgpu-qtest`：运行 GPGPU 寄存器与 CP 队列 QTest。
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

运行 `make -f Makefile.camp test-gpgpu-stack` 验证 ARM64 Linux 软件栈，
运行 `make -f Makefile.camp test-gpgpu-qtest` 验证设备寄存器与 RV64 CP 队列
（11 项），运行 `make -C software/gpgpu check` 验证 RV32 汇编器；
Linux `6.18.57-0-gpgpu-virt` 重建命令见软件栈 README。
核对测试数量与日志，不能仅凭评分脚本退出状态判断全部通过。

2026-10-07 的 17,953 个示例输出值、68 项运行时检查及两种架构各 21 项
设备 QTest 属于第一阶段历史验收。解释器支持 BF16、FP8 和 FP4 转换，
尚未实现 FP16 转换；设备不支持迁移。
