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
| CP | Command Processor | 命令处理器 |
| ACK | Acknowledgment | 确认；本版用于确认已经读取执行结果 |
| TCG | Tiny Code Generator | QEMU 动态翻译后端 |
| TLS | Transport Layer Security | 传输层安全协议 |
| JSON | JavaScript Object Notation | JavaScript 对象表示法 |
| ISO | International Organization for Standardization | 此处指 ISO 9660 光盘镜像 |
| SHA-256 | Secure Hash Algorithm 256-bit | 256 位安全散列算法 |
| BDF | Bus, Device, Function | PCI 总线、设备、功能号 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| ROCr | ROCm Runtime | ROCm 的异构计算运行时 |
| HSAKMT | HSA Kernel Mode Thunk | HSA 内核接口封装层；此处仅借鉴接口风格 |
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
| PTE / PDE | Page Table Entry / Page Directory Entry | 页表叶子项／页目录项 |
| TTM | Translation Table Maps | 图形对象存储位置与迁移管理框架 |
| VMA | Virtual Memory Area | CPU 进程的虚拟内存映射区域 |
| CSR | Control and Status Register | 控制与状态寄存器；本实验用于读取启动信息 |
| SGPR / VGPR | Scalar / Vector General-Purpose Register | 标量／向量通用寄存器；本版仅保存独立启动状态 |

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

**[DESIGN]** GPU 物理空间连续覆盖 32 位地址：`[0, vram_size)` 属于 VRAM，`[vram_size, 0x1_0000_0000)` 属于 Outbound。以 64 MiB VRAM 为例，Outbound 从 `0x0400_0000` 开始，长度为 `0xfc00_0000`。Host IOVA `0x4100_0000` 对应 GPU 物理地址 `0x4500_0000`；IOVA 0 对应 `0x0400_0000`，允许正常使用。

驱动在启动 CP 前写入 BAR0 `0x0b00` 的窗口配置并读取确认状态。窗口由整个设备共用，队列运行期间不重配。DMA 分配统一采用 31 位 mask，保留原始 DMA 地址供 DMA API 释放和映射，转换后的 GPU 物理地址写入设备协议。Host BO 仍逐对象使用 `dma_alloc_coherent()`。

Outbound 只决定访问路由。Host 存储或 IOMMU 映射不存在时访问仍会失败。SMMUv3 功能保留；开启后，PCIe DMA 再由系统 IOMMU 将 IOVA 翻译成 Host 物理地址。

<a id="stage-three-gpuvm"></a>

## 2. GPUVM、对象绑定与资源回收

GPUVM 开启时使用三级 4 KiB 页表、两个用户 VMID 和同步 TLB 失效。PTE 只保存 GPU 物理页地址与有效、读、写、执行权限，Host 页指向 `vram_size + IOVA`。根目录和中间表页也使用 GPU 物理地址，遍历通过公共物理入口读取，不再次进入 GPUVM 翻译。

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

`a0` 直接保存 `kernarg_address`：开启 GPUVM 时为 GPUVA，关闭时为 GPU 物理地址。参数读取按普通内存处理。16 字节 Kernel 描述符在偏移 `0x0c` 保存 `kernel_abi_version=2`，旧值 0 和其他版本被拒绝；AQL 包仍为 64 字节。设备／固件队列协议、DRM 和 GPUVM 编码均为版本 2，组件必须一起更新。

## 4. 公共库与三个计算示例

`GpgpuKmtMemory` 提供 CPU 映射 `cpu` 和唯一的设备地址 `gpu_address`，应用不再选择 Host 地址、VRAM 偏移或 GPUVA。`gpgpuKmtAllocMemoryEx()` 先创建整对象再绑定；代码指定读与执行权限，普通数据指定读写权限。参数中写入 32 位地址前须检查范围。

库中的 `gpgpuKmtReserve()`、`gpgpuKmtPublish()`、`gpgpuKmtWaitSignal()` 和 `gpgpuKmtDrainQueue()` 分别负责槽位预留、Header 与 Doorbell 发布、Signal 与事件等待、排空。默认等待先主动检查约 200 μs，再进入事件等待，唤醒后复查 Signal。

`gpgpu-demo` 链接 `libgpgpu-kmt.a`，固定将代码、描述符和计算数组放在 VRAM；参数、Ring、进度和 Signal 放在 Host BO，经 Outbound 访问。用于 COPY 的 Host BO 也在 Queue 创建前准备。三个例子保持原输入和比较规则：

- 向量加法比较 17,003 个结果，Grid 为 266×1×1，Block 为 64×1×1。
- 矩阵乘法计算 19×17 · 17×23，比较 437 个结果，Grid 为 3×3×1，Block 为 8×8×1。
- ReLU 比较 513 个结果，Grid 为 6×1×1，Block 为 96×1×1，并保留负零结果检查。

任一数值比较、提交、等待或正常资源回收失败，程序返回非零退出码。

## 5. 构建、运行与本次结果

已有开发环境保存在 `build/gpgpu-kernel-gpuvm/`，内核为 `6.18.57-0-gpgpu-virt`，包含 DRM/GEM 与 `GPU_BUDDY`。缺少该环境时先运行内核构建脚本；它使用仓库中的固定源码、配置与工具链清单。

```sh
# 构建用户库、示例和三个 RV32 Kernel
make -C software/gpgpu
# 构建 RV64 CP 固件
make -C software/gpgpu/firmware
# 需要准备内核环境时使用既有构建工具
python3 software/gpgpu/scripts/kernel-build.py --help
# 唯一核心计算验证入口：自动构建并顺序运行 GPUVM 开和关
make -f Makefile.camp test-gpgpu-stack
```

核心计算固定使用 ARM virt、A57、1 GiB RAM、IOMMU=none 与双进程设备。两种状态的日志分别保存在 `build/gpgpu-kernel-gpuvm/core/on/` 和 `core/off/`：`compute.log` 保存逐元素比较结果，`console.log` 保存客体日志，`result.json` 保存实际结果与组件散列。没有新增 CSR、Outbound 或生命周期专项验收，也不运行旧 QTest、旧 Runtime 或历史组合回归。

2026-10-28 已完成版本 2 的全栈构建与核心计算。GPUVM 开启和关闭均通过向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513；每种状态比较 17,953 个结果，客体退出码均为 0。两次运行的驱动与示例散列一致，客体日志未出现内核异常。

> **[SOURCE]** 本次工作区版本 2 的实际产物与运行记录：GPUVM 开启于 `2026-10-28T14:34:47Z` 完成，关闭于 `2026-10-28T14:35:12Z` 完成。各记录包含内核版本、驱动、固件和示例 SHA-256，可核对本次运行产物：
>
> - 开启：[计算日志](../../build/gpgpu-kernel-gpuvm/core/on/compute.log)、[结果与散列](../../build/gpgpu-kernel-gpuvm/core/on/result.json)。
> - 关闭：[计算日志](../../build/gpgpu-kernel-gpuvm/core/off/compute.log)、[结果与散列](../../build/gpgpu-kernel-gpuvm/core/off/result.json)。

这些本地 `build/` 产物不纳入 Git。本轮未运行 SMMUv3、多进程、资源生命周期或其他专项验收，历史测试数量不用于证明版本 2。

## 6. 历史验收记录

以下记录属于当时的固定提交与旧协议。旧测试源文件、运行入口和兼容实现已从当前代码删除；这些数量、命令和日志只用于查阅历史，不构成当前验收要求。

<a id="stage-three-validation"></a>

### 6.1 第三阶段验收与代码快照（历史）

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
> 整改后提交 `6325a87` 中的等待与寿命检查见 [tests/test-queue.c](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L497) 的 [第 497～539 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L497)、[第 817～863 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L817)，非法参数地址见该文件的 [第 609～616 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/test-queue.c#L609)。固件目录修复见 [linux/guest-queue-test.sh](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/guest-queue-test.sh#L8) 的 [第 8～14 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/guest-queue-test.sh#L8)。

同日规格复核发现管理完成后的未知结果码未校验，创建失败分支可能提前释放
MQD 及相关引用，尚待修复。上述测试数量是已提交的历史运行记录；复核环境
仅完成用户态编译和汇编检查，未重跑完整客体验收。证据与复核范围见
[大纲复核记录](<../../../QEMU GPGPU 实验大纲.md#stage-two-spec-review>)；
寄存器与协议查阅见 [GPGPU 硬件手册](<../../../QEMU_2026_GPGPU_硬件手册.html>)。

同日完成 `hw/gpgpu/` 和 `software/` 的中文注释与 Linux 风格整改，
原实现提交 `3217651` 重写为 `6325a87`。本次用户态与 RV64 固件编译通过，
汇编器检查 4/4；40 个 C/头文件的规范化语法树一致，5 个库目标文件去除
调试信息后逐字节一致，CP 固件与 4 个计算 Kernel 二进制一致。
格式配置、kernel-doc、Python 和 Shell 语法检查通过；Linux checkpatch 为
0 错误、25 提示，主要涉及 MMIO 的 volatile、声明宏及既有深层分支。
本次未重跑完整 QEMU 与 Linux 客体验收，上述端到端数量仍属整改前记录。

下节的 2026-10-08 记录属于第一阶段历史结果。

### 6.3 第一阶段验证记录（历史）

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
