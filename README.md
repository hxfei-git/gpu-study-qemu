# QEMU GPGPU 教学设备与软件栈

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| CPU | Central Processing Unit | 中央处理器 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| ARM64 | Arm 64-bit Architecture | Host 客体使用的 64 位 Arm 架构 |
| RV32 / RV64 | 32 / 64-bit RISC-V | 计算执行器／原生 CP 使用的 RISC-V 位宽 |
| DRM / GEM | Direct Rendering Manager / Graphics Execution Manager | Linux 图形设备框架／内存对象管理 |
| GPUVM | GPU Virtual Memory | GPU 虚拟内存 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| HSAKMT | HSA Kernel Mode Thunk | HSA 内核接口封装层；本项目借鉴其接口风格 |
| IH | Interrupt Handler | 本实验的完成与错误通知模块 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| BAR | Base Address Register | PCI 基址寄存器 |
| PCI | Peripheral Component Interconnect | 外设互连总线 |
| VRAM | Video RAM | 设备显存 |
| DMA | Direct Memory Access | 直接内存访问 |
| IOMMU | Input/Output Memory Management Unit | 输入输出内存管理单元 |
| GPL | GNU General Public License | GNU 通用公共许可证 |
| GNU | GNU's Not Unix | GNU 自由软件项目 |
| CC BY-SA | Creative Commons Attribution-ShareAlike | 知识共享署名—相同方式共享许可 |

## 1. 项目定位与入口

本项目在 QEMU 中实现一个可观察、可修改的 GPGPU 教学设备，并配套 Linux 驱动、用户库、CP 裸机固件和计算示例。重点是沿一次任务看清对象分配、队列提交、地址翻译、完成通知与资源回收，不追求兼容 AMD 的完整硬件协议或现成运行时。

当前任务与阶段状态只看[实验进度](<./docs/实验进度.md>)。第一次阅读可沿本页的调用图进入[软件栈说明](<./software/gpgpu/README.md>)；运行结果也在该说明中记录，本页不复制通过数。

**[BOUNDARY]** 这是自定义 QEMU 教学设备。父仓库中以外部 Host CPU + MI300X 独立 GPU 为基线的 AMDGPU 学习笔记用于理解真实软件栈；本设备的寄存器、地址布局和执行器约束不能作为 MI300X 的硬件结论。

## 2. 一次计算经过的代码

实验中的 Host 是 ARM64/A57 Linux 客体，应用和驱动运行在其中。设备在另一个 `qemu-system-riscv64 -machine gpgpu-cp` 进程中，由 RV64 CP 执行裸机固件，再把应用 Kernel 交给 RV32 解释器。

```text
demos/gpgpu-demo.c        准备数组、调用运行时、逐元素比较结果
  │ gpgpu_rt_*()
lib/rt/                   装载 Kernel、组 AQL 包、准备并等待 Signal
  │ gpgpuKmt*()
lib/kmt/                  HSAKMT 风格薄层：封装设备、内存、队列与事件 ioctl
  │ ioctl
linux/                    DRM 驱动：GEM、GPUVM、管理队列、IH 与事件
  │ BAR0，以及内存中的 MQD、Ring、Signal
hw/gpgpu/ + firmware/     QEMU 设备模型与 RV64 CP 固件；RV32 执行器运行 Kernel
```

除 `hw/gpgpu/` 外，图中目录均相对于 `software/gpgpu/`。各层共用 `software/gpgpu/include/gpgpu/` 的协议头，不各自保存常量副本。设备、固件、驱动和用户库需要一起构建更新。

主要修改入口是 `hw/gpgpu/`、`hw/riscv/gpgpu-cp.c`、`software/gpgpu/` 和 `tests/qtest/gpgpu-{regs,cp}-test.c`。其余目录主要是保留的 QEMU 本体；它们为机器、CPU、PCI、内存和设备运行提供基础，不是待完成的课程目录。

## 3. 配置、构建与运行

宿主机需要 QEMU 的 C/Python 构建依赖、Ninja、`pkg-config`、GLib 和 pixman 开发包，另需 `libjson-c-dev`、`libcmocka-dev`，以及 CP 固件使用的 `riscv64-unknown-elf-gcc`。GPGPU 构建不需要 Rust。Ubuntu 24.04 可通过软件包管理器安装后面三个依赖：

```sh
sudo apt install libjson-c-dev libcmocka-dev gcc-riscv64-unknown-elf
```

在仓库根目录运行：

```sh
# 配置与构建不需要 Linux 客体内核
make -f Makefile.gpgpu configure
make -f Makefile.gpgpu build JOBS=4
make -f Makefile.gpgpu test-gpgpu-qtest

# 用户库、示例与 RV32 教学汇编器
make -C software/gpgpu all check
```

QEMU 默认构建目录为 `build/arm64-qemu`。Linux 全栈还需要指定的开发内核与客体镜像，准备方法见[软件栈第 5.1 节](<./software/gpgpu/README.md#51-从零准备基础环境与离线材料>)，不要把设备 QTest 通过当作客体环境已经就绪。

```sh
# 使用开发客体运行 GPUVM 开、关两种模式下的计算示例
make -f Makefile.gpgpu test-gpgpu-stack
```

全部命令、产物位置、环境限制和实际结果见[软件栈第 5 节](<./software/gpgpu/README.md#5-构建运行与本次结果>)，构建目标可用 `make -f Makefile.gpgpu help` 查看。

## 4. 文档导航

- [实验进度](<./docs/实验进度.md>)记录当前阶段、任务、下一步和阻塞项；[四阶段实验大纲](<./docs/QEMU GPGPU 实验大纲.md>)维护依赖、范围与完成标准。
- [CP/MEC 第一版规格](<./docs/CP_MEC 第一版规格.md>)说明队列、派发、完成和错误处理；[GPUVM 第一版规格](<./docs/GPUVM 第一版规格.md>)说明地址空间、绑定和资源回收。
- [GPGPU 硬件手册](<./docs/GPGPU_硬件手册.html>)按硬件模块查阅寄存器、位域、权限和复位值；[协议版本差异](<./software/gpgpu/PROTOCOL_V3.md>)用于核对组件变更。
- [软件栈说明](<./software/gpgpu/README.md>)保存当前代码入口、构建与验收，第 6 节归档历史工作和验证记录。
- [GPGPU 适配笔记](<./docs/GPGPU_适配.html>)保存早期注册、初始化与执行器分析；[CP/MEC 软件实现、参考与扩展](<./docs/历史/CP_MEC 软件实现、参考与扩展.md>)和[旧设备说明](<./docs/历史/gpgpu.rst>)均为历史参考，按各自注明的版本阅读。

## 5. 来源、许可与维护

项目从 QEMU Camp 2026 的 GPGPU 教学设备继续开发。原始课程入口为 <https://qemu.gevico.online/exercise/2026/>；父仓库保留的讲义与实验 HTML 不属于独立克隆本仓库所必需的运行材料。

QEMU 本体遵循 [COPYING](./COPYING) 中的 GPL 许可说明，具体文件继续保留各自的版权与许可证声明。[硬件手册的来源与许可](<./docs/GPGPU_硬件手册.html#source-and-license>)保留原作者、贡献者和插图来源，沿用 CC BY-SA 4.0。不要把代码与文档概括为同一种许可。

开发规则见 [AGENTS.md](./AGENTS.md)，常用调用关系与命令补充见 [CLAUDE.md](./CLAUDE.md)。`README.rst` 和 `MAINTAINERS` 为 QEMU 工具保留的上游文件，不作为本项目的阅读或任务入口。构建、运行产物放在 `build/`，不提交到 Git。
