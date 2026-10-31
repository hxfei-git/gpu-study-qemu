# QEMU GPGPU 教学设备与软件栈

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| HTML | HyperText Markup Language | 超文本标记语言 |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| CPU | Central Processing Unit | 中央处理器 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| ARM64 | Arm 64-bit Architecture | Host 客体使用的 64 位 Arm 架构 |
| RV32 / RV64 | 32 / 64-bit RISC-V | 计算执行器／原生 CP 使用的 RISC-V 位宽 |
| GPUVM | GPU Virtual Memory | GPU 虚拟内存 |
| GPL | GNU General Public License | GNU 通用公共许可证 |
| CC BY-SA | Creative Commons Attribution-ShareAlike | 知识共享署名—相同方式共享许可 |
| AMDGPU | AMD GPU Linux Kernel Driver | AMD GPU Linux 内核驱动 |

## 1. 项目定位与入口

本项目在 QEMU 中实现一个可观察、可修改的 GPGPU 教学设备，并配套 Linux 驱动、用户库、CP 裸机固件和计算示例。重点是沿一次任务看清对象分配、队列提交、地址翻译、完成通知与资源回收，不追求兼容 AMD 的完整硬件协议或现成运行时。

当前阶段见[实验进度](<./docs/实验进度.md>)；代码结构、构建命令和运行结果见[软件栈说明](<./software/gpgpu/README.md>)。各文档负责维护哪些信息，见 [AGENTS.md](./AGENTS.md) 的“信息归属”。

**[BOUNDARY]** 这是自定义 QEMU 教学设备。父仓库中以外部 Host CPU + MI300X 独立 GPU 为基线的 AMDGPU 学习笔记用于理解真实软件栈；本设备的寄存器、地址布局和执行器约束不能作为 MI300X 的硬件结论。

## 2. 一次计算经过的代码

实验中的 Host 是 ARM64/A57 Linux 客体，应用和驱动运行在其中。设备在另一个 `qemu-system-riscv64 -machine gpgpu-cp` 进程中，由 RV64 CP 执行裸机固件，再把应用 Kernel 交给 RV32 解释器。从应用到设备的五层调用、各层入口和共享协议头见[软件栈第 0 节](<./software/gpgpu/README.md#0-代码结构与分层>)。

## 3. 配置、构建与运行

依赖（含首次配置需要联网下载的子项目）、构建与测试命令、Linux 全栈所需的开发环境和当前验证结果，都在[软件栈第 5 节](<./software/gpgpu/README.md#build-and-validation>)。

## 4. 文档导航

- [实验进度](<./docs/实验进度.md>)、[四阶段实验大纲](<./docs/QEMU GPGPU 实验大纲.md>)
- [CP/MEC 第一版规格](<./docs/CP_MEC 第一版规格.md>)、[GPUVM 第一版规格](<./docs/GPUVM 第一版规格.md>)
- [GPGPU 硬件手册](<./docs/GPGPU_硬件手册.html>)、[协议版本差异](<./software/gpgpu/PROTOCOL_V4.md>)
- [软件栈说明](<./software/gpgpu/README.md>)
- 历史参考，按各自注明的版本阅读：[GPGPU 适配笔记](<./docs/GPGPU_适配.html>)、[CP/MEC 软件实现、参考与扩展](<./docs/历史/CP_MEC 软件实现、参考与扩展.md>)、[旧设备说明](<./docs/历史/gpgpu.rst>)

## 5. 来源、许可与维护

项目从 QEMU Camp 2026 的 GPGPU 教学设备继续开发。原始课程入口为 <https://qemu.gevico.online/exercise/2026/>，离线副本保存为[课程讲义](<./docs/课程资料/QEMU_2026_讲义.html>)与[课程实验](<./docs/课程资料/QEMU_2026_实验.html>)，保留原文与许可。

**[BOUNDARY]** 课程资料按当时版本阅读，当前接口以本项目规格和软件栈说明为准。构建与运行不依赖这两份 HTML。

QEMU 本体遵循 [COPYING](./COPYING) 中的 GPL 许可说明，具体文件继续保留各自的版权与许可证声明。[硬件手册的来源与许可](<./docs/GPGPU_硬件手册.html#source-and-license>)保留原作者、贡献者和插图来源，沿用 CC BY-SA 4.0。不要把代码与文档概括为同一种许可。

开发规则见 [AGENTS.md](./AGENTS.md)，[CLAUDE.md](./CLAUDE.md) 只补充 Claude Code 的使用说明。`README.rst` 和 `MAINTAINERS` 是 QEMU 工具保留的上游文件，不作为本项目的阅读或任务入口。
