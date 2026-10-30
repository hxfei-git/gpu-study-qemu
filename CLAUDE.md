# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

## 1. 规则来源与父仓库

上面一行导入本仓库的 `AGENTS.md`。C 代码风格、三级注释、测试要求、
“子系统：具体修改”的提交标题和当前 GPGPU 实验状态都以它为准；本文件只补充
跨文件的软件栈结构和 GPGPU 专用命令。规则要改时，改 `AGENTS.md`，
不在这里另存一份。

不要删除这行导入。Claude Code 默认只在没有 `CLAUDE.md` 时加载 `AGENTS.md`；
有了本文件，`AGENTS.md` 只能经导入进入上下文。

本仓库通常作为 gpu_study 父仓库的子模块，检出在 `4.experiments/gpu-study-qemu`。
从本目录启动时，Claude Code 会逐级向上加载父仓库根目录的 `CLAUDE.md`，
并经其中的导入加载父仓库的 `AGENTS.md`。修改本仓库代码时遵循本仓库规则；修改 `../` 下的
规格、大纲和硬件手册时遵循父仓库规则。单独克隆本仓库时，`../` 下的参考材料不存在。

## 2. 分支与父仓库同步

作为子模块检出时，本仓库通常处于 detached HEAD，直接提交的新提交不属于任何分支。
提交前先确认 HEAD 与 `main` 指向同一提交，再运行 `git switch main`；两者不同时，
切换会改变工作区代码，先向用户说明。

本仓库提交后，父仓库还要另行提交新的子模块指针，并按需同步 `../` 下的文档。
`software/gpgpu/PROTOCOL_V3.md` 列出协议从版本 2 到版本 3 的差异，用来逐条更新
父仓库的规格和硬件手册；两边不一致时，协议常量以 `software/gpgpu/include/gpgpu/` 为准。

## 3. GPGPU 软件栈：从应用到设备的五层调用

一次计算依次经过五层，每层只调用下一层公开的接口。除 `hw/gpgpu/` 位于仓库根目录外，
图中路径都在 `software/gpgpu/` 下：

```text
demos/gpgpu-demo.c      准备数据、调用运行时、逐元素比较结果
  │ gpgpu_rt_*()
lib/rt/                 装载 Kernel、组 AQL 包、准备 Signal 并等待
  │ gpgpuKmt*()
lib/kmt/                HSAKMT 风格薄层，一个函数基本对应一个 ioctl
  │ ioctl（include/gpgpu/uapi/gpgpu_drm.h）
linux/                  DRM 驱动 gpgpu_pci.ko：GEM、页表、管理队列、IH、事件
  │ BAR0 寄存器，以及内存中的 MQD、Ring、Signal
hw/gpgpu/ + firmware/   QEMU 设备模型与 RV64 CP 裸机固件；RV32 执行器运行 Kernel
```

这条路径还依赖几项分散在不同文件中的约定：

- 五层共用 `software/gpgpu/include/gpgpu/` 中的协议头。`AGENTS.md` 写的
  `include/gpgpu/` 就是这个目录，不是 QEMU 顶层的 `include/`。QEMU 由顶层
  `meson.build` 的 `-iquote` 把该目录加入头文件搜索路径，固件、驱动和用户库的
  Makefile 用 `-I` 指向同一目录。
- 各组件的版本号都由 `gpgpu_version.h` 的 `GPGPU_PROTOCOL_VERSION`（当前为 3）派生，
  包括设备的 `DEV_VERSION`、固件握手检查的 `VM_VERSION` 和 Kernel 描述对象的
  `kernel_abi_version`。设备、固件、驱动和用户库必须一起更新；版本混用时，
  驱动 probe 或固件握手会失败。
- CP QTest 和 Linux 全栈测试以双进程运行设备。设备实例 `gpu0` 位于
  `qemu-system-riscv64 -machine gpgpu-cp` 进程中，经 `x-vfio-user-server` 导出到
  unix socket；`qemu-system-aarch64` 中的 A57 客体用 `vfio-user-pci` 连接这个 socket。
  CP QTest 按 `QTEST_GPGPU_REMOTE_BINARY` 自行启动远端进程，Linux 全栈测试由
  `software/gpgpu/scripts/gpgpu_remote.py` 启动和管理远端进程。寄存器级 QTest
  则把设备放在 aarch64 进程内。
- GPUVM 开关是设备属性 `-global gpgpu.gpuvm=on|off`，双进程时加在远端进程的
  命令行上，默认开启；关闭时，用户地址就是 GPU 物理地址。
- 设备模型按硬件模块分为 `cp/`、`compute/`、`mem/` 和 `irq/`，模块之间只调用
  对方头文件中的函数。驱动的对象关系、寿命和锁顺序写在
  `software/gpgpu/linux/gpgpu_drv.h` 文件头，各文件分工见
  `software/gpgpu/README.md` 第 0 节。

## 4. GPGPU 构建与测试命令

以下命令在本仓库根目录运行：

```sh
# 设备寄存器 6 项、RV64 CP 队列 5 项：先构建固件，再在 build/arm64-qemu 中运行 ninja，按 TAP 核对 11/11
make -f Makefile.camp test-gpgpu-qtest

# 只运行一个用例：GLib 的 -p 选择用例路径，路径见测试文件末尾的 qtest_add_func
QTEST_QEMU_BINARY=build/arm64-qemu/qemu-system-aarch64 \
QTEST_GPGPU_REMOTE_BINARY=build/arm64-qemu/qemu-system-riscv64 \
QTEST_GPGPU_REMOTE_LOG_DIR=build/gpgpu-qtest-logs \
QTEST_GPGPU_FIRMWARE=build/gpgpu-firmware/gpgpu-cp.bin \
build/arm64-qemu/tests/qtest/gpgpu-cp-test --tap -p /gpgpu/cp/dispatch
# 寄存器用例只需要 QTEST_QEMU_BINARY，例如 gpgpu-regs-test --tap -p /gpgpu/regs/irq

make -C software/gpgpu                  # 用户库、gpgpu-demo 与三个 RV32 Kernel，输出到 build/gpgpu-stack
make -C software/gpgpu/firmware         # RV64 CP 固件 build/gpgpu-firmware/gpgpu-cp.bin
make -C software/gpgpu check            # RV32 教学汇编器回归
make -C software/gpgpu check-bootstrap  # 基础环境引导脚本回归

# Linux 全栈：GPUVM 开、关各运行一次三个计算示例；缺少内核环境时在检查阶段退出
make -f Makefile.camp test-gpgpu-stack
make -C software/gpgpu/linux run IOMMU=smmuv3 GPUVM=off   # 启动客体并进入交互 shell
make -C software/gpgpu/linux module                       # 在开发客体中重编驱动模块
```

- `make -f Makefile.camp configure`、`build` 和 `test` 只面向课程的 RISC-V CPU、
  SoC 与 Rust 实验，产物在 `build/`，不包含 GPGPU 需要的 aarch64 目标。
- `test-gpgpu-qtest` 只在已配置的 `build/arm64-qemu` 中运行 ninja，不负责配置。
  `build/arm64-qemu` 由 `software/gpgpu/scripts/gpgpu_linux.py` 的 `ensure_qemu()`
  配置，目标为 `aarch64-softmmu,riscv64-softmmu`，并启用 `--enable-vfio-user-server`。
  `gpgpu_linux.py` 调用 `ensure_qemu()` 前会先检查内核环境；全新检出且尚无内核环境时，
  照 `ensure_qemu()` 中的完整参数手动运行 configure。
- Linux 全栈所需的内核环境按 `software/gpgpu/README.md` 第 5.1 节，用
  `bootstrap-env.py` 和 `kernel-build.py` 准备。
- 按 `AGENTS.md` 核对通过数量时，QTest 看 `build/gpgpu-qtest.log` 中的 TAP `ok` 行；
  Linux 全栈看 `build/gpgpu-kernel-gpuvm/core/on/`、`core/off/` 下的 `result.json`
  和 `compute.log`，两种状态都通过后才会生成 `core/summary.json`。
