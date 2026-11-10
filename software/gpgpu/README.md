# QEMU GPGPU 软件栈：统一访存、AQL 与 CSR 启动接口

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真工具 |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU | Graphics Processing Unit | 图形处理器 |
| CUDA | Compute Unified Device Architecture | 统一计算设备架构 |
| ARM64 | Arm 64-bit Architecture | Arm 64 位架构 |
| WSL | Windows Subsystem for Linux | Windows 的 Linux 子系统 |
| RISC-V / RV32 | Reduced Instruction Set Computer V / 32-bit RISC-V | 精简指令集／协议版本 5 之前计算执行器使用的 32 位模型 |
| RV64 | 64-bit RISC-V | 本实验原生 CP 与计算执行器使用的 64 位 RISC-V 模型 |
| API / ABI | Application Programming Interface / Application Binary Interface | 应用编程／二进制接口 |
| CLI | Command-Line Interface | 命令行接口 |
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
| INT8 / W8 | 8-bit Integer / 8-bit Weight Storage | 8 位有符号整数／8 位权重存储 |
| RMSNorm | Root Mean Square Normalization | 均方根归一化 |
| Conv | Convolution | 卷积 |
| BN | Batch Normalization | 批量归一化 |
| SiLU | Sigmoid Linear Unit | Sigmoid 线性激活函数 |
| RoPE | Rotary Position Embedding | 旋转位置编码 |
| KV | Key / Value | 注意力的键和值 |
| EOS | End of Sequence | 序列结束标记 |
| DFL | Distribution Focal Loss | 分布焦点损失；本文指检测头的分布投影 |
| RSS / PSS | Resident Set Size / Proportional Set Size | 常驻内存量／按比例分摊共享映射的常驻内存量 |
| SIMD | Single Instruction, Multiple Data | 单指令多数据 |
| FMA / MAC | Fused Multiply-Add / Multiply-Accumulate | 融合乘加／乘累加 |
| FLOP / GFLOP/s | Floating-Point Operation / Giga Floating-Point Operations per Second | 浮点运算次数／每秒十亿次浮点运算 |
| INT64 | 64-bit Integer | 64 位整数 |
| NMS | Non-Maximum Suppression | 非极大值抑制 |
| IoU | Intersection over Union | 交并比 |
| RGB | Red, Green, Blue | 红、绿、蓝颜色通道 |
| NCHW | Number, Channels, Height, Width | 批次数、通道、高度、宽度的张量布局 |
| JPEG | Joint Photographic Experts Group | 联合图像专家组；本文指其图像格式 |
| BPE | Byte Pair Encoding | 字节对编码 |
| YOLO | You Only Look Once | 单阶段目标检测模型系列 |
| COCO | Common Objects in Context | 常见物体场景数据集 |
| mAP | Mean Average Precision | 平均精度均值 |
| BMM | Batched Matrix Multiplication | 批次矩阵乘法 |
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
| RT | Runtime | 本仓库 `lib/rt/` 小运行时 |
| PC / PID | Program Counter / Process Identifier | 程序计数器／进程编号 |
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
| dcache | Data Cache | 数据缓存；本实验指翻译后的设备级数据缓存 |
| WB | Write-Back | 写回；本实验指页属性允许进入 dcache 的访问类型 |
| VM | Virtual Memory | 本文指一个文件持有的 GPU 地址空间 |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| VPN | Virtual Page Number | 虚拟页号，即 GPUVA 除以 4 KiB |
| PTE / PDE | Page Table Entry / Page Directory Entry | 页表叶子项／页目录项 |
| TTM | Translation Table Maps | 图形对象存储位置与迁移管理框架 |
| VMA | Virtual Memory Area | CPU 进程的虚拟内存映射区域 |
| CSR | Control and Status Register | 控制与状态寄存器；本实验用于读取启动信息 |
| SGPR / VGPR | Scalar / Vector General-Purpose Register | 标量／向量通用寄存器；本版仅保存独立启动状态 |

## 0. 代码结构与分层

一次计算从应用一路走到 RV64 线程，依次经过五层代码。每一层只调用下一层公开的接口，所有层共用 `include/gpgpu/` 中的协议头：

```text
demos/gpgpu-demo.c          小型计算示例
或 gpgpu-model-eval.c       真实算子回放 → model_ops.c
或客体模型应用              gpgpu_model_apps.py → libgpgpu-model.so
                           model_runtime.c → model_ops.c
或 gpgpu-model-run.c        离线验证 CLI → 同一个 model_runtime.c
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

协议头按内容拆分：`gpgpu_regs.h` 是 BAR0 寄存器，`gpgpu_cp_map.h` 是 CP 私有地址空间，`gpgpu_queue_abi.h` 是 MQD、管理命令、AQL 包、Kernel 描述对象与 Signal，`gpgpu_ih_abi.h` 是 IH 记录，`gpgpu_vm_abi.h` 是页表项与 GPUVA 布局，`gpgpu_version.h` 是协议版本，`uapi/gpgpu_drm.h` 只描述 ioctl。KMT/RT 只包含用户 ABI：`uapi/gpgpu_drm.h`、`gpgpu_queue_abi.h` 与 `gpgpu_vm_abi.h`，看不到寄存器。`gpgpu_tensor_abi.h` 定义有界矩阵／布局指令的通用参数前缀；算子回放器与手写 Kernel 共用 `gpgpu_eval_abi.h`，其前缀引用通用定义，另加软件操作编号和数学常量。

设备模型 `hw/gpgpu/` 按硬件模块分目录，每个模块只访问自己的状态，跨模块通过对方头文件中的函数：

- `gpgpu.c` 负责 QOM/PCI 组装、全局寄存器和复位顺序；`bar0.c` 把 BAR0 每个 0x100 字节的寄存器组挂成独立子区域，统一检查访问宽度，并提供 CP 本地寄存器窗口。
- `cp/`：`cp_ctrl` 管理原生 CP 的复位与启动，`cp_window` 是 CP 访问 GPU 地址的 64 KiB 窗口，另有 `hqd` 与 `doorbell`。
- `compute/`：`dispatch` 处理执行器寄存器与 CP 握手，`scheduler` 按 warp 分段推进，`rv64` 解释标量和自定义指令，`lane_mem` 负责取指与 1／4／8 字节数据访存；`tensor` 通过用户访存读取有界块，执行矩阵或布局指令。
- `mem/`：`access`（设备内全部 GPU 业务访存的入口，按访问类型经过或绕过 dcache）、`vram`、`phys`（GPU 物理地址到 VRAM 或 Outbound 的路由）、`gpuvm`（用户地址翻译、页缓存属性与故障记录）、`dcache`（WB 数据读写的缓存与维护命令，行为见 [dcache 规格](<../../docs/dcache 第一版规格.md>)）、`dma`。直接读写后备存储的函数只在 `mem/backend.h` 中声明，其他模块经 `access.h` 访问。
- `irq/`：`irq` 是中断锁存与 PCI 通知，`ih` 是通知记录 Ring。

CP 固件 `firmware/` 中，`main.c` 是启动检查与调度循环，`ring.c` 负责取包与回收，`mgmt.c` 处理管理命令，`aql.c` 处理派发包，`ih.c` 负责通知与错误，`mem.c` 经访存窗口读写 GPU 地址并恢复访存异常。

Linux 驱动 `linux/` 按对象分文件：`gpgpu_main.c`（PCI）、`gpgpu_dev.c`（设备生命周期与固件）、`gpgpu_mgmt.c`、`gpgpu_ih.c`、`gpgpu_drm.c`（ioctl 与会话）、`gpgpu_bo.c`、`gpgpu_vram.c`、`gpgpu_vm.c`、`gpgpu_queue.c`、`gpgpu_event.c`。对象关系、寿命与锁的顺序写在 `gpgpu_drv.h` 文件头。

协议当前为版本 8，组件升级见 [PROTOCOL_V8.md](./PROTOCOL_V8.md)；显存与固件布局的变化见 [PROTOCOL_V7.md](./PROTOCOL_V7.md)，缓存接口见 [PROTOCOL_V6.md](./PROTOCOL_V6.md)。

## 1. 应用提交与统一访存

Host 指 ARM64 Linux 客体中的 CPU 与应用。应用通过 DRM render node 创建 GEM 对象，由 HSAKMT 风格库提交 AQL 包；RV64 CP 处理队列，RV64 计算执行器运行 Kernel。设备属性 `gpuvm=on/off` 决定用户地址含义，默认开启。两种状态使用同一套对象、队列、等待和回收接口。

```text
打开设备：库创建本进程的通知页并登记给驱动
  → 应用：创建并绑定代码、描述符、参数和数组
  → 创建 AQL Queue，填写包体并发布 Header
  → 8 字节写 Doorbell
  → RV64 CP 读取描述符、派发 RV64 Kernel
  → Signal → 通知槽 → IH → INVALID Header → 消费进度
  → Host 复查 Signal、读取并比较结果
  → 排空并卸载 Queue → 销毁事件 → 解绑并释放对象

CP 用户访问 / RV64 取指与数据访问
  ├─ gpuvm=on：GPUVA → 页表与权限检查 → GPU 物理地址 + 页属性
  └─ gpuvm=off：输入就是 GPU 物理地址，属性为 DEVICE
                      ↓
                访存入口，按访问类型分流
                  ├─ WB 数据读写 → dcache，未命中时向下回填
                  ├─ DEVICE 数据读写 → 先处理重叠的缓存行，再向下访问
                  └─ 取指，直接向下访问
                      ↓
                公共物理访问入口
                  ├─ VRAM
                  └─ Outbound → Host IOVA → PCIe DMA
```

GPUVM 开启时，叶子项没有标为不可缓存的页按 WB 访问，数据读写在权限检查之后经过设备共用的 dcache，VRAM 与 Host 数据都可以缓存；GPUVM 关闭时数据访问都不进入 dcache。取指不经过 dcache；页表遍历、CP 管理域、BAR2、DMA 与 IH 按 DEVICE 访问。Ring、进度、Signal 和通知页是不可缓存对象，驱动的控制对象只经管理域和 IH 访问；CPU 直接改写可缓存对象后，要先失效设备中的旧副本再发布任务。缓存行为与交接顺序见 [dcache 规格](<../../docs/dcache 第一版规格.md>)。

**[DESIGN]** GPU 物理地址为 32 位，VRAM 固定为 2 GiB，与 Outbound 在 `0x8000_0000` 边界相邻，中间没有空洞。首页与固件保留区的完整布局见 [CP/MEC 规格第 3.1 节](<../../docs/CP_MEC 第一版规格.md#spec-firmware-layout>)。Outbound 为 `[0x8000_0000, 0x1_0000_0000)`；Host IOVA `0x4100_0000` 对应 GPU 物理地址 `0xc100_0000`，IOVA 0 对应 `0x8000_0000`，允许正常使用。

Outbound 布局由设备固定，BAR0 `0x0b00` 起的布局寄存器只读；驱动在启动 CP 前读回核对，再写 `OUTBOUND_CONTROL` 打开窗口并读取确认状态。窗口由整个设备共用，队列运行期间不重配。DMA 分配统一采用 31 位 mask，保留原始 DMA 地址供 DMA API 释放和映射，转换后的 GPU 物理地址写入设备协议。Host BO 仍逐对象使用 `dma_alloc_coherent()`。

Outbound 只决定访问路由。Host 存储或 IOMMU 映射不存在时访问仍会失败。SMMUv3 功能保留；开启后，PCIe DMA 再由系统 IOMMU 将 IOVA 翻译成 Host 物理地址。

<a id="stage-three-gpuvm"></a>

## 2. GPUVM、对象绑定与资源回收

GPUVM 开启时使用 48 位 GPUVA、四级 4 KiB 页表、两个用户 VMID 和 64 组 × 4 路的组相联 TLB。VMID 在建队列时取得、队列卸载后归还；驱动取得和归还 VMID 时失效整个 VMID，占有 VMID 期间绑定或解绑对象只失效该对象的 VPN 范围，设备同步完成并以 ACK 确认。PTE 保存地址域、SYSTEM 位、缓存属性 UNCACHED 和有效、读、写、执行权限：Host 页写 IOVA 并置 SYSTEM，设备换算为 `0x8000_0000 + IOVA`；VRAM 页写 VRAM 偏移。根目录使用 GPU 物理地址，驱动的中间表页都在 VRAM，遍历通过公共物理入口读取，不再次进入 GPUVM 翻译。

用户 GPUVA 的唯一可分配区间为 `[0x1000, 0x1_0000_0000_0000)`。用户库先用 `mmap(PROT_NONE)` 预留一段对齐的 CPU 地址，把对象经 render node 映射到这里，再以同一数值 MAP，所以对象的 GPUVA 就是它在本进程中的 CPU 地址。Host 对象的这段地址可读写，就是公开的 `cpu`；VRAM 对象的这段地址只占位，CPU 访问会触发段错误，`cpu` 为 NULL。释放时先 UNMAP，成功后才解除 CPU 映射。

每个进程有一页通知页，由库在 `gpgpuKmtOpen()` 中创建，`BO_CREATE` 带 `GPGPU_BO_EVENT_PAGE` 登记给驱动；事件的通知槽都在这一页中。`0x8000_0000` 与 `0x9000_0000` 均为普通 GPUVA，不再代表控制页或参数窗口。GPUVM 关闭时不创建页表，能力查询返回零个 GPUVA 区间，MAP 返回对象固定的 GPU 物理地址；非零 `fixed_va` 请求返回不支持。

```text
GEM handle ─────────┐
GPU 绑定引用 ────────┼→ 固定 Host / VRAM 存储
CPU mmap 引用 ──────┘

关闭 handle：只解除句柄引用
Ring、进度对象：Queue 卸载后才允许解绑
通知页：会话关闭时由驱动撤销登记后才解绑
解绑：开启 GPUVM 时撤销页表并确认 TLB 失效
最后引用结束：归还存储
```

两种状态均建立独立绑定引用；地址为零或没有页表不能作为释放条件。Queue 存活期间可以绑定和解绑其他对象；驱动不知道哪些对象还在被任务使用，由应用确认任务结束后再释放。未知管理完成结果或无法确认访问结束时继续保留资源。正常关闭按 Queue、事件、绑定、存储顺序执行，CPU mmap 独立保活实际存储。

全设备保留两个用户 Queue 名额，每上下文一条。关闭 GPUVM 后，各进程共享物理地址空间，不提供页权限隔离。VRAM 首页及固件区继续受用户访问保护；管理队列、MQD、管理完成区、IH 与 COPY 端点统一使用 GPU 物理地址。

CP 的管理域和用户域显式区分，不根据 `VMID == 0` 猜测地址含义。CP 自己的代码、栈和本地访存窗口继续使用 RV64 CP 地址空间。

## 3. RV64 Kernel 的启动接口

**[DESIGN]** 每 Warp 保存九个只读共享启动值：Block 编号、Block 尺寸、Grid 中的 Block 数量。每 Lane 保存三个线程坐标。新 Warp 创建时填写一次，分段继续执行时保留；X 维变化最快。启动状态独立于 Kernel 可覆盖的通用寄存器。

```text
0xCC0～0xCC2：THREAD_ID_X / Y / Z
0xCC3～0xCC5：BLOCK_ID_X / Y / Z
0xCC6～0xCC8：BLOCK_DIM_X / Y / Z
0xCC9～0xCCB：GRID_DIM_X / Y / Z
0xF14：mhartid，保留原有编码
```

`csrr` 汇编为 `CSRRS rd, csr, x0`。执行器仅接受此只读形式；`rd=x0` 也先检查 CSR 是否有效。未知 CSR、写入及其他未支持形式均返回指令错误。汇编器接受数字与 `.equ` 常量，并检查 CSR 编号范围。

执行器的整数寄存器与 PC 为 64 位，现有指令子集按 RV64 语义执行，另有 LD、SD 读写 64 位指针。`a0` 直接保存 64 位 `kernarg_address`：开启 GPUVM 时为 GPUVA，关闭时为 GPU 物理地址。参数块每项 8 字节，Kernel 用 `ld` 读取。24 字节 Kernel 描述对象在偏移 `0x00` 保存 64 位代码入口，在 `0x10` 保存当前 `GPGPU_KERNEL_ABI_VERSION`，`0x14` 的保留字段为 0，其他值被拒绝；AQL 包仍为 64 字节。设备、固件、驱动、用户库与描述对象共用协议版本，组件必须一起更新。

<a id="stack-runtime"></a>

## 4. 公共库与三个计算示例

`GpgpuKmtMemory` 由库分配，调用者只读其中的 CPU 映射 `cpu` 和唯一的设备地址 `gpu_address`，不需要区分 Host 地址、VRAM 偏移或 GPUVA。`gpgpuKmtAllocMemory()` 先创建整对象再绑定，权限由调用者指定：代码为读与执行，普通数据为读写。`flags` 为 `GPGPU_BO_UNCACHED` 时对象按不可缓存绑定，不进入 dcache，运行时用它创建 Ring、进度对象和 Signal；参数块、暂存区和计算数组使用可缓存对象。参数块每项 8 字节，`gpgpu_rt_dispatch()` 接收 `uint64_t` 数组，对象的 `gpu_address` 原样写入，不截断；写完参数后先用 `gpgpuKmtSyncMemory()` 失效参数块在设备中的旧副本，再发布包。

KMT 中的 `gpgpuKmtReserve()`、`gpgpuKmtPublish()`、`gpgpuKmtWaitSignal()` 和 `gpgpuKmtDrainQueue()` 分别负责槽位预留、Header 与 Doorbell 发布、Signal 与事件等待、排空。运行时 `gpgpu_rt_dispatch()` 按顺序调用它们完成一次派发。默认等待先主动检查约 200 μs，再进入事件等待，唤醒后复查 Signal。

驱动的事件是一次性的，被通知后不会回到等待状态，所以运行时每次派发都创建新事件，计算完成后销毁。Signal 归零可能早于驱动处理完对应的通知，这时驱动暂不允许销毁，旧事件留到下一次派发前再试。提交或等待失败后，CP 可能仍在读写参数块和 Signal，运行时不再派发，只能由 `gpgpu_rt_destroy()` 卸载队列后回收。

`gpgpu-demo` 链接 `libgpgpu.a`，经运行时把代码、描述符和计算数组放在 VRAM；参数、Ring、进度和 Signal 放在 Host BO，经 Outbound 访问。用于 COPY 的 Host BO 也在 Queue 创建前准备。三个例子保持原输入和比较规则：

- 向量加法比较 17,003 个结果，Grid 为 266×1×1，Block 为 64×1×1。
- 矩阵乘法计算 19×17 · 17×23，比较 437 个结果，Grid 为 3×3×1，Block 为 8×8×1。
- ReLU 比较 513 个结果，Grid 为 6×1×1，Block 为 96×1×1，并保留负零结果检查。

另有一个生命周期示例：同一条队列存活期间释放输出数组、重新分配后再派发一次，两次各比较 17,003 个结果。第二次派发前还会改写 X 并经 COPY 重新上传，结果按新输入比较。它在第一次派发前打印 X、Y、Z 的设备地址，GPUVM 开启时就是这三个数组在进程中的 CPU 地址，关闭时是 GPU 物理地址。

任一数值比较、提交、等待或正常资源回收失败，程序返回非零退出码。

<a id="model-operator-replay"></a>

真实算子由 `scripts/gpgpu_model_eval.py` 和 `demos/gpgpu-model-eval.c` 配合回放，不向客体安装模型框架。以 Qwen decode 的 RMSNorm 为例，主机已有真实输入、权重和 CPU 参考输出，客体要算出全部 896 个结果再逐值比较：

```text
主机：固定模型、输入与 CPU 参考
  → prepare：生成命令块，按原 stride 恢复输入的物理存储布局
  → run：启动同一 1 GiB ARM64 客体与远端设备
       → 客体回放器：分配 BO，用小 staging 分块上传输入和权重
       → RT/KMT → AQL → CP → RV64 Kernel
            归约：每行平方和 → 均值加 epsilon → 倒数范数
            缩放：每个输入乘倒数范数及对应权重 → 完整输出
       → 分块回读全部输出 → 与 CPU 参考比较 → 正常回收
```

`kernels/model_eval.S` 实现计算与布局操作，`model_math.inc` 提供其使用的数学近似。复合算子包含全部归约、逐元素处理和分块派发，`operator_ns` 覆盖参数上传与同步；输入文件读取、分配清零、映射、输入 H2D、验证 D2H 和 CPU 比较分别记录。离线打包只恢复输入原有的 stride，网络中的复制、重复和拼接仍由设备执行，不能把 CPU 参考结果作为设备输出。

回放器默认使用 `--backend scalar`，每个 Lane 按原标量指令计算输出。选择 `--backend block` 后，线性投影、批次矩阵乘、卷积、复制、拼接和 nearest 的 Lane 改为处理一个有界工作块，其他算子及复合归约保留原标量步骤。矩阵 Kernel 仍逐块推进 K，全部分块与布局工作都计入 `operator_ns`，不在 CPU 预先计算 im2col 或输出。指令与参数合同见[硬件手册](<../../docs/GPGPU_硬件手册.html#tensor-block>)；运行配置、实际路径和结果在第 5 节维护。

`run --dual` 在同一个 ARM64 客体中启动两个独立回放进程，分别打开设备、建立自己的队列和地址空间。两个进程使用同一份冻结材料，各自持有输入与输出，按文件屏障协调：

```text
Qwen 进程：装载与建队列 → ready ─┐
YOLO 进程：装载与建队列 → ready ─┴→ 主机发布 start
                                    → 各自回放、完整回读比较 → 各自写 done
两个 done 均可见 → 主机发布 release → 两进程分别卸载与释放
```

准备与回收不插入另一进程的长任务中；共同运行期间仍沿 CP 的整 Dispatch 串行调度。每个正式算子的 `operator_ns` 和共同回放区间的宿主墙钟分别保存。后者包含预热、回读验证及屏障观察，不是纯计算时间；先完成的进程等待共同收尾，有限样本的后段可能已解除竞争，不能作为持续饱和负载的稳态吞吐。

**[BOUNDARY]** 上述回放入口只执行独立算子。Qwen 的输入和已有 KV 来自原权重 FP32 网络，W8 算子另与同一份 INT8/scale 的 FP32 参考比较；设备误差与量化误差分开记录，不代表完整量化模型的生成质量。数学近似针对材料中的有限输入范围，不是完整 libm；实际覆盖、运行命令、性能估算与日志均见下一节。

<a id="full-model-runtime"></a>

### 4.1 完整模型的常驻数据与请求执行

用户在 ARM64 Linux 客体中运行 `gpgpu-qwen-chat` 或 `gpgpu-yolo`，由客体 CPU 接收问题、读取 JPEG，并处理设备返回的结果。WSL 只在运行前导出固定模型材料、构建和启动 QEMU；每次请求不经过 WSL 分词或图像处理。应用包不含 CPU 参考激活、KV 或最终输出，独立参考不挂载给客体。

```text
WSL：离线导出固定权重、配置与官方 tokenizer；启动 QEMU
                    │ 只读共享固定材料和原始 JPEG
                    ▼
ARM64 客体 Linux：gpgpu_model_apps.py
  ├─ gpgpu-qwen-chat：终端输入 → 官方模板与分词 → 按实际历史构图
  └─ gpgpu-yolo：读取 JPEG → LetterBox、RGB/NCHW、归一化
                    ↓ ctypes 同步调用，不启动 WSL 请求服务
  libgpgpu-model.so：model_runtime.c → model_ops.c
                    ↓
  RT/KMT → DRM 驱动 → AQL/CP → RV64 Kernel／张量块指令
                    ├─ Qwen：全部层、RoPE、设备写 KV → logits
                    │        客体 argmax → 实际 token → 下一次设备 decode
                    └─ YOLO：完整网络、DFL 与检测头 → pre-NMS
                    ↓
客体 CPU：中文解码与历史维护，或 NMS、坐标还原、绘框和结果保存
                    ↓ 读写共享结果目录
WSL：可查看客体生成的文本、检测 JSON 与 annotated.jpg
```

`scripts/gpgpu_model_apps.py export` 只导出固定材料，不把用户文本或图片编进模型包。聊天程序在会话开始时打开一次 C 后端，上传权重并保留 RT、Kernel 与各类 BO。之后客体按实际问题生成新图，后端先校验权重合同，再在 idle 状态按需扩大非权重 BO；换图不重新读取或上传权重。同步请求失败后停止使用本会话；设备访问未确认结束时保留资源，只能继续收尾。

`model_graph.py` 记录张量的 BO 类别、字节偏移、shape 和元素 stride。具名权重只装一份，激活在最后一个消费者完成后复用；有别名或残差依赖的存储继续保留。非连续输入只有在算子支持对应 stride 时直接使用，否则由设备 Copy 重排。`model_ops.c` 同时供原算子回放器和完整模型使用，基本算子选择现有块后端，Softmax、RMSNorm 等拆成软件 Kernel 的归约与逐元素步骤。这里没有 CPU 网络 fallback，也没有增加 Dispatch 级取指或译码缓存。

Qwen 使用逐输出行 INT8 权重和 FP32 计算，embedding 与 lm_head 共用同一量化矩阵。每层 K/V 分别保存 `[capacity,2,64]` 的 FP32 token-major 数据；新 K 在 RoPE 后写入，V 不旋转。两个 KV head 各供七个 Q head 使用，BMM 以零 batch stride 共享对应历史前缀，不扩成十四份 KV。

设 prompt 有 `L` 个 token。设备完成分块 prefill 后，KV 保存 `L` 个已执行输入位置，最后位置的 logits 产生第一个新 token。只有把这个 token 再送入 decode，KV 才增长到 `L+1` 并产生下一个 token。因 EOS 或预算停止时，最后选出的 token 尚未作为输入执行，不能把它计入 KV 长度。每个新请求从本次 prompt 重建可读前缀，未使用的容量不参与 attention。

聊天程序读取固定的官方 tokenizer 资产与原聊天模板，采用从官方纯 Python 实现提取的 byte-level BPE，支持中文编码和解码；客体不安装 Torch、Transformers 或 CPU 模型。一次回答按有界前向窗口构图，序列化后释放 Python 节点对象；窗口间保留相同容量、布局与绝对位置的设备 KV，避免按整个生成预算常驻重复的网络元数据。客体把实际已生成的 token 填入下个窗口的 INPUT，设备接着消费尚未执行的末 token。每个新的对话轮把此前真实的 user／assistant 文本与新问题重新编码，设备完整重算这段前缀；首版不跨对话轮复用 KV 内容。`/clear` 只清除 CPU 对话历史，下一问从新前缀开始，权重仍常驻；`/exit` 关闭会话并回收。超出容量明确拒绝，不自动截断。回答在本轮生成结束后显示，尚未实现流式输出。

YOLO 保持原版 YOLO11n 的 FP32、未融合 Conv/BN 路径，完整展开多分支、C2PSA/Attention 和三尺度检测头。DFL、框解码、stride 缩放及类别 sigmoid 在设备完成；CPU 只做图像预处理、NMS、坐标还原和结果保存。生成与检测命令、实际结果和边界统一见[完整模型运行记录](#model-e2e-results)。

> **[SOURCE] 可选源码索引**：当前工作区，基线 `8a435ac`；实际运行的源码散列随第 5 节结果保存。
>
> - 设备逐轮执行、实际 token 检查与写回：[model_runtime.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/demos/model_runtime.c:1166:1>)，[第 1166～1290 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/demos/model_runtime.c:1166:1>)。
> - 常驻资源与换图调用：[model_runtime.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/demos/model_runtime.c:1613:1>)，[第 1613～1730 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/demos/model_runtime.c:1613:1>)。
> - 客体输入、窗口推进与后处理：[gpgpu_model_apps.py](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_model_apps.py:255:1>)，[第 255 行起](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/gpgpu_model_apps.py:255:1>)。
> - 激活最后消费者规划：[model_graph.py](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_graph.py:440:1>)，[第 440 行起](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_graph.py:440:1>)。
> - 完整模型编排：[model_qwen.py](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_qwen.py:213:1>) 的[第 213 行起](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_qwen.py:213:1>)；[model_yolo.py](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_yolo.py:209:1>) 的[第 209 行起](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/scripts/model_yolo.py:209:1>)。

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

# 用户库、gpgpu-demo 与三个 RV64 Kernel（输出到 build/gpgpu-stack），以及教学汇编器回归
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

<a id="model-e2e-results"></a>

**客体模型应用（协议 8）。** 应用在 QEMU Linux 内接收文本或 JPEG、调用设备并保存结果，分层见[第 4.1 节](#full-model-runtime)。模型包复用下方[冻结材料](#model-eval-results)，不重复下载权重；更换问题或图片不需要重新导出包。

客体需要 Python、NumPy、OpenCV、Pillow、regex 和 Jinja2，不需要 Torch、Transformers 或 Ultralytics。当前开发盘已用原 APK 签名验证安装这些依赖，并实际导入；版本和安装日志见[客体依赖记录](../../build/gpgpu-guest-apps/installation-v2/result.json)，离线包清单见[依赖缓存](../../build/gpgpu-guest-apps/dependencies-v2/manifest.json)。构建需要支持共同目标规则的 GNU make 4.3 或更新版本。这里只确认现有开发盘，不据此宣称从零引导已通过。

已有普通交互客体时，先在该客体退出应用并关机，再从 WSL 启动模型模式；不要把协议 7 的已加载驱动继续配给新设备。当前默认固定包为 `build/gpgpu-model-apps/default/`：

```sh
# WSL：使用 O2 远端设备，开启 shared_fetch、tensor_native 和 GPUVM。
# 只读共享模型和原始图片；结果目录读写共享。
make -C software/gpgpu/linux run-models
```

进入 QEMU Linux 后运行：

```sh
sh /mnt/gpgpu-src/model-shell.sh

# 以下输出目录必须尚不存在；也可省略 --output，使用自动生成的新目录。
gpgpu-qwen-chat --max-new-tokens 16 --output /mnt/gpgpu-output/chat-manual-new
# 在“你：”后输入问题；/clear 清历史，/exit 退出模型会话。

gpgpu-yolo --image /mnt/gpgpu-input/bus.jpg --output /mnt/gpgpu-output/yolo-manual-new
```

聊天的 `session.json`、各轮 `generated.txt`／`generated.json` 和有界窗口的程序、结果与 `model.log` 均由客体写入。YOLO 生成 `result.json`、`postprocess/detections.json` 和 `postprocess/annotated.jpg`；WSL 可在 `build/gpgpu-model-results/` 的对应目录查看，WSL 不执行本次请求的图像处理。检测先绑定原 JPEG 的内容指纹，推理期间原路径若被替换，后处理会拒绝，不把旧图的框画到新图上。

需要重新导出固定包时，使用新的目录，并把同一目录传给启动入口；导出不执行 CPU 网络前向，也不绑定用户输入：

```sh
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_apps.py export \
  --output build/gpgpu-model-apps/user-new
make -C software/gpgpu/linux run-models MODEL_DIR="$PWD/build/gpgpu-model-apps/user-new"
```

`MODEL_DIR`、`INPUT_DIR`、`RESULT_DIR` 分别决定只读模型、只读原图与读写结果目录；`MODEL_DEVICE_QEMU` 指定远端设备。启动脚本重建所选设备、检查驱动并同步固件；模型 shell 在客体编译共享库和入口，然后装载驱动。应用退出时确认 RT/KMT 收尾；打开失败或访问未确认结束时，不记录为正常回收。

**W8 数值定位。** 原 2048 容量失败记录与 `atol=rtol=2e-4` 保持不变。同输入逐位重放已复现旧设备的 11 个超差值，误差在第 21 层 MLP 的大值残差抵消中放大：RMSNorm 原串行平方和为 `2,976,165.0`，补偿结果为 `2,976,168.75`；一个 down 通道的旧逐项 FMA 为 `-1630.0787353515625`，固定块 FP32 补偿为 `-1630.07666015625`，CPU128 为 `-1630.0767822265625`。这些差异经残差抵消传播，不能只凭“FP32 误差”关闭失败项。

协议 8 的 W8 块规则见[硬件手册](<../../docs/GPGPU_硬件手册.html#tensor-block>)；软件 RMSNorm 的平方和使用 Kahan 累加，并保留非有限值处理。WSL 上的真实源码重放已把原 hidden23 首位置的超差从 11 个降为零，三条数学路径及异常核对通过。该重放只覆盖首位置，设备首块与完整容量结果分别如下。根因、原数值和定向复核见[数值定位记录](../../build/guest-apps-numeric/root-cause-v1/validation-summary.json)。当前协议 8 的[QTest 日志](../../build/gpgpu-guest-apps/qtest-v8-attempt2/gpgpu-qtest.log)通过退出状态和 TAP 计划／结果核对，两版验证分别保留。

设备端的[128-token 严格诊断](../../build/gpgpu-guest-apps/prefix128-v8-attempt2/result.json)通过全部 40 项张量检查，退出与资源回收正常，请求为 `218.368 s`，仅作诊断耗时。首块另直接使用原容量用例的黄金参考核对，三层 hidden、K/V 的九项检查全部通过；hidden23 的超差值为零，最大绝对差为 `0.0004348755`，相对 L2 为 `3.27734e-6`。这项[原容量参考对照](../../build/gpgpu-guest-apps/prefix128-v8-attempt2/original-capacity-first-chunk.json)只覆盖首块，完整 2048 容量结果单独判断。

**原容量边界已通过完整复验。** 原 2041-token 前缀加八个生成 token 的诊断请求完成全部 23 轮，215 项张量检查全部满足原 `atol=rtol=2e-4`，实际 KV 前缀到 2048 个位置，应用退出码为零，资源回收和内核日志正常。八个生成 token 与原参考一致。原输入、原 CPU 参考和旧失败结果未改；本次只提高保护性等待上限，未把等待上限当作性能门槛。完整请求为 `6443.182 s`，约 `107.4 min`，开启检查点，仅作诊断耗时。

**客体聊天与图片请求。** 同一聊天进程依次完成“记住名字”、根据历史回答名字、`/clear` 后重新问候，实际输出为“记住了。”“小明。”“你好。”，均以 EOS 停止。三问分别为 44、65、35 个 prompt token，生成四、四、三个 token；前两问各跨两个前向窗口，第三问使用一个窗口。独立官方 tokenizer 与完整 W8 CPU 参考分别核对了模板、token、文本、停止原因、历史、窗口连续性和 KV 长度，全部通过；参考在客体退出后生成，不向客体提供参考 token。

两张 JPEG 都由客体读取、预处理并调用同一份固定 YOLO 包：原图得到一辆公交车和四个人，客体生成的水平镜像得到一辆公交车和三个人。两图的预处理输入均与独立参考逐字节一致，完整 `[1,84,2100]` 预测和最终检测都满足原容差；最终框最大差约 `0.000122` 像素，类别匹配。标注图由客体保存，已实际打开查看；更换图片没有重新导出模型。

**客体应用性能。** 下列均为本机非隔离系统的单次功能测量，不构造稳态吞吐或置信区间。完整命令墙钟由 WSL 同一计时器记录，包含应用启动、最终输出保存与关闭；客体的 `request_ns` 截止解码或检测后处理，最终状态文件和关闭在其后。两个计时域分别报告，不直接相减。

YOLO 两次完整应用墙钟为 `75.340 s`、`70.377 s`；客体网络前向为 `17.309 s`、`17.136 s`。客体模型准备约 `15.5 s`，预处理与材料校验区间约 `26.6～27.4 s`，其中图像数值预处理仅约 `0.25～0.28 s`；后处理约 `1.68～2.71 s`，NMS 约 `0.02～0.06 s`。因此应用等待还包含材料校验和 CPU 处理，不能只报网络前向。设备算子时间中，SiLU 约占 `39.1%`、未融合 BN 占 `34.3～34.6%`、卷积占 `14.5～14.6%`；按网络阶段划分，backbone 约占 `50%`、Detect 占 `27.5%`、neck 占 `18.4%`、attention 占 `4.1～4.2%`。

Qwen 三问完整会话墙钟为 `1093.977 s`。首次应用准备为 `395.242 s`，其中 C 资源与权重准备为 `215.638 s`：BO 分配约 `63.703 s`、权重读取约 `117.705 s`、权重 H2D 约 `24.843 s`，另有应用包校验、官方 tokenizer 初始化与库装载等开销。权重只上传一次，后续各问的 INPUT H2D 总量约 `4～13 KiB`。本次生成预算为八个 token、prefill chunk 为八，与默认配置不同；三问从输入后到解码完成分别为 `233.211 s`、`287.339 s`、`154.089 s`，不宣称实时交互。

三问的设备 prefill 前向分别为 `98.779 s`、`147.694 s`、`80.332 s`；之后三、三、两次 decode 前向累计为 `33.182 s`、`33.927 s`、`22.185 s`。客体构图和程序落盘另占 `93.116 s`、`98.887 s`、`47.880 s`，是明显的应用层成本。有界窗口限制常驻元数据，不减少完整网络计算。设备算子时间中，W8 投影约占 `41%`、Softmax 占 `11.5～11.8%`、RMSNorm 占 `8%`；参数上传约占算子时间的 `17%`，已包含在上述分类里，不再叠加。这些分解不能证明未单独计量的驱动锁或队列等待占比。

运行仍使用 1 GiB 客体和 2 GiB VRAM；Qwen 窗口的模型逻辑 arena 峰值约 `522.23 MiB`，不含页表、RT 控制对象和对齐。设备进程与 ARM64 客体进程的 RSS 高水位约为 `2070 MiB`、`1009 MiB`，不是可直接相加的模型显存量。相关进程采样交换量为零，全局交换使用量前后无净增加；非连续采样不证明全程没有交换。

**回归与代码评审。** 完整算子回放通过全部 250 个数值签名，共比较 `18,398,447` 个输出，失败值为零，各进程退出正常且未保留在途资源。标准全栈在 GPUVM 开、关下都通过计算与生命周期示例，内核日志核对通过；两套 QEMU、用户态 `all check` 和接口 `kernel-doc` 通过。独立 code review 及窗口／资源寿命复核已完成，已确认的本次问题已修复，覆盖 token 值域、文件边界、跨文件系统导出、成对构建产物、图像身份、形状来源、有界构图和中断后的实际回收状态。既有 Conv 非有限 padding 差异经基线核对不属于本次引入，未扩大修复范围。

Linux checkpatch 按本项目 typedef 约定核对，中文行宽另按 8 列制表符、宽字符两列检查真实的 100 显示列；最终无错误、无告警。末次 C 换行调整重新构建后，WSL 侧与 ARM64 客体侧 CLI、共享库的 `.text` 摘要均不变。公开的 `make -C software/gpgpu/linux run-models` 入口也已实际启动，通过基础计算和真实图片检测；这次入口核对不混入上述性能样本。

> **[SOURCE] 协议 8 实际结果与范围**：源码快照、组件散列、输入和实际命令随结果保存。
>
> - 公开启动入口：[run-models 实跑与 ARM64 代码段核对](../../build/gpgpu-guest-apps/launcher-v8-attempt2/result.json)。
> - 客体应用：[运行与内核记录](../../build/gpgpu-guest-apps/device-v8-attempt4/result.json)、[三轮会话](../../build/gpgpu-guest-apps/device-v8-attempt4/qwen-chat/session.json)、[原图标注](../../build/gpgpu-guest-apps/device-v8-attempt4/yolo/postprocess/annotated.jpg)、[镜像标注](../../build/gpgpu-guest-apps/device-v8-attempt4/yolo-mirror/postprocess/annotated.jpg)、[独立参考核对](../../build/gpgpu-guest-apps/cpu-reference-v8/result.json)。
> - 两模型性能：[应用与算子分解](../../build/gpgpu-guest-apps/performance-v8/performance.json)。它只汇总实际样本，不外推未测输入。
> - 原容量边界：[完整复验](../../build/gpgpu-guest-apps/regression-v8-attempt2/capacity/result.json)，与[旧版失败](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-capacity/result.json)分开保留。
> - 回归：[完整算子汇总](../../build/gpgpu-guest-apps/regression-v8-attempt2/operators-summary.json)、[GPUVM 开／关全栈](../../build/gpgpu-guest-apps/regression-v8-attempt2/core/summary.json)、[各阶段命令与结果](../../build/gpgpu-guest-apps/regression-v8-attempt2/result.json)。
> - 评审与风格：[问题处理和复验范围](../../build/gpgpu-guest-apps/code-review-v8.json)、[最终风格与代码段核对](../../build/gpgpu-guest-apps/style-final-v8-corrected/result.json)。沙箱 socket 失败与旧固件副本导致的 probe 失败另存，均不计为通过。

**[BOUNDARY]** 当前支持分别离线运行、batch=1、Qwen 容量不超过 2048、YOLO11n 固定 320×320 FP32。聊天只支持纯文本与 greedy 生成，不支持流式、工具调用或跨对话轮 KV 复用；旧离线固定程序入口不具有客体聊天的有界窗口内存保证。未验证更长上下文、实时检测或 COCO mAP。协议 7 的完整模型数值、正式性能和原始容量失败记录保留在[历史验收](#model-v7-history)，不作为当前组件的性能或通过结论。

<a id="model-eval-results"></a>

**真实模型算子评估（协议 7）。** 基线为 `17a00fd99de65c2b4fe0255c2f2d621385a26014` 加工作区改动。各阶段的代码快照、组件散列、实际命令、原始样本、错误和日志保存在 `build/gpgpu-validation/model-ops/run-20261108/`；以下目录名均相对于该位置。显存和组件升级见 [PROTOCOL_V7.md](./PROTOCOL_V7.md)。

材料固定为官方 Qwen2.5-0.5B-Instruct revision `7ae557604adf67be50417f59c2c2f167def9a775` 与用户提供的原版 YOLO11n 权重；后者 SHA-256 为 `0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1`。CPU 导出环境在 `build/gpgpu-model-eval/venv/`，依赖锁及下载来源在 `material/`。完整配置、真实文本／图像、精度、量化规则和散列以[参考清单](../../build/gpgpu-model-eval/reference-v2/manifest.json)为准；其 SHA-256 为 `9731c2d5018b870bf6c9cf5728aa966ec5a6fed31b3d029cabe0f9c59510c019`。导出耗时和 RSS 是准备成本，不是设备计算性能。

`block-full-v2` 在 GPUVM 开启、数据页 WB、共享取指、远端设备 O2 构建及块后端下，完整比较通过 250 个数值签名、18,398,447 个输出元素。139 个签名使用张量块指令，111 个使用保留的标量路径；未把元数据视图或原 FP32 对照算成设备执行。数值签名对应冻结调用清单中的 2,635 次调用，另有 225 个元数据签名和 24 个仅供原权重 FP32 对照的签名。BMM、COPY、RoPE 的非连续输入按原 stride 恢复存储，实际复制与拼接仍在设备执行。

比较保持预定的 `atol=rtol=2e-4`，COPY、CONCAT／STACK、NEAREST 按原始位比较，整数精确比较，失败值为零。全组最大绝对误差为 0.00390625，最大相对 L2 为约 `1.4011e-6`；大绝对误差出现在数值较大的 BMM 输出，仍满足逐值绝对／相对容差组合，未因失败放宽阈值。独立量化参考的相对 L2 为约 `6.20e-5～1.35e-2`，它衡量相同真实 FP32 输入上的 W8 与原权重算子差异，不是完整量化网络的生成质量。

**执行与分块的实测变化。** 初始逐 Lane 组在 O0 下保留三次样本；扩展代表组、共享取指组和 O2 组使用同一冻结数据。以完整 prefill 投影 X[128,896]、W[128,896] 为例，`lane-broad-v2` 为 24.321 s，`warp-broad-v2` 为 6.570 s，`o2-warp-extended-v2` 为 2.521 s。剖析确认小 W8 投影的逻辑 Lane 指令数不变，而实际取指／字段译码从 1,038,475 次降到 32,463 次；数据请求量不变。共享内容只含静态字段，类别合法性和执行仍逐 Lane 处理。

同一 O2 设备上，`scalar-max-extended-v2` 把派发容量取自共享线程上限，完整 prefill 投影为 2.336 s、全部 lm_head 为 21.736 s、YOLO 首层卷积为 2.697 s；`block-full-v2` 分别为 0.02575 s、0.70948 s、0.04179 s。DFL 仍使用标量复合步骤，大分块后约 0.480 s，块后端没有把它伪计成矩阵加速。`block-profile-v2` 与正式计时分开：prefill 投影实际完成 14,680,064 MAC，使用 1,792 个安全 SIMD 块、无 SoftFloat 回退；布局计数也与真实完整元素数对应。按每 MAC 计 2 FLOP，该投影的完整算子有效吞吐约 1.14 GFLOP/s；此口径不另计反量化、偏置和整数索引，也不是实体 GPU 峰值。

`tile-default-v2`、`tile-max-v2`、`tile-max-uncached-v2` 和 `tile-max-off-v2` 各选同一组真实算子，预热一次、正式三次。DFL 中位数由 2.115 s 降到 0.489 s，代表复制由 0.830 s 降到 0.188 s。该复制在 on+UNCACHED 下约 0.224 s，off 约 14.409 s。off 的数据均为 DEVICE，而 on+UNCACHED 没有改变 RT 自身参数页的 WB 属性：每 Lane 读取 Host 参数在 off 下会反复经过 Outbound／socket。因此该组不能作为纯地址翻译开销对照，也不支持靠删除少量检查或缓存维护来选择 off；本轮未增加 off 专用快路径。

执行升级采用有界矩阵／布局指令，而不扩展跨轮预译码缓存或计算核 TCG/JIT。共享取指已实际减少约 32 倍取指与字段译码；O2 代表组的旧估算中，prefill 主要耗在线性投影，长上下文 decode 则有大量复制。继续缓存指令不能消除这些逐元素索引、数据请求和浮点操作；TCG/JIT 还需要代码、别名、映射与权限的失效约定。本轮让通用块指令合并确实连续的访存并跨独立输出使用 SIMD，保留原用户访存和故障路径；这是一项显式指令扩展，不称为普通译码优化。

**模型时间与空间预算。** `block-full-v2/estimate.json` 全部使用已测完整数值签名，算子和约为：prefill128 150.44 s；decode 的既有上下文 128／512／2048 分别为 11.57／13.61／20.51 s；YOLO 320×320 单图为 18.81 s。加必要输入和最终输出 COPY 的常驻估算约为 150.48、11.60／13.65／20.55、18.90 s；冷启动准备外推后约为 400.05、215.07／229.29／284.93、31.61 s。这里的冷启动指设备数据尚未驻留，未强制清空宿主文件缓存，也不包含客体启动或材料散列核对。全签名组每例只有一个正式样本，不能把相同上下界称作置信区间或稳定性证明；旧代表组的未测签名与相容类外推均在各自估算文件中显式记录。

预算统计完整去重权重：Qwen 约 496 MB，YOLO 约 10.6 MB。保留全部数值输出到网络结束、不复用输出，仅复用最大 scratch 时，Qwen 各场景规划数据量约 529～964 MB，YOLO 约 76.9 MB；页表、对齐碎片和控制对象不含在这些数值内。这是独立回放阶段的空间规划，完整网络采用的激活寿命复用与实际分配见[完整模型运行记录](#model-e2e-results)。运行记录中的两套 QEMU 分别采样 PSS，不能把共享映射的 RSS 简单相加；全签名组各次采样均无交换。

独立算子估算用于安排运行预算。完整离线推理的实际输出、逐层数值、常驻分配和请求耗时以[完整模型运行记录](#model-e2e-results)为准；交互生成与实时检测仍未验证。

大对象准备也单列记录：decode embedding 上传 136,742,408 B，使用 64 KiB staging、2,089 个块，覆盖大于旧单 BO 上限及非整块尾部。`vram_clear_dma=1` 使用驱动私有零源加速未发布 BO 的清零，默认仍关闭；页表页保持锁内 BAR 清零。按叶表范围复用 PTE 定位后，同一大表的 `buffer_setup_ns` 从约 23.37 s 降到 9.33 s；实际分配清零约 5.65 s、文件读取约 33.48 s、H2D 约 6.39 s。数据读取仍是冷启动的重要成本。DMA 清零的正常路径、非零偏移、尾块及同址复用后全零已核对，超时／移除竞争尚未做定向注入。

**同客体双进程竞争。** `competition-*` 使用 Qwen decode128 的完整 lm_head 与 YOLO DFL，每例预热一次、正式三次。两边是同一客体内两个不同 PID，分别使用独立用户队列，输出、正常回收与内核日志均通过。分块 65,536 时，单跑中位数分别为 702.8／482.0 ms，共同运行分别为 769.4／1197.7 ms，slowdown 为 1.095／2.485。Qwen 的最长 Dispatch 约 699 ms，YOLO 的已观测最长排队下界约 701 ms，符合 CP 要先完成整个 Dispatch 的行为。

把分块降到 1,024 后，单跑分别为 729.5／2088.7 ms，共同运行分别为 747.4／2245.5 ms，slowdown 为 1.025／1.075；YOLO 最长排队下界降到约 155 ms，但宿主共同区间由 4.943 s 增到 12.072 s。较短 Dispatch 提供了更多换队列机会，更多派发／参数上传／同步也提高了总开销，不能只按 slowdown 选择小分块。大分块下 YOLO 三次样本为约 1206.0、1197.7、499.3 ms，末段因 Qwen 先完成而接近单跑；不把这一有限重放称为稳态双模型运行。

生命周期配对使用同一设备进程的宿主时钟，分别保留 Doorbell、Dispatch 和 CP 推进 R 的时刻。排队值只报告提醒到开始的已观测下界；小分块单跑还出现一次 CP 轮询先于 Doorbell，记录为不可由提醒推算排队，未强行记零。原始样本、PID、队列归属、资源与全部事件保存在下方竞争来源索引中。

独立的 `competition-profile-*` 每进程只取一个正式样本、不预热，用于资源诊断而非性能成绩。单跑与双进程的逻辑指令、实际取指、数据请求和字节数相同；Qwen 的 TLB 未命中均为 75,429 次，YOLO 从 455 次变为 460 次，两边 dcache 未命中都没有整体增长。本组合观察到的主要延迟变化是整 Dispatch 排队，没有看到明显的段内缓存计数恶化；这不说明其他负载没有缓存竞争。驱动锁等待未单独计量，不能给出精确占比。

固件、默认与 O2 两套 QEMU 构建通过，汇编器入口通过 8 个编码用例；标准指令与自定义指令均已同 GNU as 逐字节核对，自定义指令使用 GNU `.insn r4` 作独立参考。完整 O2 QTest 为寄存器 7/7、CP 14/14，包含共享 Lane 顺序与 W8 的 K 分块／末块偏置／行列尾部。标准 `test-gpgpu-stack` 在 GPUVM 开、关下的计算与生命周期示例均比较通过，内核日志检查通过。局部字节存储的定向核对另验证特殊浮点、宿主环境恢复、INT64 转换、空洞和后行故障；它不替代真实 GPUVM 验证。所有 socket 限制、早期高位地址截断和编译／测试修正前的失败记录均保留，未记成模型数值通过。

独立代码评审确认的六项问题均已修复：共享头的增量构建依赖、`copy_` 源输入、估算样本隔离、实际材料散列核对、退出码原子发布及手册版本依据。定向核对拒绝被改写的 job／原数组／打包数据，也拒绝仅含竞争样本或混入竞争样本的单模型估算；重新准备的全部真实 job 与打包数据逐字节不变，原单模型估算不变。评审后复跑完整 QTest、标量／块两条 `copy_` 路径和两种分块的双进程回放，输出、正常回收与内核日志均通过。临时核对材料留在 `build/`，不增加长期测试框架。

最终用户态 `all check` 与结构体、接口的 `kernel-doc` 通过。Linux `checkpatch` 为 0 errors、2 warnings，分别是新增文件的 MAINTAINERS 提示与 `noinline` 属性的宏建议；QEMU 检查器仍对两处单语句 `for` 要求花括号，与本仓库规则相反，按仓库约定保留。原始诊断和退出码均保存，不把这些风格检查写成全通过。文档链接只完成路径、锚点与行号的静态核对，未实际点击 Windows VS Code 预览。

> **[SOURCE] 原始记录**
>
> - 材料与覆盖：[参考清单](../../build/gpgpu-model-eval/reference-v2/manifest.json)、[回放参数](../../build/gpgpu-model-eval/jobs-v1/jobs.json)、[量化误差汇总](../../build/gpgpu-validation/model-ops/run-20261108/initial/coverage-quantization.json)。
> - 阶段结果：[逐 Lane](../../build/gpgpu-validation/model-ops/run-20261108/lane-broad-v2/result.json)、[共享取指](../../build/gpgpu-validation/model-ops/run-20261108/warp-broad-v2/result.json)、[O2 代表组](../../build/gpgpu-validation/model-ops/run-20261108/o2-warp-extended-v2/result.json)、[标量大分块](../../build/gpgpu-validation/model-ops/run-20261108/scalar-max-extended-v2/result.json)、[全部签名](../../build/gpgpu-validation/model-ops/run-20261108/block-full-v2/result.json)及[模型预算](../../build/gpgpu-validation/model-ops/run-20261108/block-full-v2/estimate.json)。
> - 执行与访存：[共享剖析](../../build/gpgpu-validation/model-ops/run-20261108/profile-warp-v2/result.json)、[块剖析](../../build/gpgpu-validation/model-ops/run-20261108/block-profile-v2/result.json)、[分块与 GPUVM 对照](../../build/gpgpu-validation/model-ops/run-20261108/initial/tile-vm-summary.json)。
> - 竞争：[汇总](../../build/gpgpu-validation/model-ops/run-20261108/initial/competition-summary.json)、[大分块单跑](../../build/gpgpu-validation/model-ops/run-20261108/competition-solo-max-v2/result.json)、[大分块双进程](../../build/gpgpu-validation/model-ops/run-20261108/competition-dual-max-v2/result.json)、[小分块单跑](../../build/gpgpu-validation/model-ops/run-20261108/competition-solo-small-v2/result.json)、[小分块双进程](../../build/gpgpu-validation/model-ops/run-20261108/competition-dual-small-v2/result.json)、[缓存计数汇总](../../build/gpgpu-validation/model-ops/run-20261108/initial/competition-cache-summary.json)及[单跑](../../build/gpgpu-validation/model-ops/run-20261108/competition-profile-solo-v2/result.json)／[双进程](../../build/gpgpu-validation/model-ops/run-20261108/competition-profile-dual-v2/result.json)剖析。
> - 回归与边界：[完整 QTest](../../build/gpgpu-validation/model-ops/run-20261108/initial/block-full-qtest-v2.json)、[全栈](../../build/gpgpu-validation/model-ops/run-20261108/final-stack-v2/summary.json)、[浮点与块访存定向核对](../../build/gpgpu-validation/model-ops/run-20261108/initial/tensor-directed-v2.log)、[同址清零](../../build/gpgpu-validation/model-ops/run-20261108/directed-zero/run-v2/result.json)、[GNU R4 独立编码核对](../../build/gpgpu-validation/model-ops/run-20261108/tensor-encoding-v2/result.json)。
> - 评审与复验：[六项问题及修复依据](../../build/gpgpu-validation/model-ops/run-20261108/initial/independent-review.json)、[主机定向核对](../../build/gpgpu-validation/model-ops/run-20261108/review-host-v1/result.json)、[QTest 与设备复验命令](../../build/gpgpu-validation/model-ops/run-20261108/initial/review-device-v2.json)、[Linux 风格与 kernel-doc](../../build/gpgpu-validation/model-ops/run-20261108/initial/final-style-v2.json)、[QEMU 风格原始诊断](../../build/gpgpu-validation/model-ops/run-20261108/initial/final-qemu-checkpatch-v5.log)。

<a id="model-eval-commands"></a>

复用冻结材料时无需再次导出。`run` 在启动客体前核对所选 job、原始数组与对应 stride 打包数据的 SHA-256，拒绝内容变化；核对成本另存 `material_verification`，不计入算子时间，但读取会预热宿主文件缓存。`estimate` 只接受单进程通过记录，双进程等待不能混入单模型成本。

`run --output` 必须指定新的结果目录，不能覆盖已有样本；下面的 `--tile` 取当前共享能力上限，调整分块时同时保留实际命令和结果。默认 QEMU 配置仍为 O0；独立 O2 构建的参数记录在 `initial/qemu-o2-build.json`，本轮远端设备显式使用该二进制，A57 Host 的 QEMU 保持默认构建。

```sh
# 查看已冻结的材料和覆盖，不运行模型。
python3 -I software/gpgpu/scripts/gpgpu_model_eval.py inventory \
  build/gpgpu-model-eval/reference-v2/manifest.json

# 如需重新生成 job，使用新的目录；原始参考数据保持不变。
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_eval.py prepare \
  build/gpgpu-model-eval/reference-v2/manifest.json --output build/gpgpu-model-eval/jobs-new

# 在同一 1 GiB 客体逐例回放；不用 --case-regex 时包含全部数值签名。
python3 -I software/gpgpu/scripts/gpgpu_model_eval.py run build/gpgpu-model-eval/jobs-v1 \
  --output build/gpgpu-validation/model-ops/user-run-new \
  --device-qemu build/arm64-qemu-o2/qemu-system-riscv64 \
  --execution warp --backend block --tile 65536 --driver-param vram_clear_dma=1 \
  --device-arg=-global --device-arg=gpgpu.tensor_native=on

# 只接受通过且配置、组件和冻结清单一致的运行。
python3 -I software/gpgpu/scripts/gpgpu_model_eval.py estimate \
  build/gpgpu-model-eval/reference-v2/manifest.json \
  build/gpgpu-validation/model-ops/user-run-new/result.json \
  --output build/gpgpu-validation/model-ops/user-run-new/estimate.json
```

**[BOUNDARY]** 上述数据是独立算子回放与按真实调用次数汇总的模型估算，不是完整网络推理。KV 来自 CPU 参考，常驻估算假设上下文已驻留；冷启动未计 tokenizer、预处理、mask 构造、采样、生成控制、NMS、界面或未实现的融合。双进程只覆盖上述有限回放组合。完整模型质量、全部对象同时分配、BO 清零 DMA 超时／移除竞争和非 x86 宿主实际构建仍未验证。

<a id="main-merge-validation"></a>

**合并后的功能复验（协议 7）。** `a7998a8` 与 `2a3dedf` 的合并工作区通过用户态 `all check`、GPUVM 开／关标准全栈、代表性真实算子和同客体双进程回放。启用 `vram_clear_dma=1` 的 GPUVM 开／关正常恢复均通过：旧任务失败、旧连接可清理，新连接计算正确。独立静态复核未发现 DMA 清零与设备恢复交集中的合并语义问题。这里记录功能回归，不替换前述性能样本或模型估算；补充故障注入仅以各模式已完成的原始结果为准，不把未完成模式计作通过。

> **[SOURCE]** 合并复验材料位于 `build/gpgpu-validation/model-ops/main-merge-a7998a8/`：[实际命令与退出状态](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/verification-v2.json)、[全栈结果](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/core-v2/summary.json)、[代表算子](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/representative-v2/result.json)、[双进程](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/dual-v2/result.json)、[恢复原始输出](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/recovery-v2.log)、[独立静态复核](../../build/gpgpu-validation/model-ops/main-merge-a7998a8/integration-review.json)。各次复验使用新目录，先前结果与失败日志均保留。

以下是协议 6 的既有记录，不替代协议 7 的真实算子验证。

<a id="device-recovery-validation"></a>

**整设备中止与复位恢复的验证。** 代码基线为 `17a00fd`，加 `dev/device-recovery` 工作树的未提交改动；改动源码的 SHA-256 见本地源码清单。行为约定见 [CP/MEC 规格第 7.5 节](<../../docs/CP_MEC 第一版规格.md#spec-device-recovery>)。实际运行命令如下，最后一条依赖本地保存的一次性核对材料，不是仓库的常驻测试入口：

```sh
make -f Makefile.gpgpu test-gpgpu-stack
make -C software/gpgpu BUILD_DIR="$PWD/build/gpgpu-validation/device-recovery/host-stack" all check
python3 build/gpgpu-validation/device-recovery/directed/run.py "$PWD" on off failure cancel uncertain
```

- 驱动使用现有交叉工具链以 `W=1` 编译，无警告；用户库和示例以 `-Wall -Wextra -Werror` 构建通过，教学汇编器 7/7。Linux `checkpatch.pl` 对 C 差异报 0 errors、0 warnings，改动头文件的 `kernel-doc -none` 通过。
- Linux 全栈复用 `6.18.57-0-gpgpu-virt`、A57、1 GiB RAM、IOMMU=none 与双进程设备。GPUVM 开、关均输出 `GPGPU DRM ABI=6`，向量加法、矩阵乘法、ReLU 和生命周期示例两次派发全部比较通过；客体退出码均为 0，内核日志检查通过。组件散列和实际比较数量保存在两种模式的 `result.json`。
- `on`、`off` 用正式驱动终止运行中的长任务：旧等待失败，事件报告 `GPGPU_EVENT_ERROR_RESET`，Signal 没有被补写为完成，旧会话新建对象返回 `-ECANCELED`。旧队列、事件、运行时和连接能够清理；新连接完成向量加法，空闲时再次恢复后仍能计算。
- `failure` 在 GPUVM 开启时使用临时驱动桩：丢弃一次 DMA 完成中断制造 COPY 超时，再让第一次恢复直接返回“复位未确认”。恢复返回 `-EIO` 后，超时 COPY 的对象不能解绑，旧队列不能卸载，新连接被拒绝；再次恢复成功后，两端对象与旧连接可以回收，新连接计算正确。
- `cancel` 在管理请求等待期间恢复设备；`uncertain` 先让建队列请求超时，再注入一次复位未确认。在 `uncertain` 场景中，用户库在复位前及恢复失败后都拒绝关闭结果未知的旧连接，驱动的 INFO 不报告旧访问已结束；再次恢复确认后才输出 `GPGPU_INFO_SESSION_RESET`。两种场景的旧连接最终都能关闭，同一进程重新 Open 后完成向量加法。
- 五种定向模式均返回 0，客体内核日志检查通过。临时模块的客体编译提示编译器版本与内核构建时不同；9p 共享目录构建还出现时钟偏差提示。构建均完成，上述退出状态与计算比较通过。

现有全栈入口只跑正常计算，不能验证主动终止、未确认时的保留及再次恢复，因此定向程序和注入桩只保存在 `build/`，不增加常驻脚本或测试用例。

> **[SOURCE]** 本地运行记录：[改动源码清单](../../build/gpgpu-validation/device-recovery/source-manifest.json)、[驱动额外警告编译](../../build/gpgpu-validation/device-recovery/module-w1.log)、[用户态构建与汇编器](../../build/gpgpu-validation/device-recovery/userspace-build-check.log)、[风格检查](../../build/gpgpu-validation/device-recovery/style.log)、[全栈输出](../../build/gpgpu-validation/device-recovery/stack-final.log)、[GPUVM 开启结果](../../build/gpgpu-validation/device-recovery/core/on/result.json)、[GPUVM 关闭结果](../../build/gpgpu-validation/device-recovery/core/off/result.json)、[恢复定向输出](../../build/gpgpu-validation/device-recovery/directed/final-run.log)。初版运行记录仍保留在同目录的 `stack.log` 与 `directed/run.log`；`build/` 不纳入 Git。

**[BOUNDARY]** 最终运行需要创建 Unix socket，沙箱内启动被拒绝后在沙箱外运行通过。未重跑 QTest、SMMUv3 或基础环境引导。设备模型和固件没有行为改动，新增的用户接口标志未改变结构体布局。注入桩模拟的是驱动“未确认复位”，没有制造设备后端的真实回写失败，也未注入重新启动时的固件装载或 HIQ 握手失败。设备移除与文件关闭的并发回收修复经过静态复核，未专门做并发运行注入。

**统一访存入口与页缓存属性的验证。** 本轮在提交 `18504d0` 加未提交改动的代码上运行，命令与日志归档在 `build/gpgpu-validation/unified-mem-access/`，不会被下一次运行覆盖：

- `make -f Makefile.gpgpu build` 后运行 `test-gpgpu-qtest`：寄存器 QTest 7/7，CP QTest 12/12。GPUVM 开启的 CP 用例把 Ring、进度、参数和 Signal 映射为 UNCACHED 页，缓存专项用例走 WB 映射。与缓存属性相关的用例各查一件事：
  - `/gpgpu/regs/dcache-commands`：维护命令的编码规则；
  - `/gpgpu/cp/dispatch`：GPUVM 关闭时完成派发，dcache 的命中与未命中计数不变；
  - `/gpgpu/cp/gpuvm-system`：中间项带 UNCACHED 报页表错误；
  - `/gpgpu/cp/gpuvm-cached-permission`：已缓存的行不能绕过只读权限；
  - `/gpgpu/cp/dcache-shared-line`：CP 与执行器经 WB 页共用同一物理行，期望的未命中与回写次数由行大小推导；
  - `/gpgpu/cp/dcache-partial-write`：WB Host 页上的部分写不改变同一行的其余字节；
  - `/gpgpu/cp/dcache-invalidate`：CPU 改写 WB 页上的参数并失效后，执行器读到新值；
  - `/gpgpu/cp/dcache-bar-write`：BAR 写入按 DEVICE 处理，使已缓存的描述对象行失效。
- `make -C software/gpgpu all check` 构建用户库、示例和 Kernel，教学汇编器 7/7。驱动以宿主机交叉编译器、`W=1` 编译，无警告。Linux `checkpatch.pl` 对已跟踪文件的 C 代码差异报 0 errors、0 warnings；新文件中 `access.c` 无告警，`access.h` 只有沿用 QEMU 设备写法的 typedef 警告，`backend.h` 有一条 `GPGPUState *s` 的空格误报，与现有 `hw/gpgpu/gpgpu.h` 第 46 行相同；`kernel-doc -none` 通过。
- Linux 全栈 GPUVM 开、关都输出 `GPGPU DRM ABI=6`，完成向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513，生命周期示例两次派发各 17,003/17,003；客体退出码为 0，内核日志检查通过。两种模式的组件 SHA-256 相同：固件 `33db7c502a168326de0313580169c8143a6bb944f855de6a80ef70bca4017472`，模块 `a837205edde50002a14190eddaf5b3b4701c2a5ec83262af0ed3ca5ad7fb90e6`，示例 `02122d7a1f6ce806cdac60a0c151fce84bc3f1b48f6fbc3a95495542cfa56eb4`。
- 一次性变异核对：GPUVM 关闭时的用户访问改用 WB、DEVICE 访问前不处理重叠行，分别使 `dispatch`、`dcache-bar-write` 失败；恢复源码重建后通过。
- 一次性定向核对释放时的存储保留：在 GPUVM 关闭的全栈环境中，用临时驱动让下一次“回写并失效”命令按失败返回，VRAM 与 Host 各核对一页。正常释放后再分配，两类存储都拿回同一页（VRAM `0x1000`，Host `0xc3e5_c000`）；维护失败后这一页被保留，再分配分别得到 `0x3000` 与 `0xc3ba_1000`，内核日志各有一条保留记录。随后卸载驱动，设备释放时归还了保留的 Host 存储；客体内核的 `gpu_buddy_fini()` 遇到未归还的块会触发 `WARN_ON`，内核日志检查通过，说明保留的 VRAM 块也随分配器归还。临时驱动和测试程序只放在本轮记录的 `directed/` 下，不纳入仓库。

> **[SOURCE]** 本地运行记录：[命令、变异核对与边界](../../build/gpgpu-validation/unified-mem-access/commands.txt)、[QTest 输出](../../build/gpgpu-validation/unified-mem-access/qtest.log)、[用户态构建与汇编器](../../build/gpgpu-validation/unified-mem-access/userspace-build-check.log)、[驱动额外警告编译](../../build/gpgpu-validation/unified-mem-access/module-w1.log)、[风格检查](../../build/gpgpu-validation/unified-mem-access/style.log)、[全栈输出](../../build/gpgpu-validation/unified-mem-access/stack.log)、[两种模式汇总](../../build/gpgpu-validation/unified-mem-access/core/summary.json)、[定向核对输出](../../build/gpgpu-validation/unified-mem-access/directed/run.log)。上一轮 dcache 第一版的记录保留在 `build/gpgpu-validation/dcache-review/`。

**[BOUNDARY]** QTest 与全栈需要创建 Unix socket，在沙箱外运行。本轮全栈使用 IOMMU=none，没有运行 SMMUv3、基础环境引导回归和下列失败路径核对。回写失败和只失效时遇到脏行没有专门用例，也没有注入后备存储故障，所以冷复位丢弃脏行、驱动记录 `DCACHE_FAULT` 两条路径没有运行验证。释放存储时的保留由定向核对在驱动侧模拟维护失败验证，设备没有真正回写失败；设备移除后、确认停止前释放存储的保留路径也没有运行验证。

以下两组记录在协议版本 5 时取得，本轮没有重跑，保留作对照。

**对象分配优化的验证（协议版本 5）。** 当时的分配实现对应 `gpgpu_bo.c` 的 Git blob `a8cc6777369a02a537f135e4c4f8ad8cb82ca16c` 和 `gpgpu_vram.c` 的 Git blob `53284e4653806b4555c43b5c3ad7d0726d629f1c`。运行命令与日志归档在 `build/gpgpu-validation/d3-allocation/`：

- 驱动以宿主机交叉编译器、`W=1` 编译通过，无警告；Linux `checkpatch.pl` 和 `kernel-doc -none` 通过。
- `make -f Makefile.gpgpu test-gpgpu-stack` 沿用上述开发环境。GPUVM 开、关的向量加法、矩阵乘法、ReLU 和生命周期示例全部比较通过，客体退出码均为 0，内核日志检查通过。最终源码在客体中重编后的模块与两种全栈记录使用的模块 SHA-256 一致，均为 `9bad8e58e0f0164aa95b8b5344895a566909ff6da7a32f3c53b8d480599420b5`。
- 一次性定向核对直接编译包含产品 `gpgpu_vram.c`，调用客体内核的 GPU_BUDDY。17 页请求实际保留 69,632 字节，起点为 `0x2_0000`，满足 128 KiB 自然对齐；清零量也是 69,632 字节。尾部从 `0x3_1000` 开始的 15 页可立即重新分配，尾部预置内容未被清零。全部归还后，可用容量恢复到分配前的 62,910,464 字节。

> **[SOURCE]** 本地运行记录：[命令与边界](../../build/gpgpu-validation/d3-allocation/commands.txt)、[驱动额外警告编译](../../build/gpgpu-validation/d3-allocation/module-w1.log)、[全栈输出](../../build/gpgpu-validation/d3-allocation/stack-validation.log)、[两种模式汇总](../../build/gpgpu-validation/d3-allocation/core/summary.json)、[定向核对结果](../../build/gpgpu-validation/d3-allocation/directed/result.json)。

**[BOUNDARY]** 定向核对只把 `memset_io` 替换为普通内存清零，不单独覆盖 BAR2 通道或 Host DMA 实际物理占用；设备的 BAR2 和 Host DMA 访问另由全栈覆盖。首次全栈启动被沙箱的 Unix socket 权限阻止，允许 socket 后运行通过。定向验证桩的首次运行缺少预留块链表初始化，补齐桩的初始化后通过；失败日志保留在定向记录中。设备模型、固件及协议没有变化，本次未重跑 QTest、SMMUv3 或下列旧版失败路径检查。

以下保留的全栈与失败路径结果对应提交 `ec9a9c0`、协议版本 5，包括 MAP 表页不足、队列映射与卸载被明确拒绝、COPY 未启动时的资源收尾，以及 VRAM 分段清零。设备模型与 CP 固件的执行逻辑仍与 `e2db1a2` 相同。版本 5 首次验证记录在 `build/gpgpu-validation/protocol-v5/`，`ec9a9c0` 的最终记录在 `build/gpgpu-validation/d1-d4-d5-final/`，都不会被下一次运行覆盖：

- QTest 13/13 与基础环境引导回归 6/6 在与提交 `e2db1a2` 相同的代码上运行。之后的行为改动只涉及示例程序、驱动和用户库，这两个入口不执行这些代码，没有重跑；共享协议头仅修改注释，协议值与布局不变。教学汇编器 7/7 在 `dca7909` 上重跑。修正后的驱动另用宿主机交叉编译器以 `W=1` 编译，无警告；用户库与示例以 `-Wall -Wextra -Werror` 构建通过。
- Linux 全栈复用 `6.18.57-0-gpgpu-virt` 开发环境，驱动模块与用户库在客体中按修正后的源码重编。GPUVM 开、关都完成向量加法 17,003/17,003、矩阵乘法 437/437、ReLU 513/513，生命周期示例两次派发各 17,003/17,003，客体退出码为 0；示例输出 `GPGPU DRM ABI=5`，GPUVM 状态和客体内核日志检查通过。
- 两种模式的组件 SHA-256 相同：固件 `d05b805d666df7dac5fc4bc9c3b56fb7235a55650cc7742dcd4a90a6f4d1c9f2`，模块 `e6543456d1cccb5e449b0b96b685628d6d7cf7321759db2e0b442dbaa6982ae7`，示例 `a97bc5f7efd0c216b1fa66a03305b2f48617480011b1a26f6babaeefd4b541cc`。

失败路径另用一次性程序在同一开发环境中核对，程序与日志一并保存在本轮记录的 `directed/` 下，不纳入仓库。三次运行均返回 0，客体内核日志检查都通过：

- MAP 表页不足（GPUVM 开启）：先用未绑定的 VRAM 对象占满 VRAM。新会话第一次 MAP 时根表分配失败，返回 `-ENOSPC`；已有会话把 Host 对象映射到需要三张新表的地址，连续两次都返回资源不足，而不是“暂不可用”。释放 88 KiB 后，两个会话都在原会话中重试成功；GPU 把向量加法结果写到重试得到的地址，核对通过，连接正常关闭。
- 卸载被 CP 拒绝（GPUVM 开、关各一次）：另一进程约 2 秒的矩阵乘法占住 CP 时卸载队列，并在 CP 处理卸载前再发布一个包。CP 返回结果码 6，驱动报告忙，队列保持 READY；这个包完成、队列排空后重试卸载成功。随后在另一个 VMID 仍被占用时，第三个进程建队列并完成计算。
- 建队列被 CP 拒绝（GPUVM 开启）：进度对象只有 GPU 写权限，CP 装载时读 W 失败。用户库收到确定的参数错误；同一会话改用可读写的进度对象后建队列、卸载成功，Ring 和两个进度对象都能释放，连接正常关闭。
- COPY 未启动时的计数：在正式源码上加一个只丢弃一次 DMA 完成中断的临时桩。COPY A 超时，返回 `-ETIMEDOUT`；随后的 COPY B 未启动，返回 `-EIO`。关闭 B 的句柄后，它的 4 MiB VRAM 可以重新分配；关闭 A 的句柄后存储仍然保留。表页不足的核对与这项在同一次运行中使用该桩模块，桩不经过 MAP 路径。
- VRAM 分段清零：占满 VRAM 时使用了多个 4 MiB 对象，COPY B 回收后也重新分配了同样大小的对象，均完成清零，未出现 soft lockup。分段之间只让出 CPU，清零期间仍持有设备锁。

GPUVM 开启时，本轮全栈中生命周期示例打印的 X、Y、Z 地址为 `0x0000_ffff_84f1_d000`、`0x0000_ffff_84f0_c000`、`0x0000_ffff_84ef_a000`。这些值是库从 `mmap` 取得的 CPU 地址，每次运行可能不同；本次四级索引的 L0、L1、L2 依次为 `0x1ff`、`0x1fe`、`0x27`，L3 分别为 `0x11d`、`0x10c`、`0xfa`，所以全栈的翻译经过了 L0 索引非零的路径。GPUVM 关闭时，三个地址是 VRAM 物理偏移 `0x2_0000`、`0x6_0000`、`0x4_0000`。QTest 的 GPUVM 用例另以 `0x0000_7fff_0000_0000` 为基址覆盖同一条路径。

> **[SOURCE]** 本地运行记录：[QTest 输出](../../build/gpgpu-validation/protocol-v5/qtest-build.log)、[用户库与汇编器](../../build/gpgpu-validation/protocol-v5/userspace-build-check.log)、[引导回归](../../build/gpgpu-validation/protocol-v5/bootstrap-check.log)、[全栈输出](../../build/gpgpu-validation/protocol-v5/stack-validation.log)、[两种模式汇总](../../build/gpgpu-validation/protocol-v5/core/summary.json)；本轮的[全栈输出](../../build/gpgpu-validation/d1-d4-d5-final/stack-validation.log)、[两种模式汇总](../../build/gpgpu-validation/d1-d4-d5-final/core/summary.json)与[定向核对输出](../../build/gpgpu-validation/d1-d4-d5-final/directed/run.log)。版本 4 的记录保留在 `build/gpgpu-validation/vmid-residency/`。`build/` 产物不纳入 Git。

**[BOUNDARY]** QTest 与全栈需要创建 Unix socket，在禁止 socket 的沙箱中运行会失败。上述结果不覆盖从零引导开发环境与 SMMUv3；失败收尾只核对了各自的核心结果，没有在修正前的代码上对照运行。VRAM 分段清零消除了本次核对中的 soft lockup，但没有减少经 BAR2 的写入量；双进程下大对象清零仍慢，其他 ioctl 与 IH 处理仍须等待设备锁。

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

<a id="model-v7-history"></a>

### 6.5 完整模型自动验收（协议 7，历史）

**旧版完整模型自动验收（协议 7，历史基线）。** 以下数值、性能与回归只描述 `reviewed-v1` 保存的组件，不是协议 8 客体应用成绩。旧入口为 `scripts/gpgpu_model_run.py` 的 `prepare/run/report`，保留用于独立参考比较与历史复现。每次准备和运行都要求新的输出目录；运行包只读挂载，CPU 参考不进入客体。

`prepare` 从真实文本或图像产生本次输入，生成完整模型程序及独立 CPU 参考。Qwen 的参考从 W8 embedding 开始，使用同一份 `q/scales` 逐层传播并连续维护 KV；`--original-reference` 另做串行的原 FP32 质量对照。模型包和参考都记录实际解析的权重清单散列，`run` 拒绝来源缺失或不一致的参考。运行包固定本次输入、执行形状与生成预算；每个请求重新预处理并核对输入散列，客体从只读包重新上传对应数据。更换文本或图像时需重新 `prepare`，权重 raw 文件仍可复用。

默认包不保留诊断中间值。需要检查 hidden、KV 或 raw heads 时，`prepare` 和 `run` 都要加 `--checkpoints`；两端设置不一致会在启动设备前被拒绝。诊断包即使不需要重新生成 CPU 参考，也必须单独准备，可用 `--no-reference`，运行时再用 `--reference` 指向同源参考。

`run` 在同一设备会话内保留权重和 BO，每请求重新上传输入。WSL 单调时钟记录真实预处理、输入读取与 H2D、全部设备前向、必要 D2H、argmax 或 NMS，以及文本／检测结果保存。客体先写本次 `sample.json` 并发布 `done`，宿主完成必要后处理后截止请求计时；参考比较另计。宿主随后发布 `release`，客体才输出完整节点遥测，全部落盘并发布 `idle` 后才开始下一请求，历史日志解析不进入请求时间。`--case-timeout` 保护请求与宿主屏障，`--timeout-ms` 只限制单次 Dispatch；两者都不是性能通过门槛。

```sh
# 使用现有独立环境；prepare 默认同时生成完整 CPU 参考。
# qwen-user-new、yolo-user-new 和结果目录都必须尚不存在。
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_run.py prepare \
  --model qwen --prompt '请只回复“你好”。' \
  --output build/gpgpu-model-eval/qwen-user-new
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_run.py prepare \
  --model yolo --image build/gpgpu-model-eval/material/yolo-input-complete/bus.jpg \
  --output build/gpgpu-model-eval/yolo-user-new

build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_run.py run \
  --package build/gpgpu-model-eval/qwen-user-new \
  --output build/gpgpu-validation/model-e2e/qwen-user-new
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_run.py run \
  --package build/gpgpu-model-eval/yolo-user-new \
  --output build/gpgpu-validation/model-e2e/yolo-user-new
build/gpgpu-model-eval/venv/bin/python -I software/gpgpu/scripts/gpgpu_model_run.py report \
  build/gpgpu-validation/model-e2e/qwen-user-new \
  build/gpgpu-validation/model-e2e/yolo-user-new \
  --output build/gpgpu-validation/model-e2e/user-report-new.json
```

结果目录的 `result.json` 保存实际命令、源码与组件散列、每个请求的验证和耗时。`inference/sample-*/generated.txt`、`generated.json` 是 Qwen 的真实设备生成文本与 token；YOLO 的 `postprocess/detections.json`、`annotated.jpg` 保存设备检测张量经过 CPU NMS 后的原图坐标结果。模型包、程序与权重的散列核对另计，不称为设备计算性能。

整网数值核对使用 GPUVM 开启、WB 数据页、O2 远端设备、共享取指、块后端和 `tensor_native=on`，开启检查点回读；这些诊断样本不作为正式性能成绩。

- YOLO 的三个 raw head、完整 `[1,84,2100]` pre-NMS 及最终检测均满足既定比较标准；真实 `bus.jpg` 得到一辆公交车和四个人。pre-NMS 最大相对 L2 约 `2.75e-7`，最终框最大绝对差约 `0.000122` 原图像素，类别与置信度核对通过。
- Qwen 的真实指令为“请用一句话介绍你自己。”，prompt 为 35 tokens，以 16-token 块 prefill 并继续生成 8 tokens。设备与完整 W8 参考的 token 完全相同，输出“我是来自阿里云的超大规模语言”，因预算结束而不是 EOS 停止。logits、三处层 hidden 和对应 KV 的检查全部满足 `atol=rtol=2e-4`，这些检查的最大相对 L2 约 `8.12e-6`；最后一个生成 token 没有再送入前向，KV 保存 42 个已执行位置。

额外文本请求 `请只回复“你好”。` 实际生成“你好。”并以 EOS 停止，三个 token 与完整 W8 参考一致。真实 Alice 正文的 128／512-token 前缀各继续生成四个 token，也与参考一致；两次完整请求分别为 `179.106 s`、`770.950 s`，设备 prefill 前向分别为 `145.792 s`、`735.410 s`，decode 速率约 `0.0935`、`0.0881 token/s`。这两项各只有一次测量，只报告实测样本。

**[BOUNDARY] 容量边界未通过全部数值检查。** 2041-token 前缀加八个生成 token 的请求已完成，设备实际维护到 2048 个 KV 位置，生成的八个 token 与参考一致；应用退出和资源回收正常。但 215 个张量检查只有 214 项通过，首个 128-token 块的第 23 层 hidden 有 11 个值超出 `atol=rtol=2e-4`，全部位于位置 0，不能记为完整验收通过。该 hidden 张量的最大绝对差为 `0.0021343`、相对 L2 为 `7.912e-6`；例如一个值为设备 `0.0923038`、参考 `0.0925392`，绝对差约 `0.0002353`，大于该元素约 `0.0002185` 的组合容差。

该容量请求开启检查点，实际宿主请求为 `5804.375 s`，约 `96.7 min`，仅保留为失败边界的诊断耗时，不纳入正式性能。其算子时间中 Softmax 约占 `38.4%`、mask 等 ADD 占 `14.8%`、缩放占 `11.7%`，W8 投影约占 `15.5%`。较长历史使注意力矩阵及其归约、mask 和缩放成为主要成本。当前不放宽数值阈值，也不以 token 相同代替 hidden 检查通过。

早期短定位保留了同一首 token 的 CPU 单 token 参考：CPU128 与 CPU1 的第 23 层 hidden 存在差异，但没有元素超过原阈值；旧设备与 CPU1 相比仍有一个超差。这组对照只说明不能直接把失败归为正常 FP32 误差；完整根因与协议 8 修复见[当前数值定位与复验](#model-e2e-results)。CPU1 只用于诊断，不替代原 CPU128 黄金参考。

> **[SOURCE] 短定位记录**：[三组同位置数值比较](../../build/gpgpu-validation/model-e2e/reviewed-v1/numeric-short-v1/result.json)、[11 个超差通道的设备／CPU128／CPU1 原值与预算](../../build/gpgpu-validation/model-e2e/reviewed-v1/numeric-short-v1/failing-channels.json)。该定位没有再次运行设备长前向。

> **[SOURCE] 扩展输入与边界**：[EOS 请求](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-eos/result.json)、[128-token 前缀](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-prefix128/result.json)、[512-token 前缀](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-prefix512/result.json)、[容量边界原始失败结果](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-capacity/result.json)、[分阶段实测汇总](../../build/gpgpu-validation/model-e2e/reviewed-v1/extended-performance.json)。容量结果的 `status=failed`、`performance_eligible=false`，原始输入、全部设备输出和参考均保留。

完整 W8 与原 FP32 属于另一项质量对照：上述短样例的八个 token 相同，但全部已保存 logits 的相对 L2 约 `0.0471`，第 23 层 hidden 汇总约 `0.1115`。这是量化在整个网络中传播的差异，不是设备实现误差，也不能由这一短样例推断其他文本的生成质量。

**完整请求的正式计时。** 两模型分别运行，配置与上方功能核对一致，但关闭 profile、trace 和检查点。权重装载一次后预热一个请求，Qwen 记录两个正式请求，YOLO 记录三个；下列数据来自实际请求，不使用旧独立算子估算。没有清空宿主文件缓存，模型准备时间不能称为磁盘冷缓存时间。

Qwen 的宿主模型准备为 `195.945 s`，其中客体权重文件读取为 `110.343 s`、BO 分配为 `54.496 s`、权重 H2D 为 `23.388 s`。真实指令的两个正式请求为 `128.520 s` 和 `127.764 s`，中位数 `128.142 s`。35-token prompt 的设备 prefill 前向中位数为 `54.774 s`，约 `0.637～0.641 token/s`；首 token ID 在客体请求开始后约 `55.159 s` 可用。之后七次 decode 的每步间隔为 `10.273～10.584 s`，实际速率约 `0.0958～0.0962 token/s`，不重复计入 prefill 产生的首 token。文本在整次生成结束后才写入宿主文件，没有把客体 ID 可用时间写成流式文本延迟。

Qwen 的算子累计时间中，W8 线性投影占 `39.4%`、Softmax 占 `12.7%`、RMSNorm 占 `8.0%`。单独分解时，参数上传约占算子时间的 `18.7%`，已包含在上述分类中。当前开销既有矩阵计算和标量归约，也有参数上传与同步；同步派发区间包含设备计算和等待，未单独计量的驱动锁等待不能给出精确占比。

YOLO 的宿主模型准备为 `13.004 s`，常驻权重后的三个正式请求为 `17.779 s`、`17.756 s`、`17.835 s`，中位数 `17.779 s`。设备完整网络前向中位数为 `17.156 s`，必要结果 D2H 及文件写入为 `0.200 s`，输入 H2D 为 `0.056 s`。宿主真实图像预处理约 `0.004 s`，NMS、坐标还原与结果保存约 `0.014 s`。按每个完整请求折算约 `0.0562 图/s`，不包含请求之间的可选验证和遥测区间，不作为持续服务吞吐。

YOLO 的算子累计时间中，SiLU 占 `39.0%`、BN 占 `34.5%`、卷积占 `14.5%`。按网络阶段划分，backbone、neck、attention、Detect 分别约占 `49.9%`、`18.6%`、`4.3%`、`27.1%`；这些比例使用算子时间作分母，不混入模型装载。该实现的主耗时是激活和未融合 BN，不能按通常的网络计算量直接认定卷积占主导。

独立 profile 运行相同的完整请求：Qwen 完成 `16,167,079,936` 次矩阵 MAC，YOLO 完成 `812,052,800` 次；两者的 `tensor_soft_tiles` 均为零，只说明块矩阵路径没有记录 SoftFloat 回退。按各自 hit＋miss 计数计算，Qwen 的 TLB／dcache 未命中比例约为 `7.74%／35.00%`，YOLO 为 `0.060%／3.26%`。这些计数反映本次模拟执行，不能当作实体显存总线流量，也没有将未命中逐次折算为延迟。诊断请求不混入上面的正式样本。

Qwen 短请求的五类 arena 共 `522.25 MiB`，其中 KV 容量占 `48 MiB`；YOLO 共 `16.47 MiB`，均不含页表和 RT 控制对象。模拟器仍保留 `2 GiB` VRAM，模型逻辑存储与进程常驻量分开记录。两次正式运行中，远端设备进程 RSS 高水位约 `2070 MiB`，ARM64 客体约 `505～508 MiB`，宿主 Python 约 `356～360 MiB`；这些数值包含各自的运行环境，不能直接相加作为模型内存峰值。

WSL 总内存约 `7.76 GiB`，两次运行退出前的可用内存均约 `2 GiB`。按运行前后采样比较，Qwen 的全局交换使用量净值未增加；YOLO 净增加约 `102.4 MiB`，而相关三进程的采样交换量均为零。机器未完全隔离，这组数据不能归因到某个模型页，也不能宣称全机无交换。每进程 RSS 高水位、PSS 采样和完整 `/proc/meminfo` 保留在结果中，没有用非连续采样冒充全机峰值。

> **[SOURCE] 独立剖析**：[Qwen 计数汇总](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-diagnostic-report.json)、[YOLO 计数汇总](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-diagnostic-report.json)。这些文件的 `diagnostic=true`、`performance_eligible=false`；原始 Dispatch 与客体日志随各自结果保存。

> **[SOURCE] 正式性能记录**：实际命令、逐样本数据、同一时钟的区间及源码快照均在结果目录；原始统计见 [Qwen](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-formal/result.json) 和 [YOLO](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-formal/result.json)，阶段／算子汇总见 [Qwen 性能](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-performance.json) 和 [YOLO 性能](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-performance.json)，量化质量另见[完整 W8 与原 FP32 对照](../../build/gpgpu-validation/model-e2e/reviewed-v1/quantization-quality.json)。

> **[SOURCE] 整网数值与输出记录**：均为 `8a435ac` 后的工作区，组件和源码快照随结果保存；实际比较、退出码、正常回收及内核日志核对均通过。
>
> - YOLO：[完整数值诊断](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-diagnostic/result.json)、[检测 JSON](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-formal/inference/sample-0003/postprocess/detections.json)、[标注图](../../build/gpgpu-validation/model-e2e/reviewed-v1/yolo-formal/inference/sample-0003/postprocess/annotated.jpg)。
> - Qwen：[hidden／KV／logits 诊断](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-diagnostic/result.json)、[生成文本](../../build/gpgpu-validation/model-e2e/reviewed-v1/qwen-formal/inference/sample-0002/generated.txt)、[完整 W8 CPU 参考](../../build/gpgpu-model-eval/e2e-reviewed-v1/qwen-chat/reference/metadata.json)、[原 FP32 对照](../../build/gpgpu-model-eval/e2e-reviewed-v1/qwen-chat/reference/original-fp32/metadata.json)。

独立 code review 已完成初审、修复复核和残留路径复核，已报告的静态评审问题均已关闭；上文的容量数值检查仍有未通过项。修复覆盖真实算子访问范围、协调期限、参考来源、正式计时、诊断模式配对及重复编码。Host 定向核对确认不足输出跨度、错误来源和两种诊断模式不匹配都会被拒绝；屏障期限独立于 Dispatch。用户态 `all check`、接口与结构体 `kernel-doc` 通过，Linux checkpatch 在遵守本仓库 typedef／新增文件约定后为 `0 errors / 0 warnings`。

修复后的完整算子回归通过 250 个数值签名、18,398,447 个输出元素，逐项退出码为零，失败值为零，均正常回收。标准 `test-gpgpu-stack` 也在 GPUVM 开、关下通过全部计算和生命周期示例，退出码均为零，内核日志检查通过。这些回归不替代上文仍未全通过的完整模型边界检查。该协议 7 验收未修改设备或指令实现，没有重跑 QTest；其 QTest 历史数字不用于证明协议 8。

> **[SOURCE] 评审与复验**：[评审闭合记录](../../build/gpgpu-validation/model-e2e/reviewed-v1/code-review.json)、[范围与屏障核对](../../build/gpgpu-validation/model-e2e/reviewed-v1/host-guards/runner-guards.log)、[参考来源拒绝](../../build/gpgpu-validation/model-e2e/reviewed-v1/host-guards/reference-admission.json)、[诊断模式双向拒绝](../../build/gpgpu-validation/model-e2e/reviewed-v1/host-guards/checkpoint-modes.json)、[风格与接口检查](../../build/gpgpu-validation/model-e2e/reviewed-v1/style-final/result.json)、[完整算子回归汇总](../../build/gpgpu-validation/model-e2e/reviewed-v1/ops-core/summary.json)、[标准全栈汇总](../../build/gpgpu-validation/model-e2e/reviewed-v1/core-regression/summary.json)。标准入口原日志另存在同目录 `core-before/`，沙箱 socket 失败也保留，没有作为数值通过。

**[BOUNDARY]** 当前范围是分别离线运行、batch=1、Qwen 容量不超过 2048、YOLO11n 固定 320×320 FP32。Qwen 超出容量会明确拒绝，不截断或滚动覆盖；未验证官方更长上下文、交互吞吐或实时检测。YOLO 固定照片核对不代表 COCO mAP 验证。详细 profile、trace 和检查点运行与正式计时分开；样本有限，不构造置信区间。Unix socket 被沙箱禁止时，启动会明确失败，不能记成设备数值通过。
