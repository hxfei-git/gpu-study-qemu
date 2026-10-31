# QEMU GPGPU 软件栈：统一访存、AQL 与 CSR 启动接口

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真工具 |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU | Graphics Processing Unit | 图形处理器 |
| CUDA | Compute Unified Device Architecture | 统一计算设备架构 |
| ARM64 | Arm 64-bit Architecture | Arm 64 位架构 |
| RV32 / RISC-V | 32-bit RISC-V / Reduced Instruction Set Computer V | 本实验计算执行器使用的 32 位精简指令集 |
| RV64 | 64-bit RISC-V | 本实验原生 CP 使用的 64 位 RISC-V 模型 |
| API / ABI | Application Programming Interface / Application Binary Interface | 应用编程／二进制接口 |
| PCI / BAR | Peripheral Component Interconnect / Base Address Register | 外设互连／基址寄存器 |
| DMA | Direct Memory Access | 直接内存访问 |
| IOMMU | Input/Output Memory Management Unit | 输入输出内存管理单元 |
| SMMUv3 | System Memory Management Unit version 3 | Arm 系统内存管理单元第三版 |
| IOVA | I/O Virtual Address | 设备 DMA 虚拟地址 |
| RAM / VRAM | Random Access Memory / Video RAM | 系统内存／显存 |
| H2D / D2H | Host to Device / Device to Host | 主机到设备／设备到主机 |
| MSI-X / IRQ | Message Signaled Interrupts Extended / Interrupt Request | 扩展消息中断／中断请求 |
| MMIO | Memory-Mapped I/O | 内存映射输入输出 |
| SIMT | Single Instruction, Multiple Threads | 单指令多线程 |
| FP32 / FP16 / FP8 / FP4 | Floating Point 32 / 16 / 8 / 4-bit | 对应位宽的浮点格式 |
| BF16 | Brain Floating Point 16-bit | 16 位浮点格式 |
| ReLU | Rectified Linear Unit | 线性整流函数 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| ACK | Acknowledgment | 确认；本版用于确认已经读取执行结果 |
| TCG | Tiny Code Generator | QEMU 动态翻译后端 |
| TAP | Test Anything Protocol | 测试结果输出协议 |
| TLS | Transport Layer Security | 传输层安全协议 |
| JSON | JavaScript Object Notation | JavaScript 对象表示法 |
| URL | Uniform Resource Locator | 统一资源定位符 |
| ISO | International Organization for Standardization | 此处指 ISO 9660 光盘镜像 |
| APK | Alpine Linux Package | Alpine 软件包 |
| SHA-256 / SHA-512 | Secure Hash Algorithm 256-bit / 512-bit | 256／512 位安全散列算法 |
| BDF | Bus, Device, Function | PCI 总线、设备、功能号 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| ROCr | ROCm Runtime | ROCm 的异构计算运行时 |
| HSAKMT | HSA Kernel Mode Thunk | HSA 内核接口封装层；此处仅借鉴接口风格 |
| KMT | Kernel Mode Thunk | 本仓库 `lib/kmt/` 薄层的简称 |
| QOM | QEMU Object Model | QEMU 对象模型 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| MQD / HQD | Memory Queue Descriptor / Hardware Queue Descriptor | 内存／硬件队列描述符 |
| KIQ / HIQ | Kernel Interface Queue / HSA Interface Queue | 内核接口／HSA 接口管理队列 |
| IH | Interrupt Handler | 本实验中保存完成与错误通知的中断处理模块 |
| VMID | Virtual Memory Identifier | 虚拟地址空间编号 |
| CPU | Central Processing Unit | 中央处理器 |
| GNU / GCC | GNU's Not Unix / GNU Compiler Collection | GNU 工具集／GNU 编译器集合 |
| DRM / GEM | Direct Rendering Manager / Graphics Execution Manager | Linux 图形设备框架／内存对象管理 |
| BO | Buffer Object | 持有固定存储的缓冲对象 |
| GPUVM / GPUVA | GPU Virtual Memory / GPU Virtual Address | GPU 虚拟内存／GPU 虚拟地址 |
| VM | Virtual Memory | 本文指一个文件持有的 GPU 地址空间 |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| VPN | Virtual Page Number | 虚拟页号，即 GPUVA 除以 4 KiB |
| PTE / PDE | Page Table Entry / Page Directory Entry | 页表叶子项／页目录项 |
| TTM | Translation Table Maps | 图形对象存储位置与迁移管理框架 |
| VMA | Virtual Memory Area | CPU 进程的虚拟内存映射区域 |
| CSR | Control and Status Register | 控制与状态寄存器；本实验用于读取启动信息 |
| SGPR / VGPR | Scalar / Vector General-Purpose Register | 标量／向量通用寄存器；本版仅保存独立启动状态 |

## 0. 代码结构与分层

一次计算从应用一路走到 RV32 线程，依次经过五层代码。每一层只调用下一层公开的接口，所有层共用 `include/gpgpu/` 中的协议头：

```text
demos/gpgpu-demo.c          准备数据、调用运行时、比较结果
  │ gpgpu_rt_*()
lib/rt/                     装载 Kernel、组 AQL 包、准备 Signal、等待完成
  │ gpgpuKmt*()
lib/kmt/                    HSAKMT 风格薄层：一个函数基本对应一个 ioctl
  │ ioctl（include/gpgpu/uapi/gpgpu_drm.h）
linux/                      DRM 驱动：GEM 对象、页表、管理队列、IH、事件
  │ BAR0 寄存器、内存中的 MQD/Ring/Signal（include/gpgpu/*.h）
hw/gpgpu/ + firmware/       QEMU 设备模型与 RV64 CP 固件
```

协议头位于 `software/gpgpu/include/gpgpu/`，不是 QEMU 顶层的 `include/`。各组件都以 `gpgpu/...` 的形式包含这些头文件：QEMU 由顶层 `meson.build` 用 `-iquote` 把 `software/gpgpu/include` 加入搜索路径；固件、驱动和用户库的 Makefile 用 `-I` 指向同一位置。

协议头按内容拆分：`gpgpu_regs.h` 是 BAR0 寄存器，`gpgpu_cp_map.h` 是 CP 私有地址空间，`gpgpu_queue_abi.h` 是 MQD、管理命令、AQL 包、Kernel 描述对象与 Signal，`gpgpu_ih_abi.h` 是 IH 记录，`gpgpu_vm_abi.h` 是页表项与 GPUVA 布局，`gpgpu_version.h` 是协议版本，`uapi/gpgpu_drm.h` 只描述 ioctl。用户态只包含后三者中的 `uapi/gpgpu_drm.h`、`gpgpu_queue_abi.h` 与 `gpgpu_vm_abi.h`，看不到寄存器。

设备模型 `hw/gpgpu/` 按硬件模块分目录，每个模块只访问自己的状态，跨模块通过对方头文件中的函数：

- `gpgpu.c` 负责 QOM/PCI 组装、全局寄存器和复位顺序；`bar0.c` 把 BAR0 每个 0x100 字节的寄存器组挂成独立子区域，统一检查访问宽度，并提供 CP 本地寄存器窗口。
- `cp/`：`cp_ctrl` 管理原生 CP 的复位与启动，`cp_window` 是 CP 访问 GPU 地址的 64 KiB 窗口，另有 `hqd` 与 `doorbell`。
- `compute/`：`dispatch` 处理执行器寄存器与 CP 握手，`scheduler` 按 warp 分段推进，`rv32` 是解释器，`lane_mem` 负责取指与 LW/SW。
- `mem/`：`vram`、`phys`（GPU 物理地址到 VRAM 或 Outbound 的路由）、`gpuvm`（用户地址翻译与故障记录）、`dma`。
- `irq/`：`irq` 是中断锁存与 PCI 通知，`ih` 是通知记录 Ring。

CP 固件 `firmware/` 中，`main.c` 是启动检查与调度循环，`ring.c` 负责取包与回收，`mgmt.c` 处理管理命令，`aql.c` 处理派发包，`ih.c` 负责通知与错误，`mem.c` 经访存窗口读写 GPU 地址并恢复访存异常。

Linux 驱动 `linux/` 按对象分文件：`gpgpu_main.c`（PCI）、`gpgpu_dev.c`（设备生命周期与固件）、`gpgpu_mgmt.c`、`gpgpu_ih.c`、`gpgpu_drm.c`（ioctl 与会话）、`gpgpu_bo.c`、`gpgpu_vram.c`、`gpgpu_vm.c`、`gpgpu_queue.c`、`gpgpu_event.c`。对象关系、寿命与锁的顺序写在 `gpgpu_drv.h` 文件头。

协议当前为版本 4，与版本 3 的差异见 [PROTOCOL_V4.md](./PROTOCOL_V4.md)；更早的版本 2 到 3 的差异见 [PROTOCOL_V3.md](./PROTOCOL_V3.md)。

## 1. 应用提交与统一访存

Host 指 ARM64 Linux 客体中的 CPU 与应用。应用通过 DRM render node 创建 GEM 对象，由 HSAKMT 风格库提交 AQL 包；RV64 CP 处理队列，RV32 计算执行器运行 Kernel。设备属性 `gpuvm=on/off` 决定用户地址含义，默认开启。两种状态使用同一套对象、队列、等待和回收接口。

```text
应用：创建并绑定代码、描述符、参数和数组
  → 创建 AQL Queue，填写包体并发布 Header
  → 8 字节写 Doorbell
  → RV64 CP 读取描述符、派发 RV32 Kernel
  → Signal → 通知槽 → IH → INVALID Header → 消费进度
  → Host 复查 Signal、读取并比较结果
  → 排空并卸载 Queue → 销毁事件 → 解绑并释放对象

CP 用户访问 / RV32 取指与数据访问
  ├─ gpuvm=on：GPUVA → 页表与权限检查 → GPU 物理地址
  └─ gpuvm=off：输入就是 GPU 物理地址
                      ↓
                公共物理访问入口
                  ├─ VRAM
                  └─ Outbound → Host IOVA → PCIe DMA
```

**[DESIGN]** GPU 物理地址为 32 位，布局固定：`[0, vram_size)` 属于 VRAM，`[vram_size, 0x8000_0000)` 是不可路由的空洞，`[0x8000_0000, 0x1_0000_0000)` 属于 Outbound。窗口起点与 VRAM 容量无关，长度为 2 GiB。Host IOVA `0x4100_0000` 对应 GPU 物理地址 `0xc100_0000`；IOVA 0 对应 `0x8000_0000`，允许正常使用。

Outbound 布局由设备固定，BAR0 `0x0b00` 起的布局寄存器只读；驱动在启动 CP 前读回核对，再写 `OUTBOUND_CONTROL` 打开窗口并读取确认状态。窗口由整个设备共用，队列运行期间不重配。DMA 分配统一采用 31 位 mask，保留原始 DMA 地址供 DMA API 释放和映射，转换后的 GPU 物理地址写入设备协议。Host BO 仍逐对象使用 `dma_alloc_coherent()`。

Outbound 只决定访问路由。Host 存储或 IOMMU 映射不存在时访问仍会失败。SMMUv3 功能保留；开启后，PCIe DMA 再由系统 IOMMU 将 IOVA 翻译成 Host 物理地址。

<a id="stage-three-gpuvm"></a>

## 2. GPUVM、对象绑定与资源回收

GPUVM 开启时使用三级 4 KiB 页表、两个用户 VMID 和 64 组 × 4 路的组相联 TLB；驱动绑定或解绑对象后只失效该对象的 VPN 范围，启用和停用上下文时失效整个 VMID，设备同步完成并以 ACK 确认。PTE 保存地址域、SYSTEM 位和有效、读、写、执行权限：Host 页写 IOVA 并置 SYSTEM，设备换算为 `0x8000_0000 + IOVA`；VRAM 页写 VRAM 偏移。根目录使用 GPU 物理地址，驱动的中间表页都在 VRAM，遍历通过公共物理入口读取，不再次进入 GPUVM 翻译。

用户 GPUVA 的唯一可分配区间为 `[0x1000, 0xffff_f000)`，末页保存内部通知槽。`0x8000_0000` 与 `0x9000_0000` 均为普通 GPUVA，不再代表控制页或参数窗口。GPUVM 关闭时不创建页表，能力查询返回零个 GPUVA 区间，MAP 返回对象固定的 GPU 物理地址；非零 `fixed_va` 请求返回不支持。

```text
GEM handle ─────────┐
GPU 绑定引用 ────────┼→ 固定 Host / VRAM 存储
CPU mmap 引用 ──────┘

关闭 handle：只解除句柄引用
Queue 卸载确认：允许解绑
解绑：开启 GPUVM 时撤销页表并确认 TLB 失效
最后引用结束：归还存储
```

两种状态均建立独立绑定引用；地址为零或没有页表不能作为释放条件。Queue 存活期间冻结绑定，代码、参数及数组随绑定保活。未知管理完成结果或无法确认访问结束时继续保留资源。正常关闭按 Queue、事件、绑定、存储顺序执行，CPU mmap 独立保活实际存储。

全设备保留两个用户 Queue 名额，每上下文一条。关闭 GPUVM 后，各进程共享物理地址空间，不提供页权限隔离。VRAM 首页及固件区继续受用户访问保护；管理队列、MQD、管理完成区、IH 与 COPY 端点统一使用 GPU 物理地址。

CP 的管理域和用户域显式区分，不根据 `VMID == 0` 猜测地址含义。CP 自己的代码、栈和本地访存窗口继续使用 RV64 CP 地址空间。

## 3. RV32 Kernel 的启动接口

**[DESIGN]** 每 Warp 保存九个只读共享启动值：Block 编号、Block 尺寸、Grid 中的 Block 数量。每 Lane 保存三个线程坐标。新 Warp 创建时填写一次，分段继续执行时保留；X 维变化最快。启动状态独立于 Kernel 可覆盖的通用寄存器。

```text
0xCC0～0xCC2：THREAD_ID_X / Y / Z
0xCC3～0xCC5：BLOCK_ID_X / Y / Z
0xCC6～0xCC8：BLOCK_DIM_X / Y / Z
0xCC9～0xCCB：GRID_DIM_X / Y / Z
0xF14：mhartid，保留原有编码
```

`csrr` 汇编为 `CSRRS rd, csr, x0`。执行器仅接受此只读形式；`rd=x0` 也先检查 CSR 是否有效。未知 CSR、写入及其他未支持形式均返回指令错误。汇编器接受数字与 `.equ` 常量，并检查 CSR 编号范围。

`a0` 直接保存 `kernarg_address`：开启 GPUVM 时为 GPUVA，关闭时为 GPU 物理地址。参数读取按普通内存处理。16 字节 Kernel 描述符在偏移 `0x0c` 保存 `kernel_abi_version=4`，其他值被拒绝；AQL 包仍为 64 字节。设备、固件、驱动、用户库与描述对象共用协议版本 4，组件必须一起更新。

## 4. 公共库与三个计算示例

`GpgpuKmtMemory` 由库分配，调用者只读其中的 CPU 映射 `cpu` 和唯一的设备地址 `gpu_address`，不需要区分 Host 地址、VRAM 偏移或 GPUVA。`gpgpuKmtAllocMemory()` 先创建整对象再绑定，权限由调用者指定：代码为读与执行，普通数据为读写。参数中写入 32 位地址前须检查范围，运行时用 `gpgpu_rt_arg_address()` 完成这一检查。

KMT 中的 `gpgpuKmtReserve()`、`gpgpuKmtPublish()`、`gpgpuKmtWaitSignal()` 和 `gpgpuKmtDrainQueue()` 分别负责槽位预留、Header 与 Doorbell 发布、Signal 与事件等待、排空。运行时 `gpgpu_rt_dispatch()` 按顺序调用它们完成一次派发。默认等待先主动检查约 200 μs，再进入事件等待，唤醒后复查 Signal。

`gpgpu-demo` 链接 `libgpgpu.a`，经运行时把代码、描述符和计算数组放在 VRAM；参数、Ring、进度和 Signal 放在 Host BO，经 Outbound 访问。用于 COPY 的 Host BO 也在 Queue 创建前准备。三个例子保持原输入和比较规则：

- 向量加法比较 17,003 个结果，Grid 为 266×1×1，Block 为 64×1×1。
- 矩阵乘法计算 19×17 · 17×23，比较 437 个结果，Grid 为 3×3×1，Block 为 8×8×1。
- ReLU 比较 513 个结果，Grid 为 6×1×1，Block 为 96×1×1，并保留负零结果检查。

任一数值比较、提交、等待或正常资源回收失败，程序返回非零退出码。

<a id="build-and-validation"></a>

## 5. 构建、运行与验证结果

本节是依赖、构建与测试命令和当前验证结果的唯一说明，其他文档只链接到这里。命令都在仓库根目录运行。

QEMU 配置和设备回归不需要 Linux 客体内核。宿主机需要 QEMU 的 C/Python 构建依赖、Ninja、`pkg-config`、GLib 和 pixman 开发包，另需 `libjson-c-dev`、`libcmocka-dev` 和固件使用的 `riscv64-unknown-elf-gcc`；GPGPU 构建不需要 Rust。Ubuntu 24.04 上，后三项可用 `sudo apt install libjson-c-dev libcmocka-dev gcc-riscv64-unknown-elf` 安装。

`libjson-c-dev` 和 `libcmocka-dev` 是 vfio-user 服务端子项目 libvfio-user 的依赖。首次配置需要联网：配置参数带 `--enable-download`，Meson 会按 `subprojects/*.wrap` 下载缺少的子项目，其中 libvfio-user 从 gitlab.com 拉取，再应用 `subprojects/packagefiles/libvfio-user/` 中的本项目补丁。下载结果留在 `subprojects/` 下，不入库；离线环境需要先把这些目录拷进来。

```sh
# 配置 aarch64 Host 与 riscv64 CP，不需要客体内核；默认目录 build/arm64-qemu
make -f Makefile.gpgpu configure
# 构建两个 QEMU、两个 QTest 程序与 RV64 CP 固件；缺少 build.ninja 时自动配置
make -f Makefile.gpgpu build JOBS=4
# 设备寄存器与双进程 CP 队列回归，核对退出状态、TAP 计划与结果
make -f Makefile.gpgpu test-gpgpu-qtest

# 用户库、gpgpu-demo 与三个 RV32 Kernel（输出到 build/gpgpu-stack），以及教学汇编器回归
make -C software/gpgpu all check
# RV64 CP 固件，输出 build/gpgpu-firmware/gpgpu-cp.bin
make -C software/gpgpu/firmware
# 基础环境引导脚本的缓存与防覆盖回归，不需要设备或网络
make -C software/gpgpu check-bootstrap

# Linux 全栈：GPUVM 开、关各运行一次三个计算示例；缺少内核环境时在检查阶段退出
make -f Makefile.gpgpu test-gpgpu-stack
# 启动远端设备与开发客体，进入交互 shell
make -C software/gpgpu/linux run IOMMU=smmuv3 GPUVM=off
# 在开发客体中重编驱动模块
make -C software/gpgpu/linux module
```

构建目标的简要说明可用 `make -f Makefile.gpgpu help` 查看。只运行一个 QTest 用例时，先用 `-l` 列出带架构前缀的路径，再用 GLib 的 `-p` 选择：

```sh
QTEST_QEMU_BINARY=build/arm64-qemu/qemu-system-aarch64 \
QTEST_GPGPU_REMOTE_BINARY=build/arm64-qemu/qemu-system-riscv64 \
QTEST_GPGPU_REMOTE_LOG_DIR=build/gpgpu-qtest-logs \
QTEST_GPGPU_FIRMWARE=build/gpgpu-firmware/gpgpu-cp.bin \
build/arm64-qemu/tests/qtest/gpgpu-cp-test --tap -p /aarch64/gpgpu/cp/dispatch
# 寄存器用例只需要 QTEST_QEMU_BINARY，例如 -p /aarch64/gpgpu/regs/irq
```

CP QTest 和 Linux 全栈都以双进程运行设备。设备实例 `gpu0` 位于 `qemu-system-riscv64 -machine gpgpu-cp` 进程中，经 `x-vfio-user-server` 导出到 unix socket；`qemu-system-aarch64` 中的 A57 客体用 `vfio-user-pci` 连接这个 socket。CP QTest 按 `QTEST_GPGPU_REMOTE_BINARY` 自行启动远端进程，Linux 全栈由 `software/gpgpu/scripts/gpgpu_remote.py` 启动和管理远端进程；寄存器 QTest 则把设备放在 aarch64 进程内。GPUVM 开关是设备属性 `-global gpgpu.gpuvm=on|off`，双进程时加在远端进程的命令行上，默认开启。

`configure` 与 Linux 入口共用 `gpgpu_linux.py` 中的一份配置参数。`build` 和 `test-gpgpu-qtest` 可用 `GPGPU_BUILD_DIR=build/verify-qemu` 把 QEMU 的配置、构建和 QTest 程序放到另一目录，不覆盖默认构建目录。固件 `build/gpgpu-firmware/`、QTest 日志 `build/gpgpu-qtest.log` 和远端日志 `build/gpgpu-qtest-logs/` 仍是共用路径，换目录运行 QTest 也会覆盖这些日志。这个变量不改变 Linux 全栈使用的 `build/arm64-qemu`。

> **[SOURCE]** 共享配置参数见 [gpgpu_linux.py](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_linux.py:44:1>) [第 44 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_linux.py:44:1>)，配置函数与 Linux 入口分别见[第 161 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_linux.py:161:1>)、[第 168 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_linux.py:168:1>)；首次构建的自动配置见 [Makefile.gpgpu](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/Makefile.gpgpu:33:1>) [第 33～45 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/Makefile.gpgpu:33:1>)。

Linux 全栈使用 `build/gpgpu-kernel-gpuvm/`，内核为 `6.18.57-0-gpgpu-virt`，包含 DRM/GEM 与 `GPU_BUDDY`；缺少该环境时按[第 5.1 节](#offline-materials)准备。核心计算固定使用 ARM virt、A57、1 GiB RAM、IOMMU=none 与双进程设备。每次运行都会覆盖以下输出：QTest 写入 `build/gpgpu-qtest.log`；全栈两种状态分别写入 `build/gpgpu-kernel-gpuvm/core/on/` 和 `core/off/`，其中 `compute.log` 保存逐元素比较结果，`console.log` 保存客体日志，`result.json` 保存实际结果与组件散列，两种状态都通过后另写 `core/summary.json`。

下列结果对应协议版本 4 的提交 `0f38bff`，在干净的工作树上运行全部入口得到；本说明所在的提交只更新验证记录，不改代码。记录复制在 `build/gpgpu-validation/`，不会被下一次运行覆盖：

- QTest 13/13，教学汇编器 7/7，基础环境引导回归 6/6。
- Linux 全栈复用 `6.18.57-0-gpgpu-virt` 开发环境，驱动模块按该提交的源码重编。GPUVM 开、关都完成向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513，客体退出码为 0；示例输出 `GPGPU DRM ABI=4`，GPUVM 状态和客体内核日志检查通过。
- 两种模式的组件 SHA-256 相同：固件 `d2415a71ca2c91c34325727c70b49416ebbd2db5ef815684ac2edefd60bb4a53`，模块 `0a42330c1a1bf1c54638841fadaaba22ad3e886b76b652be867cca7f40d496a0`，示例 `93f6ce4409fb16bba77a493b3c440304797dc6db9ab61609c312f21d4f768c49`。

> **[SOURCE]** 本地运行记录：[命令与结果汇总](../../build/gpgpu-validation/runtime-verification.json)、[QTest 输出](../../build/gpgpu-validation/qtest-build.log)、[用户库与汇编器](../../build/gpgpu-validation/userspace-build-check.log)、[引导回归](../../build/gpgpu-validation/bootstrap-check.log)、[全栈输出](../../build/gpgpu-validation/stack-validation.log)、[两种模式汇总](../../build/gpgpu-validation/core/summary.json)。`build/` 产物不纳入 Git。

**[BOUNDARY]** QTest 与全栈需要创建 Unix socket，在禁止 socket 的沙箱中运行会失败。上述结果不覆盖从零引导开发环境、SMMUv3、多进程或资源生命周期专项。

<a id="offline-materials"></a>

### 5.1 从零准备基础环境与离线材料

先运行独立引导入口，再重建带 DRM/GPU_BUDDY 的实验内核：

```sh
python3 software/gpgpu/scripts/bootstrap-env.py --list-inputs
python3 software/gpgpu/scripts/bootstrap-env.py
python3 software/gpgpu/scripts/kernel-build.py --output-dir build/gpgpu-kernel-gpuvm
make -f Makefile.gpgpu test-gpgpu-stack
```

默认输出目录 `build/gpgpu-linux-module` 里已有通过校验的基础环境时，`bootstrap-env.py` 会直接复用。要验证从零流程，须用 `--output-dir` 另选一个空目录，并把下面列出的材料放进去。

引导入口复用 `kernel-build.py` 的 `download_checked()`。固定 ISO 为 Alpine 3.24.2 aarch64；80 个编译依赖的版本、URL 和 SHA-256 保存在 [bootstrap-packages.json](./linux/kernel/bootstrap-packages.json)，两个 6.18.57 内核包沿用 [cached-packages.json](./linux/kernel/cached-packages.json)。宿主机先验证全部文件，客体只从本地安装并保留 APK 签名检查，不运行在线升级。引导脚本只格式化新建的暂存盘，已有但未通过校验的开发盘不会被覆盖。安装、重启和内核版本检查都成功后，才发布 `environment.json`。

本机材料的状态如下，可用 `--list-inputs` 导出完整清单后逐项核对：

- 两个 6.18.57 内核包的固定 URL 已返回 404，上游无法再下载。本机 `build/gpgpu-linux-module/apk-cache/` 中有散列一致的副本，`build/gpgpu-kernel-gpuvm/`、`build/gpgpu-kernel/` 和 `build/gpgpu-kernel-cross/` 的 `apk-cache/` 下另有副本。文件名和 SHA-256 为：
  - `linux-virt-6.18.57-r0.ad6fb931.apk`：`b3820fee22e0366a15f7c139e5bc00348e333f3e7f77802271df7ded795aee4f`；
  - `linux-virt-dev-6.18.57-r0.c57a20a2.apk`：`fd3462db5bd0573287ff4aeba0bc35a040af1d218a8871180b134418afad1cac`。
- ISO 的本机副本是 `build/gpgpu-arm64-linux/alpine-virt-3.24.2-aarch64.iso`，散列与脚本中的固定值一致。引导脚本只在输出目录的 `downloads/` 下查找，使用前要先复制过去。
- 80 个编译依赖本机没有缓存，需要联网按清单 URL 下载，或者从其他机器按 `--list-inputs` 给出的路径拷入，再使用 `--offline`。

缓存清单只保存校验信息，不包含 APK 内容。脚本会在输出目录的 `missing-inputs.json` 中列出实际缺少文件的完整路径、URL、SHA-256 和错误原因；补齐后可用 `--offline` 只校验本地缓存。基础环境的 `environment.json`、`developer.raw`、内核和 initramfs 都由引导生成。

重建内核还需要以下材料；网络不能访问时，按清单的固定 URL 获取原文件并放到对应位置，不能改版本或校验值：

- `build/gpgpu-kernel-gpuvm/downloads/linux-6.18.tar.xz`、`patch-6.18.57.patch.xz`：URL 与 SHA-512 在 [sources.json](./linux/kernel/sources.json)。本机这两个文件已通过校验；输出目录缺少它们时，脚本先从 `build/gpgpu-kernel/downloads/` 复制。
- `build/gpgpu-kernel-toolchain/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz`：URL 与 SHA-256 在 [cross-toolchain.json](./linux/kernel/cross-toolchain.json)。
- `build/gpgpu-kernel-host-tools/downloads/`：按 [cross-host-dependencies.json](./linux/kernel/cross-host-dependencies.json) 的 `file` 字段放置各包；工具链脚本使用同一清单校验。
- `build/gpgpu-kernel-gpuvm/apk-cache/`：补充包按 [build-dependencies.json](./linux/kernel/build-dependencies.json) 的 `cache_name` 放置，URL 与 SHA-256 也在该清单中。

清理 `build/` 时，下面这些不能当作可重建的产物删除：

- `build/gpgpu-linux-module/`：基础开发环境，`kernel-build.py` 默认从这里读取 `--base-environment`，其中两个内核包上游已经 404；
- `build/gpgpu-kernel-gpuvm/`：Linux 全栈当前使用的内核环境和开发盘；
- `build/gpgpu-validation/`：当前验证结果的记录；
- `build/gpgpu-arm64-linux/` 中的 ISO，以及 `build/gpgpu-kernel/downloads/`、`build/gpgpu-kernel-toolchain/` 中已校验的源码包和工具链压缩包，删除后要重新联网下载。

## 6. 历史验收记录

以下记录属于当时的固定提交与旧协议。旧测试源文件、运行入口和兼容实现已从当前代码删除；这些数量、命令和日志只用于查阅历史，不构成当前验收要求。

本仓库主线做过一次提交历史重构，提交号全部改变，各提交的文件树不变。本节和 `docs/` 中的历史提交号都在旧主线上，旧主线保存在远端分支 `archive/main-before-contributors-cleanup-20261031`；删除这个分支会让这些固定提交链接失效。

<a id="stage-three-validation"></a>

### 6.1 第三阶段验收与代码快照（历史）

当时按固定存储 GPUVM 接入 DRM 文件、GEM 对象与 GPUVA 绑定，再让 CP 和执行器按用户 VMID 翻译访问。关闭时先卸载队列，再失效并解除映射，最后回收引用、存储和 VMID。模式 2 使用 DRM render node，仍保留旧模式 0/1 和 RV32 控制页；这些兼容路径和控制页后来在统一访存重构中删除。

```text
DRM 文件 → GEM 对象与 GPUVA 映射 → 用户 VMID
  → CP / 执行器翻译访问 → 结果与通知
  → 卸载队列 → 失效并解除映射 → 回收引用、存储和 VMID
```

Queue 存活期间冻结映射，不接入 TTM、迁移、换页或抢占。异常保留的 GEM 对象独立持有 DRM 设备引用，直到设备访问停止后再回收。

2026-10-19，本轮实现以子仓库提交
`c5acaf7a05a6852f94985197c15c2e84953fe991` 为起点，固定实现提交为
[`3600ff795dde9069a6e4f1405f6950a61ea75faa`](https://github.com/hxfei-git/gpu-study-qemu/commit/3600ff795dde9069a6e4f1405f6950a61ea75faa)。
验收代码归档为 `build/gpgpu-stage3-source.tar.gz`，逐文件 SHA-256 与起点提交
记录在 `build/gpgpu-stage3-source.json`；运行结果索引为
`build/gpgpu-stage3-validation.json`。归档保留提交前的验收材料，
实现源文件与固定提交一致；`build/` 产物不进入 Git。

- CP QTest 18/18：原 16 项及两组 GPUVM 检查，后者覆盖全部用户存储路径、
  故障归属、另一 VM 继续运行、非法页表、部分写入、失效确认和复位范围。
- GPUVM Linux 五组验收在 `none`、`smmuv3` 下各 5/5，卸载各 1/1；SMMUv3
  实际 domain 为 `DMA`。覆盖完整跨页向量、双进程同地址隔离、故障退出、
  撤销重映射、GEM 句柄与 CPU 映射寿命、256 轮句柄创建／关闭竞争，以及 VMID 回收复用。
- 未知结果保留 3/3：映射管理请求返回初值、卸载请求返回未定义值、失效没有
  ACK。每项均核对旧 VMID 保留、另一个名额可用、第三个申请失败，并在停止
  设备后清理；内核无告警。GEM 对象独立持有 DRM 设备引用，异常保留期间
  不会提前销毁其 CPU 映射管理结构。
- 旧物理队列 5/5、卸载 1/1。单／双进程与 `none/smmuv3` 四种同步组合均
  通过：每种装卸载 5/5、17,953 个示例输出、运行时检查 68/68、卸载重载 2/2。
- 原 ARM64、RISC-V 设备 QTest 各 21/21；用户库及测试构建通过，汇编检查 4/4。

GPUVM 日志与哈希在 `build/gpgpu-kernel-gpuvm/gpuvm/{none,smmuv3,retention}/`。
CP 日志为 `build/gpgpu-gpuvm-cp-regression.log`；旧队列日志为
`build/gpgpu-stage3-legacy-queue.log`，四种同步组合为
`build/gpgpu-stage3-sync-{0,1}-{none,smmuv3}-{test,stack}.log`。
结果记录同时检查客体退出码、逐组 PASS 和内核日志，不能仅凭命令退出成功判断。

> **[SOURCE] 可选源码索引**
>
> 版本为固定实现提交 `3600ff795dde9069a6e4f1405f6950a61ea75faa`；验收文件散列另见本地 `gpgpu-stage3-source.json`：
> - 页表、权限和失效：[hw/gpgpu/gpgpu_vm.c](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/hw/gpgpu/gpgpu_vm.c#L31)，[第 31 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/hw/gpgpu/gpgpu_vm.c#L31)、[第 63 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/hw/gpgpu/gpgpu_vm.c#L63)、[第 176 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/hw/gpgpu/gpgpu_vm.c#L176)。
> - GEM 与映射寿命：[linux/gpgpu_gpuvm.c](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L40)，[第 40 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L40)、[第 123 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L123)、[第 304 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L304)、[第 346 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L346)、[第 377 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_gpuvm.c#L377)。
> - 未知管理结果判定：[linux/gpgpu_queue.c](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_queue.c#L110)，[第 110～118 行](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/linux/gpgpu_queue.c#L110-L118)；已发布请求失败沿原保留路径处理。
> - 用户可复现用例：[tests/test-gpuvm.c](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/tests/test-gpuvm.c#L44)，[第 44 行起](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/software/gpgpu/tests/test-gpuvm.c#L44)；设备注入见 [tests/qtest/gpgpu-gpuvm-test.inc，第 1 行起](https://github.com/hxfei-git/gpu-study-qemu/blob/3600ff795dde9069a6e4f1405f6950a61ea75faa/tests/qtest/gpgpu-gpuvm-test.inc#L1)。

### 6.2 第二阶段验证状态（历史）

第二阶段以 `57543dd54b6b046b888277272fa84af81746b1ac` 为接入前基线。队列模式使用双进程设备、`IOMMU=none、VMID=0`，Host 经 HSAKMT 风格库申请对象并建 Queue，RV64 CP 处理管理命令和 AQL，RV32 执行器运行 Kernel。当时 Host 参数基址完整保存为 64 位，计算线程的 `a0` 仍指向 32 位参数窗口。

```text
执行器终态与确认
  → Signal=0 → 通知槽=事件编号 → IH 记录
  → AQL Header=INVALID → 新 R
  → Host 复查结果，按对象寿命卸载和释放
```

当时的代码落点如下，路径和模式不作为现行接口：

- `include/gpgpu_queue.h` 集中布局、寄存器与错误编号；固件的 `start.S/link.ld` 固定末尾 4 MiB，`memory.c` 保存访问上下文，`management.c` 管理 MQD 绑定与请求配对，`aql.c` 处理派发和收尾。
- 设备接入 HQD、8 字节 Doorbell、IH、Host 参数窗口与分段执行；IH 使用第四个 MSI-X 向量，原同步路径继续保留。
- `linux/gpgpu_pci_main.c` 与内存、队列、事件实现共同生成 `gpgpu_pci.ko`，`queue_mode=0/1` 选择互斥路径。设备统一管理显存，用户 Doorbell 按进程独占页面；不能确认访问结束的上下文继续保留。
- `hsakmt/` 提供设备、内存、队列和事件接口。`tests/queue_helpers.c` 实现取号、Header 发布、Doorbell 与默认等待，`tests/test-queue.c` 覆盖连续计算、并发、等待、错误和对象寿命。
- 内核材料保存在 `linux/kernel/`，入口为 `scripts/kernel-build.py`。当时精简配置将模块项从 892 减到 78，原 1,286 个内置配置项保留；配置可逐字节重现，启动、PCI、IOMMU、9p、ext4 等依赖另经核对。原生构建出现跨 CPU 同步等待后，按确认方案改用宿主机交叉构建。
- 目标内核为 `6.18.57-0-gpgpu-virt`，内置 `GPU_BUDDY` 与 `FW_LOADER`。交叉工具链固定为 Arm GNU Toolchain `15.2.Rel1`（GCC `15.2.1`），当时以 16 任务输出到 `build/gpgpu-kernel-cross/`。交叉配置仅两项编译器身份字段不同，功能选项一致；必要工具与 GCC 插件导入 ARM64 客体后原生重编，配置、生成头和 `Module.symvers` 校验不变。独立开发盘为 32 GiB，客体使用 4 个虚拟 CPU、4 GiB 内存；原生构建和发行版环境保留。

2026-10-19，整改前提交 `3217651` 中的 RV64 固件与 Host 接口库已通过编译，汇编检查
4/4。CP QTest 为 16/16，包含原 8 项、设备边界检查和真实队列固件流程；
新增的 A57 与 RV64 CP 双向 64 位并发访问检查运行 2000 轮，高低半部同时
交替变化。单／双进程 SMMUv3 专项各 2/2，ARM64／RISC-V 设备检查各 21/21。
CP 日志为 `build/gpgpu-cp-qtest.log`，数量检查同时核对测试计划、PASS 数量
及没有跳过项。

交叉内核 `6.18.57-0-gpgpu-virt` 已启动，开发文件已完成 ARM64 工具转换，
本机插件加载已通过验证，配置、生成头和 `Module.symvers` 在转换前后保持
不变。驱动及用户程序已在新内核客体中原生编译。`REMOTE=1、IOMMU=none` 的队列验收通过
5/5，设备卸载通过 1/1；结果完成时间为 `2026-10-19T03:07:10Z`。

同一新内核还完成单／双进程与 `none/smmuv3` 四种同步组合。每种组合均通过
驱动检查 5/5、三个示例 3/3（共 17,953 个输出值）、运行时检查 68/68 和
卸载重载 2/2；八次驱动／软件栈运行全部通过，没有内核异常或非预期 SMMU
故障。队列及同步测试共同使用 `build/gpgpu-kernel-cross/` 的内核与模块。

本轮修复了 Alpine Live ISO 遗留的悬空 `/lib/firmware` 链接。非法参数用例
也已改用客体 RAM 和 PCI 窗口之外的物理地址，避免误用 1 GiB 客体中的合法
RAM。等待用例在提交前确认主线程和两个等待线程均阻塞在事件 ioctl，寿命
用例分别验证旧映射与旧事件阻止入口复用。

> **[SOURCE]** 2026-10-19 实际运行产物：[队列结果](../../build/gpgpu-kernel-cross/remote/none/queue-test.json)、[队列检查与卸载日志](../../build/gpgpu-kernel-cross/remote/none/queue-test.log)、[四种同步组合的八次运行汇总](../../build/gpgpu-kernel-cross/stage1-validation.json)及[内核构建清单](../../build/gpgpu-kernel-cross/kernel-build.json)。这些链接指向本地 `build/` 产物，未纳入 Git；复现构建和测试后生成。
>
> 整改后提交 `6325a87` 中的等待与寿命检查见 [tests/test-queue.c](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L497) 的 [第 497～539 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L497-L539)、[第 817～863 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L817-L863)，非法参数地址见该文件的 [第 609～616 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L609-L616)。固件目录修复见 [linux/guest-queue-test.sh](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/guest-queue-test.sh#L8) 的 [第 8～14 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/guest-queue-test.sh#L8-L14)。

<a id="stage-two-review"></a>

2026-10-19 规格复核发现：`6325a87` 的驱动读到匹配的管理完成标记后，未检查 `status` 是否属于已定义的 `0～6`。若仍为初值 `0xffff_ffff` 或其他未知值，创建失败分支可能释放 MQD 及关联引用，而设备访问尚未确认结束。当时仅重编用户态、检查汇编器，后续风格整改补验了固件编译，没有重跑完整客体验收；此前通过数仍只代表其原始记录版本。

> **[SOURCE]** 修复前固定提交 `6325a87e4c500f4930c4e0b502a1ec241f83bf26`：[管理结果读取，第 110～129 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/gpgpu_queue.c#L110-L129)、[创建失败回收，第 622～634 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/gpgpu_queue.c#L622-L634)。

该缺口在第三阶段固定提交 `3600ff795dde9069a6e4f1405f6950a61ea75faa` 中修复：未知结果返回协议错误，已发布请求及相关资源继续保留。三个注入场景的实际结果和修复源码见[第 6.1 节](#stage-three-validation)。这段记录替代原大纲中的重复复核摘要。

同日完成 `hw/gpgpu/` 和 `software/` 的中文注释与 Linux 风格整改，
原实现提交 `3217651` 重写为 `6325a87`。本次用户态与 RV64 固件编译通过，
汇编器检查 4/4；40 个 C/头文件的规范化语法树一致，5 个库目标文件去除
调试信息后逐字节一致，CP 固件与 4 个计算 Kernel 二进制一致。
格式配置、kernel-doc、Python 和 Shell 语法检查通过；Linux checkpatch 为
0 错误、25 提示，主要涉及 MMIO 的 volatile、声明宏及既有深层分支。
本次未重跑完整 QEMU 与 Linux 客体验收，上述端到端数量仍属整改前记录。

下节的 2026-10-08 记录属于第一阶段历史结果。

### 6.3 第一阶段验证记录（历史）

第一阶段先以 `b34b175` 拆分设备的寄存器路由、中断、DMA、派发和 Doorbell，再按顺序接入软件栈 ABI（`aa04ba5`）、设备软件接口（`1d60077`）、Linux PCI 驱动（`c844be7`）、运行时与编程模型（`642880f`）、客体验证（`7fcd7a1`），最后补齐成果文档。

```text
ARM64 Linux 应用
  → libgpgpu：显存分配、DMA 拷贝、Kernel 装载与同步派发
  → /dev/gpgpu0：ioctl 与 mmap DMA 暂存区
  → Linux PCI 驱动：BAR0 控制、BAR2 显存、BAR4 保留门铃
  → QEMU GPGPU：PCI DMA、MSI-X、RV32 解释器与 VRAM
```

当时只允许一个打开会话，计算和 DMA 请求同步完成，应用使用 32 位 VRAM 偏移，不能把 ARM64 指针交给 Kernel。驱动管理显存并保留首页，`mmap` 仅映射会话的 64 KiB 一致性 DMA 暂存区，大拷贝由运行时分块提交。`a0` 保存参数块的 VRAM 偏移；线程坐标通过 `0x8000_0000` 控制窗口读取，`ebreak` 结束线程。每组最多 1024 个线程、最多 4096 组、总计最多 65536 个线程，设备另设 warp 执行预算；共享存储与组内屏障尚未实现。

2026-10-07 首次验收中，三个示例的 17,953 个输出正确，MSI-X 增量为 kernel=3、DMA=21、error=0，驱动与运行时检查 68/68，卸载重载 2/2；RISC-V 和 ARM64 的 GPGPU QTest 各 21/21，其中评分测试 17/17。以下保留次日系统 IOMMU 接入后的完整回归记录。

2026-10-08 在本仓库 `main` 分支运行。ARM64 Alpine 客体内核为
`6.18.55-0-virt`，QEMU 使用 TCG。保存的内核配置中
`CONFIG_ARM_SMMU_V3`、`CONFIG_IOMMU_SUPPORT`、`CONFIG_IOMMU_DMA`
均为 `y`。本次云环境缺少旧构建产物，首次自举使用
`python /workspace/.gpu-study-env/module-test-cloud.py prepare`，通过云环境
已有代理安装软件包，并保留 TLS、包签名与 ISO 校验。后续实际回归命令为：

```sh
for iommu in none smmuv3; do
    make -C software/gpgpu/linux test IOMMU=$iommu
    make -f Makefile.camp test-gpgpu-stack IOMMU=$iommu
done
make -f Makefile.camp test-gpgpu-iommu JOBS=4
```

两种模式分别通过三个 demo 的 17,953 个输出：vector add 17,003 个，
矩阵乘 437 个，ReLU 513 个；每种模式执行三个 demo 的 MSI-X 总增量
均为 kernel=3、DMA=21、error=0。两种模式各自的驱动与运行时检查为 68/68，
包括 `64 KiB + 37` 字节分块搬运、并发 DMA、三维索引、非法参数和坏
kernel 后恢复；装卸载为 5/5，软件栈末尾卸载重载为 2/2。
四条 Linux 回归命令及客体检查的退出码均为 0，未发现 Oops、BUG、panic
或非预期 SMMU fault。没有少跑或跳过的检查。

按 `1234:1337` 找到的设备，在装卸载测试中为 `0000:00:01.0`，
软件栈测试中为 `0000:00:02.0`。SMMUv3 模式对应 group 1 和 group 2，
类型均为 `DMA`；`none` 模式均无 IOMMU group。两个模式共用同一内核、
模块和磁盘：环境与模块清单散列、磁盘 inode 和大小均未变化，切换到
SMMUv3 时显示 `Module is already up to date.`。命令、退出码和复用记录
见 `build/gpgpu-linux-validation.json`，两种模式的 JSON 结果还记录了
模块、demo 和测试二进制的 SHA-256。

ARM64 和 RISC-V 原有 GPGPU QTest 各 21/21，新增专项 2/2，均无跳过，
退出码为 0。日志为 `build/gpgpu-device-aarch64-qtest.log`、
`build/gpgpu-device-riscv64-qtest.log` 和 `build/gpgpu-iommu-qtest.log`。
专项用例固定映射 IOVA `0x80_8060_4567` 到客体物理地址 `0x4ecb_a567`，
通过 BAR0 发起 H2D → D2H 并逐字节比较。非法 D2H 验证 DMA 错误、错误
MSI-X 和无完成通知；只清错误后使用原映射成功搬运。失败写入被 QEMU
拆分为多次访问，事件队列中的 18 条记录均为目标未映射范围的翻译 fault，
恢复阶段无新增 fault。这些是专项用例预期的故障。

复用的原有 SMMUv3 QTest 为 3/3，汇编器检查为 4/4。生产设备、驱动、
运行时与 ABI 均未修改。本轮未增加跨页布局、只读权限、撤销映射或重新
映射用例；GPU 内部地址隔离仍未实现。

### 6.4 CP 接入与统一访存重构（历史）

2026-10-11 接入双进程设备与原生 RV32 CP，完成复位、固件装载、启动和同地址替换固件。2026-10-14 接入 CP Host 访存窗口，CP 以普通读写指令经 IOVA 访问 Host RAM；2026-10-17 将原生 CP 迁到 RV64，增加对齐的 8 字节读写，RV32 计算执行器保持独立。详细调用与当时回归见[窗口实现记录](<../../docs/历史/CP_MEC 软件实现、参考与扩展.md#cp-host-window-implementation>)。

2026-10-28 的统一访存重构采用连续 VRAM／Outbound、单一 DRM/GEM 入口、GPUVM 开关与 CSR 启动信息，删除同步路径、旧参数窗口和三模式接口。当时协议版本为 2；2026-10-29 再重构共享定义并升级为版本 3，差异见 [PROTOCOL_V3.md](./PROTOCOL_V3.md)。不再用“统一访存版本 2”指称当前阶段。

版本 2 的历史核心计算记录为：GPUVM 开启于 `2026-10-28T14:34:47Z` 完成，关闭于 `2026-10-28T14:35:12Z` 完成。两种状态均通过向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513，每种比较 17,953 个结果，退出码为 0；驱动和示例散列一致，未发现客体内核异常。当轮没有运行 SMMUv3、多进程或资源生命周期专项验收。

> **[SOURCE]** 版本 2 的文字记录保存在固定提交 `c9f85990d5ae41b7097201cbe03252ef087ba4c6` 的 [README 第 234～243 行](https://github.com/hxfei-git/gpu-study-qemu/blob/c9f85990d5ae41b7097201cbe03252ef087ba4c6/software/gpgpu/README.md#L234-L243)。原 `core/on/`、`core/off/` 日志未入库且已被版本 3 运行覆盖；现存同路径文件只能证明最新运行，不能重新作为版本 2 的独立证据。

版本 3 的核心计算记录对应代码版本 `b8bad67`：QTest 11/11，教学汇编器 7/7，基础环境引导回归 6/6；Linux 全栈在 GPUVM 开、关两种状态都完成向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513，客体退出码为 0。两种模式的组件 SHA-256 相同：固件 `6d9251e744f6924f5b8859b95bd316033bfb99a0bf3194bf59d0ba0b88243acb`，模块 `ad0591f863710f27a7e92c4468227d1be8e9b795ff9a799cb0d087f02b54fca7`，示例 `828c05a5db9e6cc21bb13e57a2b92abf0b94b0723d03b4c66acd8c98905bc987`。升级到版本 4 之前，在 `087ea56` 上重跑的 QTest、汇编器和全栈结果与此一致，记录在版本 4 第一个提交的说明中。版本 3 到 4 的差异见 [PROTOCOL_V4.md](./PROTOCOL_V4.md)。

> **[SOURCE]** 版本 3 的本地运行记录移到 `build/gpgpu-validation/v3/`：[命令与结果汇总](../../build/gpgpu-validation/v3/runtime-verification.json)、[QTest 输出](../../build/gpgpu-validation/v3/qtest-build.log)、[全栈输出](../../build/gpgpu-validation/v3/stack-validation.log)、[两种模式汇总](../../build/gpgpu-validation/v3/core/summary.json)。
