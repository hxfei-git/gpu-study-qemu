# QEMU GPGPU 四阶段实验大纲

| 缩写   | 英文全称                                               | 中文含义                                         |
| ------ | ------------------------------------------------------ | ------------------------------------------------ |
| QEMU   | Quick Emulator                                         | 仿真与虚拟化工具                                 |
| GPU    | Graphics Processing Unit                               | 图形处理器                                       |
| GPGPU  | General-Purpose Computing on Graphics Processing Units | 利用 GPU 进行通用计算；本项目的教学计算设备      |
| CUDA   | Compute Unified Device Architecture                    | 统一计算设备架构；此处用于原实验题名             |
| ROCm   | Radeon Open Compute                                    | AMD GPU 计算软件平台                             |
| ROCr   | ROCm Runtime                                           | AMD 的 HSA 用户态运行时                          |
| HSA    | Heterogeneous System Architecture                      | 异构系统架构                                     |
| HSAKMT | HSA Kernel Mode Thunk                                  | HSA 用户态与 KFD 内核接口之间的调用封装          |
| KFD    | Kernel Fusion Driver                                   | AMD GPU 计算相关内核驱动组件                     |
| CP     | Command Processor                                      | 命令处理器                                       |
| MEC    | Micro Engine Compute                                   | 计算命令处理微引擎                               |
| IH     | Interrupt Handler                                      | 中断记录与分发模块                               |
| IRQ    | Interrupt Request                                      | 中断请求                                         |
| ACK    | Acknowledgment                                         | 确认；本版用于确认已经读取执行结果               |
| AQL    | Architected Queuing Language                           | HSA 定义的队列包格式与提交协议                   |
| HQD    | Hardware Queue Descriptor                              | 保存队列驻留状态的硬件描述                       |
| MQD    | Memory Queue Descriptor                                | 内存中的队列描述对象                             |
| KIQ    | Kernel Interface Queue                                 | 内核接口队列                                     |
| HIQ    | HSA Interface Queue                                    | HSA 接口队列                                     |
| IB     | Indirect Buffer                                        | 间接命令缓冲区；第一版不引入                     |
| CPU    | Central Processing Unit                                | 中央处理器                                       |
| RV32   | 32-bit RISC-V                                          | 本实验计算解释器使用的 32 位 RISC-V 模型         |
| RV64   | 64-bit RISC-V                                          | 本实验原生 CP 使用的 64 位 RISC-V 模型           |
| PCI    | Peripheral Component Interconnect                      | 外设互连总线                                     |
| BAR    | Base Address Register                                  | PCI 基址寄存器；用于描述设备资源窗口             |
| VRAM   | Video RAM                                              | 设备显存                                         |
| DMA    | Direct Memory Access                                   | 直接内存访问                                     |
| IOVA   | I/O Virtual Address                                    | 设备访存使用的虚拟地址                           |
| RAM    | Random Access Memory                                   | 随机存取存储器；本文分别说明 Host RAM 和 VRAM    |
| LDS    | Local Data Share                                       | AMD 的工作组片上共享存储；作为实验共享存储的参考 |
| IOMMU  | Input/Output Memory Management Unit                    | 输入输出内存管理单元                             |
| SMMUv3 | System Memory Management Unit version 3                | Arm 系统内存管理单元第三版                       |
| MSI-X  | Message Signaled Interrupts Extended                   | 扩展消息中断                                     |
| DRM    | Direct Rendering Manager                               | Linux 图形设备内核框架                           |
| GEM    | Graphics Execution Manager                             | DRM 图形内存对象管理框架                         |
| GPUVA  | GPU Virtual Address                                    | GPU 虚拟地址                                     |
| GPUVM  | GPU Virtual Memory                                     | GPU 虚拟内存；设备地址空间及其映射管理           |
| VM     | Virtual Memory                                         | 虚拟内存；本文指 GPU 地址空间                   |
| TTM    | Translation Table Maps                                 | Linux 图形内存管理框架；用于对象存储位置管理     |
| MMU    | Memory Management Unit                                 | 内存管理单元                                     |
| VMID   | Virtual Memory Identifier                              | 设备虚拟地址空间标识                             |
| TLB    | Translation Lookaside Buffer                           | 地址翻译缓存                                     |
| ReLU   | Rectified Linear Unit                                  | 修正线性单元；本实验的计算示例之一               |
| CSR | Control and Status Register | 控制与状态寄存器 |
| PTE | Page Table Entry | 页表项 |
| ABI | Application Binary Interface | 应用二进制接口 |

<a id="current-progress"></a>

## 1. 阅读入口与实验目标

本大纲维护阶段依赖、实验范围和完成标准，不再记录当前任务或重复验收结果。开始工作先看[实验进度](<./实验进度.md>)；查构建命令、当前代码和实际运行记录，进入[软件栈说明](<../software/gpgpu/README.md>)。

- [CP/MEC 第一版规格](<./CP_MEC 第一版规格.md#spec-overview>)规定队列、派发、完成与错误处理；[GPUVM 第一版规格](<./GPUVM 第一版规格.md>)规定地址翻译、对象绑定和回收。
- [GPGPU 硬件手册](<./GPGPU_硬件手册.html>)按硬件模块组织寄存器、位域、访问权限和复位值。
- [CP/MEC 软件实现、参考与扩展（历史）](<./历史/CP_MEC 软件实现、参考与扩展.md>)保留旧实现分析、图示和 AMD/gem5 参考；查阅时按各段固定提交理解，不据此恢复已删除的接口。
- 规格确认过程只保留在 [CP/MEC 规格第 9 节](<./CP_MEC 第一版规格.md#spec-open-interfaces>)。原课程资料见[讲义](<./课程资料/QEMU_2026_讲义.html>)与[实验](<./课程资料/QEMU_2026_实验.html>)，按课程当时版本阅读。

<a id="stage-two-implementation-results"></a>

第二阶段的代码落点与验收记录已并入[软件栈第 6.2 节](<../software/gpgpu/README.md#62-第二阶段验证状态历史>)；本锚点保留供旧引用跳转。

<a id="stage-two-spec-review"></a>

第二阶段管理完成结果码的复核缺口、修复版本与异常保留结果，见[软件栈第 6.2 节的复核记录](<../software/gpgpu/README.md#stage-two-review>)。

**[DESIGN]** 实验参考 ROCr / HSA / KFD 的软件分层、对象关系和提交语义，形成类 AMD 的 GPGPU 教学软件栈。实验对象是自定义 QEMU 设备，寄存器、命令和地址设计按本项目约定实现；不以复现 MI300X 或兼容整套 ROCm 为目标。

## 2. 四阶段实验安排

四阶段按下列依赖顺序安排。第一、二阶段保留当时的接入范围，第三阶段说明统一后的接口，第四阶段在前述资源保护基础上完善异常恢复。实际推进到哪里，以实验进度为准。

```text
第一阶段：应用经运行时和驱动完成计算
  → 第二阶段：用户发布 AQL，CP 取包派发，Signal 报告完成
  → 第三阶段：引入 DRM，建立并隔离 GPU 地址空间
  → 第四阶段：完善异常恢复与资源回收
```

### 2.1 第一阶段：跑通应用到设备的计算流程

**[BOUNDARY]** 本节保留该阶段当时的安排与完成条件。旧同步接口、专项测试与固定数量已退出当前执行要求；版本 3 的实现与唯一核心计算入口统一见[第三阶段](#stage-three-vmid)。

第一阶段建立 Linux 驱动、用户态运行时、代码加载和参数传递，让计算 kernel 接收输入、执行并返回可核对的结果。A57、PCI / BAR、VRAM、DMA、SMMUv3、MSI-X、RV32 计算解释器及已有计算示例继续作为后续阶段的基础。新增 CP 队列路径时，保留原同步路径的回归验证。

### 2.2 第二阶段：用户 AQL Queue、Doorbell、CP 与 Signal

**[BOUNDARY]** 本节保留该阶段当时的安排与完成条件。旧同步接口、专项测试与固定数量已退出当前执行要求；版本 3 的实现与唯一核心计算入口统一见[第三阶段](#stage-three-vmid)。

本阶段要让应用连续发布任务，由设备独立推进计算，再通过 Signal 等待结果。用户态向上实现到 HSAKMT 风格接口库，封装驱动的设备、内存、队列和事件能力；实验程序等调用方组织命令包，CP 固件负责读取、解析和派发。ROCr/HSA 运行时及高层任务接口留待后续评估。

```text
Host 装载 CP 固件，建立首次 KIQ
  → 驱动经 KIQ 建立 HIQ，确认可用后经 HIQ 建立用户队列
  → 调用方准备代码、参数和 Signal，向用户 Ring 发布 AQL Packet
  → Doorbell 通知设备检查生产进度
  → CP 从自己的消费位置读取任务，交给计算执行器
  → 计算完成并收尾，CP 发布 Signal，回报消费进度
  → IH 记录经中断交给驱动，驱动定位事件并唤醒等待者
  → 应用检查结果，驱动按卸载协议释放队列资源
```

第一版的队列分工、阻塞执行、消费时序和管理完成协议以[规格](<./CP_MEC 第一版规格.md>)为准。本大纲不再单独规定队列数量、Ring 容量、索引位宽或内存布局。

#### 2.2.1 实现顺序与范围

以实验子仓库 `57543dd54b6b046b888277272fa84af81746b1ac` 和当前已确认规格为基线，沿用双进程设备、RV64 裸机 CP、RV32 计算执行器及一次完整执行一个 Dispatch 的模型。队列先使用 `REMOTE=1、IOMMU=none、VMID=0`。原同步路径继续单独回归。

1. **建立可重复的内核构建。** 根据缓存包固定 Linux `6.18.57` 对应的 Alpine 构建提交、源码校验值、配置和补丁顺序。按 2026-10-19 的调整，默认使用固定版本的宿主机交叉工具链，独立保存源码和输出；客体原生方式保留为可选入口，其默认资源为 32 GiB 构建盘、4 个虚拟 CPU、4 GiB 内存。构建并行度、资源和超时可调整。配置、补丁及构建脚本进入仓库，源码与产物放在 `build/`，保留原环境。启用内置 `GPU_BUDDY=y`，补齐隐藏配置项、模块签名路径和依赖，生成同版本内核、模块、开发文件、`Module.symvers` 和启动镜像；开发文件中的构建工具须能在 ARM64 客体执行。先验证启动、驱动编译及第一阶段回归。
2. **落实共享定义与完整访存。** 集中定义对象布局、寄存器、IH 地址、提交结果、执行器错误枚举和新增 ioctl，用编译期检查核对大小、偏移和对齐。共享首部和 W/R 使用完整 32 位访问，Signal、管理标记和通知槽使用完整 64 位访问。消息 DMA 对自然对齐的 4/8 字节直接 RAM 访问采用等宽原子读写，保留地址翻译、权限、映射引用、脏页及指令翻译失效处理；其余搬运沿原路径。核对 Host 进度映射可供 CPU 原子取号。
3. **接入 HQD、Doorbell 与 IH。** Host 选槽窗口和 CP 本地窗口访问同一份 HQD，检查访问来源、宽度和状态。Doorbell 按完整 8 字节处理，通过配置匹配队列，只提示重新检查。IH 使用第四个 MSI-X 向量，实现记录发布、消费回报、屏蔽恢复和确认后的再次通知；IH 故障通过原错误中断交付。
4. **接入执行器交接和 Host 参数窗口。** 保留 RV32 解释器，增加配置快照、代码及参数范围检查、执行状态和结果确认。任务对象保存线程状态、工作组位置和累计预算，由后续 QEMU 回调分段推进。只读参数窗口保留完整 Host 地址。相关访问结束后才发布终态，CP 仍等待整个 Dispatch 完成。
5. **实现 CP 固件。** 先完成末尾 4 MiB 固件区的链接、栈、全局数据初始化和访存异常处理，再实现 KIQ 建立 HIQ、HIQ 管理用户队列、AQL 取包及派发。固件分别保存槽位绑定和管理请求结果，按既定顺序检查队列。成功路径依次完成执行确认、Signal、通知槽、IH、无效首部及消费回报；错误路径保留首次错误和已有写入，按规定范围停止取包。
6. **接入驱动资源管理。** 驱动装载时选择互斥的旧同步模式或队列模式。队列模式支持多个进程上下文，取消打开、关闭及等待超时触发的全局复位，分别处理 DMA、IH 和错误。设备统一使用 `gpu_buddy` 管理前 60 MiB VRAM，登记首个保留页。Host 内存、队列、事件、管理请求及用户映射分别持有引用；Doorbell 按进程分配独占映射页，管理入口使用单独页面。
7. **贯通管理、事件和正常卸载。** 驱动装载固件并初始化首次 KIQ、IH。管理锁只保护命令对发布，释放后各请求等待独立结果区；事件处理先保存通知或故障，再唤醒。正常销毁先由调用方停止提交、完成发布并排空，驱动复核后请求卸载；确认 MQD 保存和旧通知处理结束，再解除引用。创建失败、超时或进程退出时，尚不能确认访问结束的资源继续保留。
8. **接入 HSAKMT 风格库和调用方。** 库按[规格 9.7](<./CP_MEC 第一版规格.md#spec-host-api-scope>)提供设备、内存、队列和事件接口，分别返回句柄、映射资料和设备访问地址，检查版本与状态。实验程序共用辅助代码实现 AQL 取号、填包、发布、Doorbell 和 Signal 等待，不增加逐任务 ioctl。先验证连续向量加法，再复用矩阵乘和 ReLU。固定少量提交线程，每线程最多保留一个未发布编号，限制在途预留。默认等待从本次等待开始主动检查约 `200 μs`，再进入事件等待并复查 Signal。

以下为第二阶段历史接入安排，路径及模式仅对应当时实现。设备按当时职责增加 HQD、IH 和参数访存组件；固件分开组织主循环、访存与异常、管理命令、AQL 派发与完成；驱动分为 PCI 入口、内存、队列和事件，接口库按四组接口分文件。接口说明输入输出、所有权和完成条件，关键锁、屏障及引用位置添加中文注释。原 ioctl 和同步库接口保留，新接口独立检查版本。为将多个源文件链接为原来的 `gpgpu_pci.ko`，仅将入口源文件改名为 `gpgpu_pci_main.c`，同步源码清单、构建指纹及装载逻辑。

第二阶段接入前基线的 Doorbell 写回调为空且最大访问宽度为 4 字节，消息 DMA 的通用内存拷贝也尚未证明并发 64 位访问完整性；旧驱动会全局清中断并在超时和会话关闭时复位。这些缺口分别由步骤 2、3、6 处理，队列协议继续以规格为准。AMD 的 Doorbell 分页、原子取号、完整首部发布和事件等待实现只用于补齐实现细节。

**[BOUNDARY]** 多 VMID、抢占、强制中止、运行中复位恢复、预留空洞修复、完整 ROCr 和共享存储留在后续扩展。第二阶段等待超时只结束本次等待，资源保留规则仍须验证。SMMUv3 下的队列地址翻译接入不在本轮范围；已有 DMA 和 CP Host 窗口继续保留两种 IOMMU 配置回归。

#### 2.2.2 五组验收与完成标准

设备边界用例加入现有 QTest，Linux 侧增加一个队列测试程序，按以下五组检查：

1. **正常提交。** 两条用户队列连续运行，逐轮改变输入；检查结果、Signal、消费进度和管理结果，覆盖物理槽及 32 位编号回绕。
2. **并发与可见性。** 同队列多线程取号，覆盖前包延迟发布、W 不变时补发布、乱序 Doorbell、两个进程管理命令成对发布。验证首部先失效再回报 R，用高低位均变化的数值检查 64 位完整访问。任务数据与屏障位置按[规格 6.4 节](<./CP_MEC 第一版规格.md#spec-dispatch-visibility>)核对。
3. **等待与通知。** 覆盖主动检查期间完成、阻塞后完成、通知先于登记、多个等待者和超时，合并检查 IH 屏蔽恢复及确认时新记录到达。
4. **基本错误退出。** 覆盖非法 Dispatch、参数访问失败、普通管理失败、管理通道及 IH 故障，检查停止范围、等待退出、首次错误及资源保留。逐次写入失败组合的穷举留到[异常恢复扩展](#failure-recovery-extension)。
5. **正常卸载与寿命。** 未排空时拒绝正常销毁，排空后卸载并重建；旧通知、等待者或映射仍在时禁止提前复用。进程退出不能释放设备仍可能访问的资源。

该阶段沿用当时的 CP、IOMMU、设备、汇编器与同步路径回归。历史结果见软件栈第 6 节；当前只运行软件栈第 5 节列出的入口，按测试程序提供的计划核对结果，不另设固定数量检查。

完成标准是应用能连续发布多个 AQL 任务，提交不等待 Kernel 完成；CP 按规格处理队列，任务结果、消费进度和管理完成分别可观察；默认等待支持短暂轮询后转入内核等待，IH 唤醒后复查 Signal；正常卸载后设备不再访问已释放资源，旧通知和驱动后续处理不引用已回收对象。每步记录实际实现与运行结果，未运行或未通过的检查明确保留，不能以代码或文档已写完代替整体验收。

<a id="stage-three-vmid"></a>

### 2.3 第三阶段：统一访存、CSR 启动信息与固定存储

当前软件栈统一采用 DRM/GEM、AQL Queue、Doorbell、RV64 CP 与 Signal/IH，计算执行器继续运行 RV32 Kernel。设备属性 `gpuvm=on/off` 决定用户访存是否翻译，默认开启。固定存储、两个用户 Queue 名额和正常资源保护继续保留，当前验证只运行三个既有计算示例。

```text
应用创建并绑定全部对象
  → 创建 Queue → 发布 AQL → 写 Doorbell
  → CP 读取描述符并派发 RV32 Kernel
  → CSR 读取线程坐标，a0 读取真实参数地址
  → GPUVM 翻译或物理地址直通
  → 公共物理入口：VRAM / Outbound → Host IOVA → PCIe DMA
  → Signal、通知槽与 IH 完成通知
  → Host 比较输出 → 排空卸载 Queue → 解绑回收
```

#### 2.3.1 连续物理地址与显式访问域

GPU 物理空间分为 `[0, vram_size)` 的 VRAM 和 `[vram_size, 0x1_0000_0000)` 的 Outbound。64 MiB VRAM 下，Host IOVA `0x4100_0000` 对应 GPU 物理地址 `0x4500_0000`。驱动在 CP 启动前读回核对设备共用窗口的只读布局，再写 CONTROL 使能并确认，运行期间拒绝重配置；DMA 分配统一使用 31 位 mask，检查完整范围并分别保存原始 DMA 地址和设备协议地址。

用户域按设备属性翻译或直通；管理域始终使用 GPU 物理地址。页表根、中间表、PTE 目标、管理队列、MQD、完成区、IH 与 COPY 端点使用同一地址语义。RV64 CP 自己的代码、栈与本地访存窗口继续保留。

#### 2.3.2 DRM/GEM 与独立绑定引用

两种状态都只开放 DRM render node。应用对象提供 `cpu` 与 `gpu_address`；MAP 返回最终设备地址，两种状态都建立绑定引用。开启 GPUVM 时，三级 4 KiB 页表与同步 TLB 失效继续生效，可分配区间统一为 `[0x1000, 0xffff_f000)`；关闭时不创建页表，非零固定 GPUVA 请求不支持。

Queue 存活期间冻结绑定。关闭 GEM handle、解除 GPU 绑定和结束 CPU mmap 分别处理，最后引用结束后才回收存储；管理完成结果未知或访问未确认结束时继续保留。关闭 GPUVM 后共享物理空间，不提供进程页权限隔离，也不增加进程独占限制。

#### 2.3.3 CSR 启动信息与真实参数地址

每 Warp 保存 Block 编号、Block 尺寸与 Grid 中 Block 数量，每 Lane 保存线程坐标。新 Warp 创建时初始化，分段继续执行时保留；通过 `0xCC0～0xCCB` 的只读 CSR 读取，`mhartid` 保留 `0xF14`。只支持 `CSRRS` 且 `rs1=x0` 的读取，未知 CSR 和写入请求返回指令错误。

`a0=kernarg_address`，不再设置参数窗口。控制页与参数窗口的保留规则删除；GPUVM 开启时 `0x8000_0000` 和 `0x9000_0000` 都可正常映射。16 字节描述符的最后字段保存 Kernel ABI 版本 3，AQL 仍为 64 字节，设备／固件、DRM 与 GPUVM 协议同步升级，拒绝旧组件。

#### 2.3.4 公共队列组件与旧代码清理

`lib/kmt/` 的 HSAKMT 风格薄层负责预留槽位、发布 Header、8 字节 Doorbell 写入、Signal 与事件等待、排空和销毁；`lib/rt/` 的小运行时负责装载 Kernel 与组包派发。三个示例统一链接 `libgpgpu.a`。完成顺序保持为 Signal、通知槽、IH、INVALID Header、消费进度。

旧同步提交、旧 Runtime、字符设备 ABI、三模式分支、特殊地址窗口及专用测试均删除，构建、CI、客体脚本与工具同步清理。范围检查、故障处理和资源保护继续用于正常执行，不作为测试辅助代码删除。

<a id="stage-three-acceptance"></a>

#### 2.3.5 唯一核心计算验证

使用 `make -f Makefile.gpgpu test-gpgpu-stack`，固定 ARM virt、A57、1 GiB、IOMMU=none，分别运行 `gpuvm=on` 和 `gpuvm=off`。两种状态采用同一流程与放置方式：代码、描述符和数组在 VRAM，参数和队列相关 Host 对象经 Outbound 访问。

向量加法、矩阵乘法和 ReLU 沿用既有规模与逐元素比较规则，任一失败返回非零。示例规模、运行命令与实际结果见[软件栈说明](<../software/gpgpu/README.md#build-and-validation>)。

本轮不增加 CSR、Outbound、协议或生命周期专项验收，不扩展多进程、IOMMU、存储位置或数据规模矩阵。SMMUv3 功能保留。

#### 2.3.6 历史成果与后续边界

第三阶段早期实现的固定提交、验收归档和日志见[历史验收记录](<../software/gpgpu/README.md#stage-three-validation>)。其中的旧控制页、参数窗口、同步模式及专项回归不再列入当前执行要求。

**[BOUNDARY]** 固定存储与 AQL 提交继续用于项目讲解。TTM、迁移、驱逐、换页、按需补页和异步页表更新留在后续；强制中止及运行中复位恢复归[第四阶段](#24-第四阶段完善异常处理与资源释放)。教学设备不作为 MI300X 硬件结论。

### 2.4 第四阶段：完善异常处理与资源释放

在第二阶段的队列卸载和资源保护基础上，围绕非法命令、访问 fault、任务超时及进程退出，完善 CP / 执行引擎中止、设备复位和恢复，验证恢复后能够继续执行正常任务。任务卡住后的强制停止与复位重建按[异常恢复扩展](#stalled-task-recovery-extension)在后续阶段细化。AQL 用户队列仍属于第二阶段的必达范围。

## 3. 后续扩展与进度维护

多 VMID 的管理与设备访问隔离范围见[第三阶段](#stage-three-vmid)。后续还需要增加 Kernel 执行中途暂停、让出计算资源和恢复执行的能力。第一版暂不采用上下文保存寄存器组；相关接口、现场保存与恢复、完成状态管理见[后续扩展](<./历史/CP_MEC 软件实现、参考与扩展.md#extensions>)。暂停与恢复的实施阶段随执行器和多队列功能的进展确定，当前不预设工期。

<a id="gpuvm-advanced-extension"></a>

GPUVM 动态管理不作为第三阶段验收内容。TTM 接入、存储迁移和驱逐用于管理多个存储位置；换页与按需补页需要额外的存储后备、故障处理和继续执行协议；运行中或异步更新需要安排旧访问及后续任务依赖。VMID 超额轮换、别名与部分撤销、跨进程共享和更细的通知权限也按实际需求后续评估。本期分别用固定存储、提前分配、Queue 存活期间冻结映射和资源不足时明确失败来限制范围，不预先设计这些扩展的完整接口。

<a id="rocr-runtime-extension"></a>

ROCr/HSA 运行时、高层任务提交与自动回收、通用内存池、通用 Kernel 装载器及完整设备拓扑留待后续单独评估。第二阶段先实现[HSAKMT 风格接口库](<./CP_MEC 第一版规格.md#spec-host-api-scope>)，通过实验程序验证内核功能和既定 AQL 协议。以后是否采用标准 HSA 接口、兼容到哪一层，根据届时的设备与驱动能力决定；当前不设计精简 ROCm 软件栈，也不承诺原版 libhsakmt 或 ROCr 可以直接运行。

<a id="queue-unmap-extension"></a>

取消未执行任务、保留未执行任务跨卸载续跑，以及提交线程异常退出后的预留空洞填补与自动恢复列为后续扩展。第一版正常卸载按[规格 7.4 节](<./CP_MEC 第一版规格.md#spec-unmap-drain>)先停止提交并等待排空；故障队列沿用错误退出和资源保留规则。需要上述扩展时，再确定未执行任务的结果、等待退出和重新装载后的处理方式，当前不增加执行中途保存恢复。

<a id="stalled-task-recovery-extension"></a>

任务长时间不结束时的挂死判断、强制中止、单队列或管线复位、设备整体复位恢复及自动恢复，留到后续优化，不作为第二阶段必做或验收内容。运行期间 KIQ 重配、故障 HIQ 重建、复位后旧队列与旧请求的处理也随此扩展细化，范围见[规格 9.6.4](<./CP_MEC 第一版规格.md#spec-stalled-task-scope>)和[9.6.5](<./CP_MEC 第一版规格.md#spec-runtime-reset-scope>)。第二阶段的等待超时只结束本次等待；尚不能确认设备访问结束时，相关资源继续保留。后续根据实际故障需求确定停止方法、完成确认和恢复步骤，当前不预先固定复位方案。

<a id="failure-recovery-extension"></a>

失败包的基本退出与资源保留按[规格 7.3 节](<./CP_MEC 第一版规格.md#spec-failure-retirement>)执行。各个写入步骤失败后的状态组合、逐步故障注入、自动重试和原地恢复列为后续扩展，第一版不逐项细化。扩展时再确定安全重试条件，避免重复执行 Kernel 或重复更新完成状态；已确定的完整读写、发布顺序和资源释放前提继续作为第一版要求。具体范围随异常处理的实际需求确定。

<a id="signal-atomic-extension"></a>

第二阶段由 CP 单独更新 Signal，不要求原子读改写接口。如果以后引入多个并发更新者，再扩展对应的原子操作及访存后端，参考关系见[Signal 原子操作扩展](<./历史/CP_MEC 软件实现、参考与扩展.md#signal-atomic-extension>)。实施阶段随需求确定。

<a id="signal-active-wait-extension"></a>

`HSA_WAIT_STATE_ACTIVE` 的持续主动等待列为后续扩展，第二阶段只实现默认的短暂轮询后阻塞等待。扩展时再增加调用者选择等待模式的接口，并明确错误与超时返回，参考关系见[持续主动等待扩展](<./历史/CP_MEC 软件实现、参考与扩展.md#signal-active-wait-extension>)；具体实施阶段随需求确定。

<a id="compute-resource-extension"></a>

工作组共享存储、组内屏障和可配置寄存器资源列为后续扩展，参考 AMD 的 LDS 与资源配置用途，具体实施阶段随执行器需求确定。第一版继续为每个线程固定提供通用、浮点寄存器；`ax+b` 中的只读标量通过参数块传入，不要求共享存储。扩展时一起确定资源声明、每组共享区域、访问边界、屏障与内存可见性，并调整当前逐 warp 执行的方式，使同组线程能够完成同步。概念与例子见[线程寄存器、参数与共享存储](<./历史/CP_MEC 软件实现、参考与扩展.md#kernel-registers-lds>)，功能范围见[计算资源扩展](<./历史/CP_MEC 软件实现、参考与扩展.md#compute-resource-extension>)；当前不增加 AMD 的完整资源寄存器或 MQD 字段。

四阶段说明推进方向，不预先固定全部功能和接口。每完成一段实验，或遇到影响计划的问题，就根据代码、运行结果与学习反馈调整近期工作。实验计划统一在本大纲维护，当前阶段、近期任务和阻塞项只写入[实验进度](<./实验进度.md>)；学习笔记中的理论缺口仍补回首次需要它的位置，第七章大纲继续管理后续笔记。
