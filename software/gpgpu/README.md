<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->
# QEMU GPGPU 软件栈：同步调用、AQL 与 GPUVM

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
| VMID | Virtual Memory Identifier | 虚拟地址空间编号；第二阶段固定为 0 |
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

前三阶段的软件栈都放在 `software/gpgpu/`。第三阶段入口见[GPUVM 对象与使用流程](#stage-three-gpuvm)。同步模式下，
ARM64 Linux 应用通过 libgpgpu 提交数据和 Kernel，QEMU 中的 GPGPU 执行
RV32 代码，再把结果传回应用。
Host 在本文中指 Linux 客体里的应用；QEMU 进程运行在物理宿主机上。

## 架构与数据路径

下图描述 `queue_mode=0` 的第一阶段同步调用。队列模式见下一节。

```text
Linux 客体
  demos/gpgpu-demo.c / tests/test-runtime.c
                  │ gpgpuMalloc / gpgpuMemcpy / gpgpuLaunchKernel
                  ▼
  runtime/gpgpu.c：libgpgpu
                  │ ioctl + mmap
                  ▼
  linux/gpgpu_pci_main.c：/dev/gpgpu0
                  │ BAR0 描述符与派发寄存器
                  │ PCI DMA：客体 RAM ↔ VRAM
                  ▼
QEMU（gpgpu.c 注册 PCI 设备和 BAR）
  hw/gpgpu/gpgpu_mmio.c：BAR0 寄存器路由
                  ├─ memory/gpgpu_dma.c：客体 RAM ↔ VRAM
                  └─ compute/gpgpu_dispatch.c：检查并保存派发参数
                  ▼
  compute/gpgpu_core.c：推进工作组、warp 和执行预算
                  → gpgpu_isa.c：逐 lane 解释 RV32 指令
                  → gpgpu_memory.c：取指、读取参数和访问数据
                  │ 通过 memory/ 的接口访问存储
                  ▼
  kernel 完成 → interrupt/gpgpu_irq.c：MSI-X → 驱动 completion → ioctl 返回
```

双进程设备中的原生 CP 使用 `qemu-system-riscv64` 执行 RV64 裸机固件；
应用 Kernel 继续由 `compute/` 下的 RV32 计算执行器运行，计算线程的
寄存器和 `a0` 仍为 32 位。同步路径把参数上传到 VRAM，队列路径则把完整的
64 位 Host 参数地址交给执行器，计算线程用 `a0 = 0x9000_0000` 读取只读
参数窗口。窗口检查对齐的 4 字节读取及本次参数范围。

工作区已实现 HQD、Doorbell、IH、CP 队列固件、执行器分段执行和 Host 参数
读取。固件与设备联合 QTest、重建内核上的 Linux 五组队列验收和四种同步
组合回归均已通过，结果见下文。下述同步模式的独占会话、暂存区和超时复位
继续用于第一阶段回归。

阅读设备代码时，先从 `gpgpu_mmio.c` 找到寄存器所属模块，再沿该模块的
读写函数追踪操作。`gpgpu_regs.h` 保存寄存器偏移和枚举，`gpgpu.h` 声明
设备类型与设备级接口；完整设备状态由 `gpgpu_internal.h` 组合，各模块在
自己的头文件中声明接口。`dma.status` 使用 `GPGPUDMAStatus` 枚举，取值为
IDLE、BUSY、COMPLETE、ERROR；中断和全局状态使用可组合的枚举位标志。
BAR4 的 8 字节 Doorbell 回调位于 `cp/gpgpu_doorbell.c`。

在同步模式中，驱动匹配 PCI `1234:1337`，申请 BAR0、BAR2 和 BAR4。
BAR0 用于控制、DMA 和 Kernel 派发，BAR2 表示设备显存资源；BAR4 在同步
模式中保留，在队列模式中用于 Doorbell。同步模式申请三个 MSI-X 向量，
分别处理计算完成、DMA 完成和设备错误。请求等待中断完成，超过 5 秒返回
`-ETIMEDOUT` 并复位寄存器状态。

每次打开字符设备建立一个会话。驱动通过 gen_pool 分配 VRAM，以 256 字节
为粒度管理空间；返回值是 32 位 VRAM 字节偏移。首个页保留，地址 0 不分配。
驱动检查 COPY 的地址范围、LAUNCH 的代码与参数范围，以及 FREE 是否使用
分配起始地址。关闭会话回收全部分配；单个设备同时只允许一个会话。
如果直接使用 ioctl/mmap，关闭 fd 后仍存在的映射会延迟会话释放，需完成
munmap 才能重新打开设备。gpgpuDestroy() 已按先解除映射、再关闭 fd 的
顺序处理；调用方应先结束其他线程对该 context 的访问。

驱动为会话分配 64 KiB 一致性 DMA 缓冲区，用户通过 mmap 访问这个缓冲区。
H2D 时运行时先把应用数据复制进缓冲区，驱动将 DMA 地址、VRAM 偏移、长度和
方向写入 BAR0，设备调用 `pci_dma_read()` 写入 VRAM。D2H 时设备通过
`pci_dma_write()` 写回 DMA 缓冲区，运行时再复制到应用输出数组。大于
64 KiB 的拷贝由运行时拆成多个描述符。用户无需提供物理地址或操作 BAR。

启用系统 IOMMU 后，描述符的系统内存端仍填写 `dma_alloc_coherent()`
返回的 `staging_dma`，此时它是 IOVA。设备沿 PCI 地址空间交给 SMMUv3
翻译成客体物理地址，再访问暂存区：

```text
应用缓冲区 ↔ mmap 的 64 KiB 暂存区
                        ↑ 系统内存访问
GPGPU 的 PCI DMA → SMMUv3：IOVA → 客体物理地址
        ↕
VRAM：描述符的显存端、应用设备指针和 kernel 参数仍为字节偏移
```

关闭 IOMMU 时沿用直通 DMA 路径。系统翻译与保护不改变 VRAM 寻址，
也不隔离不同 GPU kernel 对显存的访问。

运行时把 kernel 二进制和 Args 分别上传到 VRAM，驱动设置代码地址、参数
地址和三维 grid/block 后派发。每个线程入口的 a0 指向 Args，执行器按设备
侧控制窗口返回 threadIdx 和 blockIdx。线程使用 `ebreak` 结束。DMA 返回
成功时该次传输已完成；Launch 返回成功时所有线程已执行完毕，随后可调用
D2H 获取结果。

## 第二阶段队列路径与接口

当前队列配置为 `REMOTE=1、IOMMU=none、VMID=0`。Host RAM 对象使用完整的
64 位物理字节地址，VRAM 对象使用区内偏移。驱动装载 `gpgpu-cp.bin`，建立
KIQ 和 IH，再启动 CP；CP 经 KIQ 建立 HIQ，由 HIQ 处理用户队列创建与卸载。
全设备最多同时驻留两条用户队列。下面沿一次提交说明对象的使用顺序：

```text
实验调用方
  → gpgpuKmtOpen：取得进程连接
  → gpgpuKmtAllocMemory：申请 Ring、进度、Signal、参数与 VRAM
  → gpgpuKmtCreateQueue：驱动准备 MQD，HIQ 请求 CP 装入空闲 HQD
  → gpgpuKmtCreateEvent：把事件与源队列关联，返回通知槽地址
  → 准备 Signal=1 与参数，原子取号后等待相应 Ring 槽可写
  → 填写 AQL 包体
  → 完整发布 32 位 Header，写 64 位 Doorbell
       ↓
RV64 CP 固件
  → 读取 W 与 Header，解析 Dispatch 和 VRAM 中的 Kernel 描述对象
  → 配置 RV32 执行器，等待整个 Dispatch 结束并确认结果
  → Signal=0 → 通知槽=事件编号 → 提交 IH → Header=INVALID → 回报新 R
       ↓
Host 调用方
  → 约 200 μs 主动检查后按需阻塞，唤醒后复查 Signal
  → 同步读取结果，停止提交并排空队列
  → 卸载 Queue，结束旧通知和映射引用，再销毁 Event、释放 Memory、关闭连接
```

驱动模块通过 `queue_mode` 选择互斥模式，默认 `0` 保留同步路径，`1` 使用
队列路径。队列模式支持多个进程连接，DMA、IH 与错误状态分别处理；打开、
关闭或等待超时不触发全局复位。成功的内存拷贝接口返回时，该次 DMA 已经
完成；任务发布仍由调用方直接写 Ring，不增加逐 Dispatch ioctl。

`hsakmt/gpgpu_kmt.h` 提供四组接口。`gpgpuKmtOpen/Close/GetInfo` 管理连接
和查询版本；同进程重复打开增加连接引用，最后关闭前须释放存活对象。
`gpgpuKmtAllocMemory/FreeMemory/CopyMemory` 管理内存与已有 DMA 通路，
分别返回软件句柄、CPU 映射、Host 物理地址或 VRAM 偏移。
`gpgpuKmtCreateQueue/QueryQueue/DestroyQueue` 管理队列与 Doorbell 映射。
`gpgpuKmtCreateEvent/QueryEvent/WaitEvent/DestroyEvent` 管理通知和等待；
事件等待返回后保留状态，可唤醒多个等待者。版本不匹配明确拒绝，管理结果、
队列首次错误和接口状态分别保存。

调用方的 AQL 发布和 Signal 等待放在 `tests/queue_helpers.c`。实验固定最多
4 个提交线程，每线程只保留一个未发布编号；共享 W 使用 32 位原子取号，R/W
存放位置各按 8 字节对齐。线程按自己的 P 等待 `(P-R) mod 2^32 < N`，
因此全局 `W-R > N` 仍允许较早编号填包。填包和发布不由全局锁串行保护。
等槽期间若队列故障，提交线程结束等待并保留已预留编号和资源，不回退 W
或填补空洞。
A57 在 Header 发布前、Doorbell 写入前和读到 Signal 完成后使用 `dmb sy`；
CP 对应使用 `fence iorw, iorw`。默认 Signal 等待的约 `200 μs` 预算从本次
等待开始计算，接口库中的事件等待不再重复这一轮主动检查。

正常销毁前，调用方停止提交、完成已开始的发布并等待排空；驱动复核 R/W，
再请求 CP 卸载。确认 MQD 保存、设备访问及旧通知处理结束后，才解除队列
对 Ring、进度对象的引用。Doorbell 由驱动按进程分配独占页面；旧映射或事件
仍在时，入口和 MQD 地址继续保留。外部内存映射未解除也会阻止释放。

创建后 CPU 映射失败且卸载结果未确认时，库不返回可提交的队列标识，但保留
内部引用，使相关内存释放及最后关闭返回暂不可用。等待超时只结束本次等待。
进程异常退出后，驱动保留仍可能被设备访问的上下文和内存；本版通过设备
移除停止访问后清理，不实现运行中复位恢复、取消预留空洞或强制中止。

代码沿功能分目录组织：设备的 HQD、IH 和参数访问分别位于
`hw/gpgpu/cp/gpgpu_hqd.c`、`hw/gpgpu/interrupt/gpgpu_ih.c` 和
`hw/gpgpu/compute/gpgpu_memory.c`；分发和 RV32 执行器同属 `compute/`。
`firmware/` 将启动与异常、Host 访存、管理命令、AQL 收尾
分开。驱动的 PCI 入口改为 `linux/gpgpu_pci_main.c`，与 `gpgpu_memory.c`、
`gpgpu_queue.c`、`gpgpu_event.c` 共同链接成原名 `gpgpu_pci.ko`。
`include/gpgpu_queue.h` 集中定义 MQD、AQL、Signal、IH 和寄存器编码，
编译期核对布局；`hsakmt/` 按设备、内存、队列、事件分文件实现接口库。

> **[SOURCE]** 实验子仓库固定提交 `6325a87e4c500f4930c4e0b502a1ec241f83bf26`：
>
> - 四组接口与创建失败时的引用保留：[software/gpgpu/hsakmt/gpgpu_kmt.h](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/hsakmt/gpgpu_kmt.h#L103)，[第 103～152 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/hsakmt/gpgpu_kmt.h#L103)。
> - 固件固定布局、入口和栈范围：[software/gpgpu/firmware/link.ld](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/firmware/link.ld#L4)，[第 4～16 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/firmware/link.ld#L4)；AQL 执行、确认及完成收尾：[software/gpgpu/firmware/aql.c](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/firmware/aql.c#L72)，[第 72～173 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/firmware/aql.c#L72)。
> - 调用方的原子取号、发布屏障和默认等待：[software/gpgpu/tests/queue_helpers.c](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/queue_helpers.c#L16)，[第 16～171 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/tests/queue_helpers.c#L16)。

以上接口遵循[CP/MEC 第一版规格](<../../../CP_MEC 第一版规格.md>)中的实验
协议，采用 HSAKMT 风格的职责划分，未实现完整 ROCr 或原版 libhsakmt 的 ABI。

<a id="stage-three-gpuvm"></a>

## 第三阶段：GPUVM 对象与使用流程

应用先准备所有对象，再创建一条用户 Queue，反复发布 AQL，结束后统一关闭。
例如向量加法的两个进程都把 X 放在 GPUVA `0x0050_0000`，各自的文件上下文
绑定不同 VMID，页表指向不同存储，因此两个任务得到各自的结果。

```text
应用 A / B → 各自打开 /dev/dri/renderD128
  → GEM handle → 固定 Host coherent / VRAM 存储
  → MAP → 所属 VM 的三级页表 → 同步 TLB 失效
  → 用户 Queue → MQD/HQD 中的 VMID 1 或 2
  → CP：翻译 Ring、进度、描述对象、Signal、通知槽
  → RV32 执行器：按同一 VMID 翻译代码、参数、数组
  → 写回结果 → Signal → 通知槽 → IH → 消费回报
  → 停止提交、排空并卸载 Queue → 结束事件
  → UNMAP、失效确认、关闭 GEM → 解绑并归还 VMID
```

设备、RV64 固件、驱动和库共用 `include/gpgpu_vm.h`。新设备以
`-global gpgpu.gpuvm=on` 启动，驱动使用 `queue_mode=2`，只通过 DRM render
入口开放用户功能。`queue_mode=0/1` 分别保留同步与物理地址队列模式；它们
使用 `/dev/gpgpu0`，设备关闭 GPUVM。模式或版本不匹配时拒绝装载，运行中不切换。

**[BOUNDARY]** 本版沿用 RV64 裸机 CP 和 RV32 计算执行器。GPUVA 为 32 位，
页大小为 4 KiB，三级索引为 2/9/9 位；管理 DMA 使用 VMID 0 直通，两个用户
VMID 固定驻留。每 VM 最多一条 Queue，整个 Queue 寿命内冻结映射。CPU 同步
写表并按 VMID 全量失效；没有 TTM、存储迁移、驱逐、换页、缺页重放或抢占。
这套页表及寄存器是教学设计，不是 MI300X 硬件接口。

### 分配权限、提交与关闭

`gpgpuKmtOpen()` 查询 DRM 能力，随后普通 `gpgpuKmtAllocMemory()` 依次创建
BO、建立 Host CPU 映射、选择 GPUVA 并执行整对象 MAP，全部成功才返回。
对象的 `gpu_va` 交给 AQL、描述对象或 Kernel 参数；Host 的 `cpu` 指针只用于
准备数据，旧字段 `host_address`、`vram_offset` 在 GPUVM 模式保持零。
VRAM 上传、下载仍通过 `gpgpuKmtCopyMemory()` 按对象句柄和逻辑范围检查。

代码使用 `gpgpuKmtAllocMemoryEx()` 指定 `GPGPU_VM_READ | GPGPU_VM_EXEC`；
输入参数和数组可只读，输出、Ring、进度和 Signal 按需要读写。普通申请默认
读写权限。扩展接口的 `fixed_va=0` 仍由库选址；指定地址用于隔离和重映射
验证，不能绕过保留区、重叠、归属或权限检查。新对象的整个实际分配清零，
CPU 和 GPU 不会看到上一个对象留下的内容。

分配范围排除零页、控制页 `0x8000_0000～0x8000_0fff`、参数窗口
`0x9000_0000～0x9000_ffff` 和内部通知页 `0xffff_f000～0xffff_ffff`。
RV32 从控制页读取线程索引，`a0` 仍指向参数窗口；窗口内读取先换成参数
GPUVA，再按任务 VMID 翻译。普通取指和数据访问不走原始 VRAM 旁路。

Queue 创建前应备齐所有对象；事件创建只分配预映射通知页的 512 个槽之一。
创建 Queue 后申请或释放映射返回忙，已有对象内容可按发布与等待协议更新。
代码、参数和完成对象仍通过原 AQL 字段引用，提交不增加逐任务 ioctl。
可运行示例是 `tests/test-gpuvm.c` 中的 `prepare()`、`submit()`、`verify()`。

调用 `gpgpuKmtClose()` 前，应用先停止新提交并完成已预留包的发布。最后一次
关闭会依次排空并销毁 Queue、销毁事件、撤销映射、解除 CPU 映射和关闭句柄；
排空最多等待约 5 秒，超时或状态未知返回错误，相关对象继续保留。库跟踪调用方
提供的内存、队列、事件结构，这些结构必须保持有效，直到显式销毁或最后关闭
成功。关闭期间拒绝其他线程的新接口调用，直接写 Ring 的提交线程须由应用停下。

`gpgpuKmtGetVmInfo()` 返回可分配区间和当前绑定数量；第三个 VM 在首次 MAP
时得到资源不足，不驱逐已有 VM。`gpgpuKmtGetVmFault()` 返回队列首次故障的
VMID、GPUVA、读写取指类型和阶段。故障只停止源队列，不回滚已完成的写入。
应在销毁队列对象前读取诊断；不能确认访问结束时，不以超时替代安全卸载。

> **[SPEC]** [GPUVM 第一版规格](<../../../GPUVM 第一版规格.md>)，2026-10-19 版：
> 第 2～4 节规定对象、页表和上下文，第 5～7 节规定更新、通知与引用寿命；
> 寄存器集中在[硬件手册第 17 章](<../../../QEMU_2026_GPGPU_硬件手册.html#gpuvm-registers>)。

### 构建与复现 GPUVM

目标内核仍为 `6.18.57-0-gpgpu-virt`，新增内置 `CONFIG_DRM=y`，保留
`CONFIG_GPU_BUDDY=y`，不启用 TTM 或显示驱动。使用独立输出目录，保留原
`build/gpgpu-kernel-cross/` 与 `build/gpgpu-linux-module/`。内核来源、固定
工具链和缓存要求沿用[内核构建说明](#重建内核与运行队列测试)。

从实验子仓库根目录运行：

```sh
python3 software/gpgpu/scripts/kernel-build.py --build-mode cross --jobs 16 \
  --output-dir build/gpgpu-kernel-gpuvm
make -f Makefile.camp test-gpgpu-cp JOBS=4
make -C software/gpgpu/linux test-gpuvm IOMMU=none
make -C software/gpgpu/linux test-gpuvm IOMMU=smmuv3
make -C software/gpgpu/linux test-gpuvm-fault
```

两个 GPUVM 目标共用 `scripts/gpgpu_linux.py`，串行占用同一开发盘。
默认目录为 `build/gpgpu-kernel-gpuvm/`，可用 `GPUVM_BUILD_DIR` 指定其他目录。
端到端目标编译固件、驱动和测试程序，
在 A57 客体装载 `queue_mode=2`，运行五组检查并卸载模块；同时检查实际
IOMMU domain 和内核告警。故障目标调用独立客体脚本，在源码副本中生成三个注入模块，
分别改写管理结果或失效状态读取；生产接口没有故障注入入口。

单独演示时，在相同 GPUVM 客体中安装固件、装载模块后运行
`test-gpuvm`。程序逐项核对 2,051 个向量结果，运行两个真实进程使用相同
GPUVA 的场景，再执行权限故障、重映射和重复关闭检查。测试的 Ring 发布和
等待辅助函数与第二阶段相同，可沿上述四个函数讲解完整对象与访问关系。

<a id="stage-three-validation"></a>

### 第三阶段验收与代码快照

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

## 运行时接口

本节列出 `queue_mode=0` 使用的原同步接口。

公共 API 在 `runtime/gpgpu.h`，驱动 ABI 在 `include/gpgpu_uapi.h`。
API 除 `gpgpuDestroy()` 外返回 0 或负 errno。每个 context 用互斥锁保护
staging buffer 和提交顺序。

- `gpgpuInit()` 打开设备、检查 ABI 版本并映射 staging buffer；
  `gpgpuDestroy()` 解除映射、关闭设备并释放 kernel 对象。
- `gpgpuMalloc()` / `gpgpuFree()` 分配和释放显存。
- `gpgpuMemcpy()` 接收设备偏移、Host 指针、长度与 H2D/D2H 方向。
- `gpgpuLoadKernel()` / `gpgpuUnloadKernel()` 管理代码上传和显存释放。
- `gpgpuLaunchKernel()` 接收 kernel 对象、grid、block 和 Args，上传临时
  参数并同步提交；结束后释放参数分配。
- `gpgpuDeviceSynchronize()` 等待同一 context 已提交的操作；本阶段所有
  操作均同步，取得 context 锁后即可返回。
- `gpgpuGetInfo()` / `gpgpuGetStats()` 查询容量、设备能力和三个中断计数。

Args 按小端 32 位字段打包，设备指针用 `GpgpuDevicePtr`。ARM64 Host
指针通常为 64 位，必须先分配显存并拷贝数据，再把设备偏移放入 Args。
三个 demo 展示了实际参数布局和错误处理。
没有参数的 kernel 会由运行时上传一个全零 32 位占位字段，入口 a0 仍指向
有效分配。

## RV32 编程模型与前端

`kernels/gpgpu_kernel.inc` 规定内置变量地址。语义签名为
`__global__ void kernel(const Args *args)`，实现形式是手写 RV32 汇编。
这个签名用于说明调用约定；本阶段不支持 CUDA C 语法编译。

Args 的 VRAM 偏移放在 a0；线程初始的其他整数寄存器为 0。kernel 没有
自动配置的栈，不能依赖 C 调用约定中的栈帧。代码使用 32 位定长指令，
branch/jal 使用相对地址，便于二进制搬到任意显存分配中执行。

threadIdx 的 X/Y/Z 在 `0x80000000` 的偏移 0/4/8，blockIdx 在偏移
0x10/0x14/0x18；blockDim 和 gridDim 分别从偏移 0x20 和 0x30 开始。
`lw` 读取这些内置变量。三维索引按 X 最快变化顺序展开，warp 为 32 个线程。
向量的一维索引由 `blockIdx.x * blockDim.x + threadIdx.x` 得到；kernel
自行判断索引是否越过元素数，允许末尾 block 包含空闲线程。

`kernels/assemble.py` 是面向本实验 RV32 指令子集的两遍汇编器：第一遍
计算标签地址，第二遍编码寄存器、立即数和相对跳转，输出小端 `.bin` 与
可嵌入 Host 程序的头文件。它检查未知指令、寄存器和跳转范围，不是完整
RISC-V 工具链。编写新 kernel 应使用执行器已支持的指令；不支持的编码会
触发设备错误中断并使 Launch 返回 `-EIO`。

vector add 每个线程计算一个 FP32 元素；矩阵乘每个线程计算一个输出元素，
沿 K 维循环累加；ReLU 对有限 FP32 输入执行 max(x, 0)，包含负零测试。
矩阵乘使用二维 block/grid，向量 demo 使用跨 warp 的 block 和末尾越界
线程。执行器逐 warp、逐 block 执行，提供功能结果，不用于推断真实 GPU
的时序或吞吐量。

## 构建与运行

从仓库根目录执行：

```sh
# Host 构建：汇编 kernel、libgpgpu、demo 和测试程序
make -C software/gpgpu
make -C software/gpgpu check-assembler

# 首次自举 ARM64 QEMU、Alpine Linux 与持久 gcc/kernel headers 环境
make -C software/gpgpu/linux prepare

# 增量编译驱动，检查装卸载
make -C software/gpgpu/linux test

# 在真实 Linux 客体中编译并执行三个 demo 与错误路径测试
make -C software/gpgpu/linux stack-test
# 同一端到端测试也登记在 camp Makefile
make -f Makefile.camp test-gpgpu-stack

# 两种模式共用内核、驱动、编译器磁盘；默认 IOMMU=none
make -C software/gpgpu/linux test IOMMU=smmuv3
make -f Makefile.camp test-gpgpu-stack IOMMU=smmuv3
make -C software/gpgpu/linux run IOMMU=smmuv3

# 已 prepare 的环境可使用 test-fast / stack-test-fast 跳过构建
make -C software/gpgpu/linux stack-test-fast IOMMU=none
make -C software/gpgpu/linux stack-test-fast IOMMU=smmuv3

# 复用 prepare 创建的 ARM64 QEMU 配置，执行两个专项 QTest
make -f Makefile.camp test-gpgpu-iommu JOBS=4
```

Host 构建需要 C 编译器、ar 和 Python 3；QEMU 依赖见仓库 README。
首次 prepare 下载并校验官方 Alpine ARM64 ISO，在客体里安装匹配的 Linux
内核开发包。之后使用保存的 developer.raw 增量编译，无需重新安装客体。
客体程序由 ARM64 gcc 原生构建；RV32 kernel 使用同一份汇编源码生成。

`scripts/gpgpu_linux.py` 的同步验收与交互入口接受 `--iommu none|smmuv3`。启用时使用
`-machine virt,gic-version=3,iommu=smmuv3,default-bus-bypass-iommu=off`，
在原内核参数后追加 `iommu.passthrough=0 iommu.strict=1`。自举和模块编译
继续使用原配置；模式不进入环境指纹，切换模式不会重建环境。

测试要求保存的内核配置启用 `CONFIG_ARM_SMMU_V3=y`、
`CONFIG_IOMMU_SUPPORT=y`、`CONFIG_IOMMU_DMA=y`。客体按 PCI ID
`1234:1337` 找到 GPGPU，检查 `/sys/bus/pci/devices/<BDF>/iommu_group/type`
确实为 `DMA`；`none` 模式要求设备没有 IOMMU group。初始化日志不能代替
这个检查。脚本还检查内核异常及非预期 SMMU fault、事件和队列错误。

所有产物保存在 `build/`。两种模式的日志与结果分别写入
`build/gpgpu-linux-module/none/` 和 `build/gpgpu-linux-module/smmuv3/`：
装卸载为 `test-console.log`、`test.json`；软件栈为 `stack-console.log`、
`stack-test.log`、`stack-test.json`。脚本要求三个示例的输出数量分别为
17,003、437、513，运行时逐项 PASS 和汇总均为 68，并核对客体退出码。
专项 QTest 日志为 `build/gpgpu-iommu-qtest.log`，Makefile 同时检查退出码、
2/2 数量与无跳过。

本次把 ARM64 与 RISC-V 配置在同一构建目录
`build/arm64-qemu`（`--target-list=aarch64-softmmu,riscv64-softmmu`），
原有 GPGPU QTest 的实际命令为：

```sh
for arch in aarch64 riscv64; do
    QTEST_QEMU_BINARY=build/arm64-qemu/qemu-system-$arch \
      build/arm64-qemu/tests/qtest/qos-test \
      -p /$arch/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu/gpgpu-tests \
      --tap
done
```

### 重建内核与运行队列测试

队列模式需要内置 `CONFIG_GPU_BUDDY=y`。固定构建材料保存在
`linux/kernel/`，包括 Alpine 构建提交、源码校验值、配置和有序补丁。
构建沿用已保存的 Alpine `6.18.57-0-virt` 开发盘及匹配的软件包缓存。
`--base-environment` 默认指向 `build/gpgpu-linux-module/`，其中须包含原
`developer.raw`、环境记录和通过清单校验的 `apk-cache/`。通用的
`scripts/gpgpu_linux.py prepare` 会按运行时的软件源取包，未来重跑不保证得到这一
固定版本；重建时应保留并使用已核对的原环境。

交叉流程按 x86_64 Ubuntu 24.04 宿主机准备，使用 `dpkg-deb` 解包固定的
amd64 依赖包。宿主机须已有 `gcc`、`g++`、`make`、`flex`、`bison`、`bc`、
`perl`、`pkg-config`、OpenSSL 开发文件、`tar`、`xz`、`patch` 和
`e2fsprogs`。脚本只按 `cross-host-dependencies.json` 补充 11 个依赖包，
上述基本工具由宿主环境提供。

`scripts/kernel-build.py` 默认使用 `--build-mode cross`，在物理宿主机上
交叉编译 Linux `6.18.57-0-gpgpu-virt`，再将产物导入独立 ARM64 客体：

```text
物理宿主机：固定工具链交叉编译 ARM64 内核和模块
  → 导出源码、配置、生成头及 Module.symvers
  → 独立 ARM64 客体：安装模块、原生重编构建工具及 GCC 插件、生成启动镜像
  → 新内核客体：原生编译驱动和用户程序，运行队列与同步路径验收
```

交叉工具链固定为 Arm GNU Toolchain `15.2.Rel1`，其中 GCC 为 `15.2.1`，
下载地址和 SHA-256 保存在 `linux/kernel/cross-toolchain.json`。宿主机默认
并发数为 `--jobs 16`，输出目录为 `build/gpgpu-kernel-cross/`。客体工具
转换和导出默认使用 `--cpus 4 --memory 4G --disk-gib 32`，等待上限可用
`--timeout` 调整。`--output-dir` 可指定另一套独立输出目录。
原发行版环境 `build/gpgpu-linux-module/` 和原生构建输出
`build/gpgpu-kernel/` 均保留；继续原生构建时使用 `--build-mode native`，
其默认输出仍为 `build/gpgpu-kernel/`。

原 Alpine 配置保存在 `linux/kernel/virt.aarch64.config`，实验增量见
`linux/kernel/gpgpu.config`：启用内置伙伴分配器、DRM/GEM 和固件加载，关闭未使用的
设备、网络与文件系统模块，同时保留启动、PCI、IOMMU、virtio、9p 和
ext4 依赖。交叉配置与已确认的 `linux/kernel/gpgpu.arm64.config` 核对，
只允许编译器、汇编器和链接器的版本记录不同；实际差异记录在输出目录的
`cross-config-differences.json`，功能选项出现差异时脚本停止构建。第三阶段
生成完整配置并使用独立输出目录，第二阶段已有内核产物保持原状。

交叉输出中的构建工具使用宿主机指令集。导入 ARM64 客体时，
`scripts/guest/kernel/guest-cross-prepare.sh` 用客体的 Alpine GCC `15.2.0`
定向重编 `fixdep`、`conf`、`modpost`、`mk_elfconfig`、`genksyms`、签名工具
和已启用的 GCC 插件，并修复源码、输出及依赖文件中的绝对路径。脚本不重新
配置或编译内核；转换前后核对 `.config`、生成头、`Module.symvers` 和模块
链接脚本的内容，保持它们与交叉内核一致。路径元数据允许随安装位置调整。

> **[SOURCE]** 实验子仓库固定提交 `6325a87e4c500f4930c4e0b502a1ec241f83bf26`：
>
> - 工具链版本和校验值：[linux/kernel/cross-toolchain.json](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel/cross-toolchain.json#L1)，[第 1～10 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel/cross-toolchain.json#L1)。
> - 保留环境的版本与缓存校验：[linux/kernel-build.py](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel-build.py#L315)，[第 315～328 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel-build.py#L315)；宿主依赖解包与路径设置：[第 75～96 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel-build.py#L75)。
> - 功能配置差异检查：[linux/kernel-build.py](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel-build.py#L110)，[第 110～123 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel-build.py#L110)。
> - 客体工具重编与最终校验：[linux/kernel/guest-cross-prepare.sh](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel/guest-cross-prepare.sh#L152)，[第 152～216 行](https://github.com/hxfei-git/gpu-study-qemu/blob/6325a87e4c500f4930c4e0b502a1ec241f83bf26/software/gpgpu/linux/kernel/guest-cross-prepare.sh#L152)。

从实验子仓库根目录执行：

```sh
# 默认流程：宿主机交叉编译，再导入独立 ARM64 客体
python3 software/gpgpu/scripts/kernel-build.py --build-mode cross --jobs 16

# 独立构建 RV64 裸机固件，产物为 build/gpgpu-firmware/gpgpu-cp.bin
make -C software/gpgpu firmware

# 使用重建内核，在双进程、IOMMU=none 配置运行五组队列测试
make -C software/gpgpu/linux queue-test BUILD_DIR="$PWD/build/gpgpu-kernel-cross"

# 16 项设备／固件队列检查及 2 组 GPUVM 检查
make -f Makefile.camp test-gpgpu-cp JOBS=4
```

固件链接到 `0x03c0_0000`，代码和静态数据上限为 `0x03ff_0000`，末尾
64 KiB 作为栈。驱动通过 `request_firmware("gpgpu-cp.bin")` 从客体固件目录
取得镜像，检查长度后装入固定保留区。应用分配池只管理前 60 MiB；显存池中
的首个页按客体 `PAGE_SIZE` 保留，并登记到 `gpu_buddy`。

队列脚本构建接口库与 `test-queue`，安装固件后以 `queue_mode=1` 装载模块。
五组检查依次覆盖连续计算、并发发布、等待通知、基本错误退出和卸载与寿命；
每组必须输出一条 `PASS queue-group-N`，总结果为
`GPGPU queue tests: 5/5 PASS`。最后一组保留异常退出进程的队列与显存，
再验证剩余名额和显存区间；脚本随后移除设备，确认资源停止访问后清理。
原同步模式的单／双进程和 `none/smmuv3` 四种组合回归仍单独运行。

### 第二阶段本轮验证状态

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

## 本轮验证记录

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

## 基础 17 道测题覆盖与边界

设备识别、能力、BAR 和显存读写进入 PCI probe 与 VRAM 分配路径；全局
控制、复位和错误状态进入会话初始化、错误返回与超时处理。IRQ 配置进入
MSI-X 完成路径，DMA 配置扩展为客体 RAM 与 VRAM 之间的实际数据搬运。
grid/block、SIMT 身份和整数 kernel 为三维内置变量、地址计算、边界分支
和矩阵循环提供基础。

FP32 kernel 与 BF16、FP8 E4M3/E5M2、FP4 E2M1 转换继续由 RV32
解释器和 softfloat 支持。代码加载与派发接口接收原始 RV32 二进制，因此
基础低精度指令也可以通过相同入口执行。低精度数值正确性沿用已有 QTest
验证；三个新 demo 验证 FP32 应用数据链路，不把它们当作全部低精度格式
的用户态数值测试。当前执行器没有 FP16 转换指令。

共享内存、block 屏障、ballot/shuffle 尚未实现。非零 shared_size 返回
`-EOPNOTSUPP`。命令队列、Doorbell 和完成事件已在当前队列路径实现；
stream、多租户隔离及共享存储继续留作后续扩展。驱动检查请求的分配范围，
但 RV32 Kernel 仍可访问设备 VRAM，不能
作为不可信 kernel 的沙箱。设备已标记不可迁移，需补齐 VRAM、执行上下文
和 DMA 状态保存后才能支持迁移。

单次派发限制为 4096 个 block、每个 block 1024 个线程、总计 65536 个
线程，整体执行预算为 `8 * 1024 * 1024` 条线程指令。num_cus 与
warps_per_cu 是 DEV_CAPS 报告的参数，目前不改变串行调度方式。
