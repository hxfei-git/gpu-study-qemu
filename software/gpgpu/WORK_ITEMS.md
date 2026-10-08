# 进阶实验一工作项

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真工具 |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU | Graphics Processing Unit | 图形处理器 |
| ARM64 | Arm 64-bit Architecture | Arm 64 位架构 |
| RV32 / RISC-V | 32-bit RISC-V / Reduced Instruction Set Computer V | 本实验的 32 位精简指令集 |
| API / ABI | Application Programming Interface / Application Binary Interface | 应用编程／二进制接口 |
| PCI / BAR | Peripheral Component Interconnect / Base Address Register | 外设互连／基址寄存器 |
| DMA | Direct Memory Access | 直接内存访问 |
| IOMMU | Input/Output Memory Management Unit | 输入输出内存管理单元 |
| SMMUv3 | System Memory Management Unit version 3 | Arm 系统内存管理单元第三版 |
| IOVA | I/O Virtual Address | 设备 DMA 虚拟地址 |
| RAM / VRAM | Random Access Memory / Video RAM | 系统内存／显存 |
| H2D / D2H | Host to Device / Device to Host | 主机到设备／设备到主机 |
| MSI-X / IRQ | Message Signaled Interrupts Extended / Interrupt Request | 扩展消息中断／中断请求 |
| FP32 | Floating Point 32-bit | 32 位浮点格式 |
| CP | Command Processor | 命令处理器 |
| CSR | Control and Status Register | 控制与状态寄存器 |

本轮六项已完成，按下列顺序单独提交。提交正文说明做了什么、核心思想、
完成方法和验证结果。运行命令、架构与能力覆盖见 [README.md](README.md)。

设备结构重构提交 `b34b175` 放在软件栈 ABI 提交之前：拆分寄存器路由、
中断、DMA、派发和门铃模块，用枚举说明状态取值。后续六项按原顺序重放，
下面使用当前改写后的提交号。

1. **软件栈设计与 ABI（aa04ba5）**：规定运行时、驱动和设备的调用关系，固定 ioctl
   数据结构、VRAM 地址语义和 RV32 kernel 入口。
2. **设备的软件接口（1d60077）**：补齐参数寄存器、RV32 数据读取与分支、真实 DMA
   和 MSI-X 通知；保留基础测题的行为。
3. **Linux PCI 驱动（c844be7）**：注册 PCI 与字符设备，管理 BAR 和 MSI-X，以可
   mmap 的一致性 DMA 缓冲区提交搬运描述符，并管理 VRAM 分配与同步派发。
4. **运行时与编程模型（642880f）**：实现 libgpgpu API，提供 RV32 汇编头文件、kernel
   构建方式和三个 FP32 demo；应用逐元素核对设备返回的数据。
5. **客体端到端验证（7fcd7a1）**：复用 ARM64 Linux 环境，自动构建和执行驱动、运行时、
   demo 与错误路径测试，核对 MSI-X 计数并检查基础 QTest 的实际通过数。
6. **第一阶段成果文档（本文所在提交）**：记录架构、数据路径、运行命令、验证结果
   和能力边界，同步父目录的四阶段大纲与仓库 AGENTS.md。父目录大纲位于
   Git 仓库外；仓库内 README 与本文保存可随代码版本核对的成果记录。

2026-10-07 验收：三个 demo 的 17,953 个输出全部正确，MSI-X 增量为
kernel=3、DMA=21、error=0；驱动与运行时检查 68/68，卸载重载 2/2，
RISC-V 和 ARM64 的 GPGPU QTest 均为 21/21（其中评分测试 17/17）。

## 固定的接口与范围

```text
ARM64 Linux 应用
  → libgpgpu：VRAM 分配、DMA 拷贝、kernel 装载与同步派发
  → /dev/gpgpu0：ioctl 请求 + mmap DMA staging buffer
  → Linux PCI 驱动：BAR0 控制、BAR2 显存资源、BAR4 保留门铃
  → QEMU GPGPU：PCI DMA、MSI-X、RV32 解释器、VRAM
```

应用使用 32 位 VRAM 字节偏移，不能把 ARM64 指针传给 kernel。驱动保留
VRAM 的首个页，管理其余空间，并检查每个请求是否落在当前会话的分配中。
驱动允许一个打开的会话，所有 DMA 与计算请求同步完成；成功返回后应用可以
使用结果。mmap 只映射会话的 64 KiB 一致性 DMA 缓冲区，BAR 和 DMA 地址由
驱动管理。大拷贝由运行时分块提交。

kernel 入口约定为 `__global__ void kernel(const Args *args)` 的汇编实现：
`a0` 保存 Args 的 VRAM 偏移，参数由调用方按小端 32 位字段打包；无需栈或
C 函数调用，以 `ebreak` 结束线程。threadIdx、blockIdx、blockDim 和 gridDim
通过 GPU 侧 `0x80000000` 控制窗口读取。grid 和 block 支持三个维度，X 为
最快变化维。基础 CSR mhartid 编码继续保留。

本阶段以独立线程计算 FP32 vector add、矩阵乘和 ReLU。共享内存与 block
屏障尚无设备实现，非零 shared_size 返回错误。驱动限制每次派发最多 4096
个 block、每个 block 最多 1024 个线程、总计最多 65536 个线程；设备另有
每个 warp 的执行周期上限，防止非法 kernel 长时间占用 QEMU。

基础 17 道测题涵盖的是已有教学设备能力，README 已说明这些能力如何进入
软件栈。DMA 与中断链路另经真实 Linux 客体数据搬运和完成计数核验。
RV32 kernel 可访问设备 VRAM，第一阶段的独占会话不提供多进程地址隔离。

## 系统 IOMMU：ARM64 virt + SMMUv3

本轮沿用 PCI DMA、Linux DMA API、运行时和 ABI。`IOMMU=none|smmuv3`
及 `--iommu none|smmuv3` 控制驱动测试、软件栈测试和交互运行，默认 `none`。
自举和编译环境共用原磁盘与配置，模式切换不重建环境。

系统内存端填写 DMA 地址，启用翻译时为 IOVA；SMMUv3 将其翻译为客体物理
地址。显存端、应用设备指针和 RV32 kernel 参数仍是 VRAM 偏移。SMMUv3
客体验证按 `1234:1337` 找到设备，并要求其 IOMMU domain 类型为 `DMA`。

新增 `test-gpgpu-iommu` 只运行两个用例：固定非恒等映射的 H2D → D2H，
以及未映射 D2H 的错误通知、无完成通知和原映射恢复。页表与寄存器设置
复用现有 SMMUv3 辅助函数；大块搬运继续复用 Linux 软件栈测试。
本轮不扩展跨页、只读权限、撤销／重映射测试，也不加入故障注入 ioctl。
GPU 内部地址隔离仍留待后续阶段。运行方式与本次结果见 README。

2026-10-08 在 `main` 实测：`none` 与 `smmuv3` 各通过三个示例的
17,953 个输出、68/68 运行时检查、5/5 装卸载及 2/2 卸载重载。
SMMUv3 模式下 GPGPU domain 类型为 `DMA`，无非预期 SMMU fault。
两种模式的环境、模块清单散列和磁盘 inode／大小一致，切换未触发环境
或模块重建。ARM64、RISC-V 原有 GPGPU QTest 各 21/21，专项 2/2，
无跳过；上述命令退出码均为 0。日志按模式保存在
`build/gpgpu-linux-module/{none,smmuv3}/`，宿主机命令与退出码记录在
`build/gpgpu-linux-validation.json`，专项日志为
`build/gpgpu-iommu-qtest.log`。
