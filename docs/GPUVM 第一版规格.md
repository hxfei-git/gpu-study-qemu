# GPUVM 第一版规格

| 缩写   | 英文全称                                               | 中文含义                                                    |
| ------ | ------------------------------------------------------ | ----------------------------------------------------------- |
| GPUVM  | GPU Virtual Memory                                     | GPU 虚拟内存；设备地址空间及其映射管理                      |
| VM     | Virtual Memory                                         | 虚拟内存；本文指 GPU 地址空间及其上下文                     |
| GPUVA  | GPU Virtual Address                                    | GPU 虚拟地址                                                |
| GPU    | Graphics Processing Unit                               | 图形处理器                                                  |
| CPU    | Central Processing Unit                                | 中央处理器                                                  |
| QEMU   | Quick Emulator                                         | 仿真与虚拟化工具                                            |
| GPGPU  | General-Purpose Computing on Graphics Processing Units | 利用 GPU 进行通用计算；本项目的教学设备                     |
| DRM    | Direct Rendering Manager                               | Linux 图形设备内核框架                                      |
| GEM    | Graphics Execution Manager                             | DRM 图形内存对象管理框架                                    |
| BO     | Buffer Object                                          | 保存实际存储及其属性的缓冲对象                              |
| TTM    | Translation Table Maps                                 | Linux 图形内存管理框架；AMDGPU 用于管理对象的存储位置与迁移 |
| PTE    | Page Table Entry                                       | 页表叶子项，保存目标页地址和权限                            |
| PDE    | Page Directory Entry                                   | 页目录项，定位下一级页表                                    |
| GART   | Graphics Address Remapping Table                       | 图形地址重映射表；用于说明 AMDGPU 管理上下文的翻译          |
| MMU    | Memory Management Unit                                 | 内存管理单元                                                |
| LRU    | Least Recently Used                                    | 最久未用；TLB 组内最先被替换的一路                          |
| MRU    | Most Recently Used                                     | 最近使用；TLB 组内刚用过的一路                              |
| TLB    | Translation Lookaside Buffer                           | 地址翻译缓存                                                |
| VPN    | Virtual Page Number                                    | 虚拟页号，即 GPUVA 除以 4 KiB                               |
| VMID   | Virtual Memory Identifier                              | 设备虚拟地址空间标识；用于选择页表上下文                    |
| CP     | Command Processor                                      | 命令处理器                                                  |
| MEC    | Micro Engine Compute                                   | 计算命令处理微引擎；用于第二阶段规格题名                    |
| HQD    | Hardware Queue Descriptor                              | 保存队列驻留状态的硬件描述                                  |
| MQD    | Memory Queue Descriptor                                | 内存中的队列描述对象                                        |
| KIQ    | Kernel Interface Queue                                 | 内核接口队列                                                |
| HIQ    | HSA Interface Queue                                    | HSA 接口队列                                                |
| IH     | Interrupt Handler                                      | 中断记录与分发模块                                          |
| IRQ    | Interrupt Request                                      | 中断请求；本文沿用独立错误中断路径                          |
| ACK    | Acknowledgment                                         | 确认；本文用于翻译失效完成确认                              |
| HSA    | Heterogeneous System Architecture                      | 异构系统架构                                                |
| HSAKMT | HSA Kernel Mode Thunk                                  | HSA 用户态与 KFD 内核接口之间的调用封装                     |
| AQL    | Architected Queuing Language                           | HSA 定义的队列包格式与提交协议                              |
| RV64   | 64-bit RISC-V                                          | 本实验原生 CP 与计算执行器使用的 64 位 RISC-V 模型          |
| KFD    | Kernel Fusion Driver                                   | AMD GPU 计算相关内核驱动组件                                |
| VRAM   | Video RAM                                              | 教学设备的显存后备存储                                      |
| RAM    | Random Access Memory                                   | 随机存取存储器；Host RAM 与 VRAM 分开                       |
| DMA    | Direct Memory Access                                   | 直接内存访问                                                |
| IOVA   | I/O Virtual Address                                    | 设备通过系统 IOMMU 访问内存时使用的地址                     |
| IOMMU  | Input/Output Memory Management Unit                    | 输入输出内存管理单元                                        |
| SMMUv3 | System Memory Management Unit version 3                | Arm 系统内存管理单元第三版                                  |
| BAR    | Base Address Register                                  | PCI 设备基址寄存器；描述设备资源窗口                        |
| PCI    | Peripheral Component Interconnect                      | 外设互连总线                                                |
| CSR    | Control and Status Register | 控制与状态寄存器；用于读取线程启动信息 |
| SGPR   | Scalar General-Purpose Register | 本实验中每 Warp 共享的只读启动状态 |
| VGPR   | Vector General-Purpose Register | 本实验中各 Lane 独有的只读启动状态 |
| ALU    | Arithmetic Logic Unit | 算术逻辑单元 |
| ABI    | Application Binary Interface | 二进制接口 |
| PCIe   | Peripheral Component Interconnect Express | 高速外设互连总线 |
| API    | Application Programming Interface                      | 应用编程接口                                                |

本文保留 GPUVM 第一版的固定存储设计，沿用统一访存方案，当前为协议版本 5，与版本 4 的差异见 [PROTOCOL_V5.md](<../software/gpgpu/PROTOCOL_V5.md>)。应用统一使用 DRM/GEM、AQL Queue、Doorbell、RV64 CP 和 Signal/IH；RV64 Kernel 的访存模式只由设备属性 `gpuvm=on/off` 决定，默认开启。实现入口和实际运行记录见[软件栈说明](<../software/gpgpu/README.md#stage-three-gpuvm>)。

**[DESIGN]** 开启 GPUVM 时按页表翻译，对象的 GPUVA 就是它在应用进程中的 CPU 地址；关闭时直接解释 GPU 物理地址。两者最终使用同一个 VRAM／Outbound 物理入口。线程启动信息改为只读 CSR，`a0` 保存真实参数地址。旧控制页、参数窗口、SYSTEM 直接指向 Host DMA 的页表编码和三模式接口均已移除。

建议先读第 1 节的使用流程，再读第 2～7 节的对象、访问和回收，最后检查第 9 节的范围记录。推进顺序见[第三阶段大纲](<./QEMU GPGPU 实验大纲.md#stage-three-vmid>)。

<a id="gpuvm-overview"></a>

当前阶段与阻塞项见[实验进度](<./实验进度.md>)；代码结构、构建命令和实际验收见[软件栈说明](<../software/gpgpu/README.md>)。本文只维护 GPUVM 的已确认设计，协议常量以 `software/gpgpu/include/gpgpu/` 为准，各组件版本从 `gpgpu_version.h` 的 `GPGPU_PROTOCOL_VERSION` 派生。

## 1. 首版目标与应用使用流程

### 1.1 用 GPUVA 完成一次向量加法

本节先以 GPUVM 开启为例。应用 A 要计算 `Z[i] = X[i] + Y[i]`。Host 准备数组、代码和参数，Kernel 使用 GPUVA 读取这些对象。应用 B 可以使用相同 GPUVA，由自己的页表指向另一份存储。

```text
打开设备 → 驱动建立应用地址空间，库创建并登记本进程的通知页
  → 准备对象：分配实际存储、以对象的 CPU 地址作 GPUVA 建立映射、填写内容
  → 创建用户 Queue：MQD/HQD 关联该地址空间的 VMID
  → 向 Ring 发布 AQL，通知 Doorbell
  → CP 按 VMID 翻译用户 Ring、描述对象和完成对象
  → 执行器按同一 VMID 翻译代码、参数和数组访问
  → 写回结果，CP 更新 Signal、通知槽和 IH
  → 应用可继续提交，重复使用已经准备好的对象
  → 停止提交、排空并销毁 Queue
  → 撤销映射、等待 TLB 失效，再回收对象和地址空间
```

开启 GPUVM 时保留五项能力：DRM/GEM 对象管理、完整用户访存翻译、双进程隔离、同步 TLB 失效及 VMID 复用、基本故障退出与正常回收。CP 继续为 RV64 裸机固件，计算执行器也是 RV64；全设备仍有两个用户 Queue 名额，首版每个 VM 最多一条用户 Queue，Kernel 执行中途不切换。

> **[SPEC]** [CP/MEC 第一版规格，第 1 节](<./CP_MEC 第一版规格.md#spec-overview>)及[用户队列名额](<./CP_MEC 第一版规格.md#spec-user-queue-capacity>)，2026-10-19 状态：4 个 HQD 用于 KIQ、HIQ 和两个动态用户队列。第三阶段沿用设备容量，增加每个 VM 最多一条用户队列的首版限制。

### 1.2 先准备、重复运行、统一释放

库的普通内存申请同时完成对象创建和绑定，向应用提供 `cpu` 与 `gpu_address`。开启 GPUVM 时 `gpu_address` 是 GPUVA，数值等于对象在本进程中的 CPU 地址；关闭时是对象固定的 GPU 物理地址。调用方不必逐对象操作页表、VMID 或失效寄存器。VRAM 内容继续通过已有上传、下载入口准备和读取。

GPUVA 与 CPU 地址相同，做法参考 AMD 的 libhsakmt：先在 CPU 上申请地址，再让 GPU 使用同一个地址。库申请数组 X（VRAM，17 页）时按下面四步进行：

```text
1. 库用 mmap(PROT_NONE) 申请一段对齐的 CPU 地址，例如 0x0000_ffff_8a40_0000
2. BO_CREATE：驱动分配 X 的 VRAM 存储，返回句柄和 mmap 偏移
3. 库经 render node 用 MAP_FIXED 把 X 映射到这段地址，替换第 1 步的占位：
   Host 对象可读写；VRAM 对象只能是 PROT_NONE，CPU 访问会触发段错误
4. MAP(0x0000_ffff_8a40_0000)：驱动写页表，gpu_address 就是这个地址
```

第 1 步占住地址，进程里别的 mmap 不会再分到这段地址；第 4 步之后，CPU 和 GPU 用同一个数值指向 X，参数块和 Signal 可以直接写 CPU 指针。VRAM 对象的这段 CPU 地址只用来占位，公开的 `cpu` 字段为 NULL。释放时库先 UNMAP，确认 GPU 页表不再指向 X 后才解除 CPU 映射；UNMAP 失败时这段地址继续保留，避免被 CPU 分给别的映射，而 GPU 页表仍指向旧对象。

> **[SOURCE]** ROCr `ba56a24c6132c5d195686ae4adf969ca1222fbba` 中的 libhsakmt：[fmm.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:770:1>) [第 770～815 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:770:1>)的 `hsakmt_mmap_allocate_aligned()` 先以 `PROT_NONE` 预留并裁出对齐的 CPU 地址；[第 1569～1584 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:1569:1>)的 `fmm_map_to_cpu()` 用 `MAP_SHARED | MAP_FIXED` 把 BO 映射到同一地址，有 HostAccess 时可读写，没有时保持 `PROT_NONE`；[第 1799～1812 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:1799:1>)在分配显存对象后调用它。本设备照搬“先占 CPU 地址、再同址映射”的顺序，不复制 libhsakmt 的 aperture 管理与对齐策略。

Queue 存活期间可以继续创建、绑定和释放对象，任务数据的读写仍遵守第二阶段发布与等待协议。Ring 与进度对象由驱动的使用计数保护，Queue 卸载前不能解绑；驱动无法判断其他对象是否仍被任务使用，应用要在解绑前确认，规则见[第 5.2 节](#52-queue-存活期间同步修改页表)。事件在建 Queue 后创建，从通知页分配槽，再填写 Signal，不增加 GPU 映射。

每个 BO 只建立一段覆盖整个对象的绑定，不支持别名、部分撤销、隐式替换或跨进程共享。底层保留同步 `MAP/UNMAP` 便于实现和验证，普通应用由库封装。开启 GPUVM 时可指定固定 GPUVA：库要求这段 CPU 地址尚未被占用，驱动仍执行完整校验；关闭时非零 `fixed_va` 返回不支持。驱动只检查 MAP 地址的范围、对齐和重叠，不检查它是否等于 CPU 地址，这条约定由库保证。

**[BOUNDARY]** 本期不接入 TTM，不做存储迁移、显存驱逐、换页或缺页恢复，也不在设备缺页时分配后备存储。对象创建时取得全部存储，并持有到安全释放；空间不足返回失败。这里省略的是存储动态管理和恢复，GPUVA 的分页映射、页表遍历及权限检查仍须实现。

### 1.3 AMDGPU 参考与设备边界

源码统一参考仓库 `2.源码/linux` 的固定提交 `248951ddc14de84de3910f9b13f51491a8cd91df`。对象关系按 AMDGPU 核对，MI300X 主线相关的软件路径参考 `gmc_v9_0`、`gfxhub_v1_2`。本设备的地址范围、页表层级和寄存器是教学设计，不声明兼容 MI300X 硬件。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_discovery.c，第 2148～2157 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2148-L2157)为 9.4.3 选择 `gmc_v9_0`；[gmc_v9_0.c，第 1487～1493 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1487-L1493)在 `amdgpu_is_multi_aid()` 返回真时选择 `gfxhub_v1_2`；[gfxhub_v1_2.c，第 319～325 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gfxhub_v1_2.c#L319-L325)标注其用于 9.4.3 变体。本文据此选择相关驱动参考路径。

CP 自身的固件取指、栈和设备寄存器访问继续使用裸机地址。普通 DRM 命令提交、完整 HSA 运行时、抢占和运行中复位恢复不在本期范围。

<a id="gpuvm-objects"></a>

## 2. 应用上下文、内存对象与实际存储

### 2.1 一个连接持有一个地址空间

应用通过现有 HSAKMT 风格库打开 DRM render node，一个 DRM 文件上下文持有一个 GPU VM。库内线程共享连接；不同文件上下文的句柄、映射、队列和事件各自管理。不增加独立计算设备与 DRM 文件的关联协议，也不支持连接继承或文件描述符传递后的队列使用。

```text
应用 A 的库连接 → DRM 文件上下文 A
                   ├─ 对象句柄 → GEM/BO → 实际存储
                   ├─ GPUVA 映射 → BO、权限、页表
                   ├─ VM → 页表根、绑定的 VMID
                   └─ 用户 Queue → Ring、进度对象和所属 VM
```

驱动创建队列时为文件上下文取得 VMID，应用不能指定其他上下文的编号或页表根。软件 VM 保存地址空间；VMID 是设备选择翻译上下文的编号；HQD 保存已驻留队列的状态，三者分别管理。

> **[SOURCE]** 固定 Linux 源码：[amdgpu.h，第 444～458 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu.h#L444-L458)在 `amdgpu_fpriv` 中保存 `amdgpu_vm`；[amdgpu_kms.c，第 1435～1477 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L1435-L1477)在文件打开时创建并初始化该 VM。[amdgpu_amdkfd_gpuvm.c，第 1564～1587 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1564-L1587)将已有 VM 转为计算 VM；本实验借鉴这种内存管理关系，AQL 提交路径继续单独保留。

### 2.2 一个对象同时提供 CPU 入口与 GPU 地址

Host 已为 X 申请对象，需要先写入内容，再把设备可用的地址交给 Kernel。三种数值分别使用：句柄供驱动查找对象，CPU 地址或上传入口供 Host 准备内容，`gpu_address` 供设备访问。下图以开启 GPUVM 为例。

```text
句柄 H_X → BO_X：保存大小、内存域和引用
             ├─ 存储记录 → 实际 VRAM 区间或 Host 一致性内存
             ├─ CPU 入口 → Host 准备、读取内容
             └─ VM_A 的整对象映射 → GPUVA、权限 → 生成 PTE
```

BO 与映射分开记录，但首版限制为一对象一映射。Host RAM 继续使用驱动分配的一致性内存，VRAM 继续由 `gpu_buddy` 分配；对象创建时固定内存域，之后不搬动。对象逻辑大小单独保存，映射向上对齐到 4 KiB，分配器还可能因对齐要求保留更大的存储块。交付新对象前清零整个实际分配，保证逻辑范围和映射尾部都不暴露上一对象的数据；上传、下载仍以逻辑大小校验。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_vm.c，第 1843～1879 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1843-L1879)通过 `bo_va` 关联 BO 与 VM，检查映射重叠，并单独保存 `start`、`last`、`offset` 和 `flags`。本期保留对象与映射分离的关系，并限制为整对象单一映射。

AMDGPU 还通过 TTM 管理允许的存储位置及当前资源。本期只保留 GEM 对象和固定存储；以后若要移动 X，就需要同步旧访问、搬运内容、重写 PTE 并使旧翻译失效。仅使用 `gpu_buddy` 分配显存时，没有这些迁移能力。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_object.h，第 103～115 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_object.h#L103-L115)保存允许域、placement、内嵌 TTM 对象与 VM 关联；[include/drm/ttm/ttm_bo.h，第 73～125 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/include/drm/ttm/ttm_bo.h#L73-L125)区分存储资源与驱动管理的 GPU 映射。这里用于说明未接入 TTM 的边界，不要求本期实现迁移流程。

### 2.3 沿用对象存放位置，保护管理区域

用户 Ring、进度、参数和 Signal 仍放在 Host RAM；描述对象、代码和数组仍放在 VRAM。它们统一用 `gpu_address` 引用：开启时先翻译，关闭时直通；物理入口根据地址范围选择 VRAM 或 Outbound。

KIQ/HIQ Ring、MQD、管理结果区、IH、页表和固件由驱动管理，应用不能把这些区域映射成普通对象。通知页是用户态创建的普通 Host 对象，创建时登记给驱动，见[第 6.1 节](#gpuvm-signal)。

> **[SPEC]** [CP/MEC 第一版规格，内存对象](<./CP_MEC 第一版规格.md#spec-objects-addresses>)、[Kernel 数据存储](<./CP_MEC 第一版规格.md#spec-kernel-data-storage>)与[固件保留区](<./CP_MEC 第一版规格.md#spec-firmware-layout>)，2026-10-19 状态：普通 VRAM 为前 60 MiB，首个页保留；末尾 4 MiB 固件区不参与普通分配。第三阶段页表也从驱动管理的存储中分配并登记占用。

<a id="gpuvm-addresses"></a>

## 3. GPUVA、页表与地址翻译

### 3.1 48 位 GPUVA 与同地址隔离

开启 GPUVM 时 GPUVA 有效 48 位，位 63..48 必须为零。客体内核配置为 `CONFIG_ARM64_VA_BITS_48=y`，用户态地址同样在 48 位以内，所以对象的 CPU 地址可以直接作为 GPUVA，AQL 包、参数块和 Signal 中的 64 位地址字段原样保存。位 63..48 非零的地址（例如带标签的 ARM64 指针）报 RANGE 故障。

GPUVA 零页不分配，可用区间为 `[0x1000, 0x1_0000_0000_0000)`，上界 `GPGPU_VM_VA_LIMIT` 等于 2^48（256 TiB）。能力查询在开启时返回这一个区间和 48 位地址，关闭时返回零个区间和 32 位 GPU 物理地址。

以下地址仅用于说明两个进程的隔离关系：

```text
Queue A → VMID 1 → 页表 A
  GPUVA 0x0000_ffff_8a40_0000 → VRAM 偏移 0x0020_0000 → X_A

Queue B → VMID 2 → 页表 B
  GPUVA 0x0000_ffff_8a40_0000 → VRAM 偏移 0x0060_0000 → X_B
```

两个进程各有自己的 CPU 地址空间，同一个地址数值可以分别指向 X_A 和 X_B。两个 Kernel 都读取 `0x0000_ffff_8a40_0000`，设备根据所属 VMID 选择不同页表。填写其他进程使用过的地址数值，仍然只能查询当前 VM 的映射。

### 3.2 一次请求怎样找到目标页

设备已拿到 VMID、GPUVA、长度和访问类型，接下来查找允许访问的实际存储：

```text
检查 VMID 已绑定且上下文就绪
  → 查该 VMID 的 TLB
  → 未命中时，从页表根逐级读取表项
  → 检查有效位、读/写/取指权限及完整目标范围
  → GPU 物理页地址 + 页内偏移 → 公共物理入口 → VRAM 或 Outbound
  → 返回数据或报告本次访问错误
```

页大小固定为 4 KiB，表项为 64 位小端。采用四级页表，每张表按 4 KiB 对齐并占 4 KiB，共 512 项。驱动的表页按需分配在受保护 VRAM，用户不能改写；设备也能读取放在 Host 内存中的表页，由指向它的中间项置 SYSTEM 说明。

```text
48 位 GPUVA
  ├─ [63:48]：必须为 0
  ├─ [47:39]：L0 根目录索引，9 位
  ├─ [38:30]：L1 目录索引，9 位
  ├─ [29:21]：L2 目录索引，9 位
  ├─ [20:12]：L3 叶页表索引，9 位
  └─ [11:0] ：页内偏移，12 位

根目录 GPU 物理地址 → L1 目录 → L2 目录 → L3 叶页表 → GPU 物理页与权限
```

例如 `0x0000_ffff_8a40_2000` 的四个索引依次为 `0x1ff`、`0x1fe`、`0x52`、`0x2`，页内偏移为 0。首次建立这条路径需要四张表，共 16 KiB；同一 2 MiB 区段内的页复用已有叶页表。四级、每级 9 位与 AMD 9.4.3 的 48 位 VA 划分相同，见下方证据。

目录项保存 bit 0 `VALID`、bit 1 `SYSTEM` 和下一级表的地址域。叶子项在此基础上再保存 bit 4 `EXECUTABLE`、bit 5 `READABLE`、bit 6 `WRITEABLE`。GPU 物理地址仍为 32 位，地址域为 `[31:12]`，掩码为 `0xffff_f000`；bit 2、bit 63..32 及其余未定义位保留，有效项中出现即报页表错误。

SYSTEM 决定地址域的含义，设备据此换算出 GPU 物理页，再交给公共物理入口：

```text
叶子项 0x4100_2000 | SYSTEM | VALID | R | W
  └─ SYSTEM=1：地址域是 Host IOVA 0x4100_2000
       → 检查整页在 2 GiB IOVA 窗口内
       → GPU 物理页 0x8000_0000 + 0x4100_2000 = 0xc100_2000 → Outbound

叶子项 0x0001_1000 | VALID | R | X
  └─ SYSTEM=0：地址域是 VRAM 偏移 0x0001_1000
       → 检查整页在 VRAM 内，且避开首页和固件区
       → GPU 物理页 0x0001_1000 → VRAM

叶子项 0xc100_2000 | VALID | R
  └─ SYSTEM=0，但地址域不在 64 MiB VRAM 内 → 页表错误
```

地址域与 SYSTEM 不一致时报页表错误，设备不按地址数值去猜它属于 VRAM 还是 Outbound。最后一个例子是版本 3 的写法：当时 PTE 直接保存 GPU 物理地址，bit 1 必须为 0。版本 4 把 bit 1 定义为 SYSTEM，Host 页的叶子项改存 IOVA 并置 SYSTEM。

驱动给 Host 对象和通知页写叶子项时，直接使用 DMA API 返回的 IOVA 并置 SYSTEM；VRAM 对象写 VRAM 偏移。对象的 GPU 物理地址只用于 GPUVM 关闭时的直通访问和管理域字段。页表根没有 SYSTEM 位，上下文寄存器直接保存根页的 GPU 物理地址；根页位于 Host 时，Outbound 未使能则不能启用上下文。遍历通过公共物理入口读取表页，不递归进行 GPUVM 翻译。驱动只从受保护 VRAM 分配表页，写出的中间项都不带 SYSTEM。

完整表项写入前校验对齐、位宽及目标范围，不能靠掩码隐藏非法地址。无效项写零，不支持的有效项编码报页表错误。代码页开放读与取指，参数和输入页开放读，输出及共享状态按需要开放读写；不同权限的对象分开占页。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_gart.h，第 35～38 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.h#L35-L38)定义 4 KiB GPU 页；[amdgpu_vm.h，第 57～68 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h#L57-L68)定义 AMD 的有效、区域与权限位；[amdgpu_gmc.c，第 163～175 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c#L163-L175)将地址和标志写入 8 字节表项；[gmc_v9_0.c，第 1985～1994 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1985-L1994)设置 48 位地址掩码，并对包含 9.4.3 的分支选择 48 位 DMA 能力。[amdgpu_vm.c，第 2392～2425 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2392-L2425)按空间大小计算层级及末级大小；[gmc_v9_0.c，第 1936～1946 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1936-L1946)为 9.4.3 调用 `amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48)`，即 48 位 VA、块大小 9 位、最多 3 级目录加一级叶表。本设备的四级划分与此相同；32 位物理地址域和固定层数是本设备设计，不声明与 AMD 硬件格式兼容。

> **[SOURCE]** SYSTEM 位的参考：固定 Linux 源码 [amdgpu_vm.h，第 57～68 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h#L57-L68)把 bit 1 定义为 `AMDGPU_PTE_SYSTEM`；[amdgpu_vm.c，第 1200～1209 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1200-L1209)映射系统内存时取页的 DMA 地址，映射显存时给偏移加上 VRAM 基址；[gmc_v9_0.c，第 1019～1024 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1019-L1024)只对不带 SYSTEM 的目录项做显存地址换算。gem5 固定提交 `f5c5a6e390f55dd5984977815bf9d0bd05da6945` 的 [pagetable.hh，第 51～75 行](https://github.com/gem5/gem5/blob/f5c5a6e390f55dd5984977815bf9d0bd05da6945/src/arch/amdgpu/vega/pagetable.hh#L51-L75)在 PTE 和 PDE 的 bit 1 都定义了 `s` 字段。本设备只借用“bit 1 区分系统内存与显存”这层关系；地址域保存 IOVA、2 GiB 窗口和 32 位物理地址是本设备设计，不据此推定 MI300X 的表项格式或 TLB 参数。

对象和块搬运可以跨页，每一页分别检查。单次标量访问要求自然对齐且不跨页，完整 32/64 位共享字段不能拆成部分写入。跨页操作后续失败时保留先前写入，不提供回滚。

### 3.3 Host DMA 与系统 IOMMU

驱动已取得 Host 对象的 DMA 地址，需要把它放入 GPU 可访问的物理空间。Outbound 窗口固定从 `0x8000_0000` 开始，与 VRAM 容量无关。设 VRAM 为 64 MiB，设备物理布局和换算如下：

```text
GPU 物理地址 [0x0000_0000, 0x0400_0000)   → VRAM
GPU 物理地址 [0x0400_0000, 0x8000_0000)   → 空洞，不可路由
GPU 物理地址 [0x8000_0000, 0x1_0000_0000) → Outbound

Host IOVA 0x4100_0000
  → 加 Outbound 起点 0x8000_0000
  → GPU 物理地址 0xc100_0000

用户访问 → GPUVM 开启时翻译，关闭时直通
  → 公共物理入口
      ├─ VRAM：访问设备存储
      ├─ 空洞：路由失败，按窗口未使能处理
      └─ Outbound：减 0x8000_0000 → Host IOVA → PCIe DMA
                                       └─ SMMUv3（启用时）→ Host 物理内存
```

Outbound 的 GPU 基址固定为 `0x8000_0000`，Host IOVA 基址为 0，窗口长度为 2 GiB。VRAM 改变容量时，Host 对象的 GPU 物理地址不变；VRAM 末端到窗口起点之间的空洞不可路由。访问空洞与访问未使能的窗口按同样的方式报错：用户访问记 GPUVM 故障，CP 管理域访问记队列错误，DMA 引擎置 `DMA_FAULT`。

BAR0 `0x0b00～0x0b10` 布局寄存器只读，驱动在 CP 启动前读回核对起点、长度和 Host 基址，再写 `0x0b14` CONTROL 并确认成功；写只读布局置 `OUTBOUND_STATUS.ERROR` 并锁存 `INVALID_CMD`；窗口由整个设备共用，运行期间拒绝重配置。窗口覆盖仅决定路由，Host 存储或 IOMMU 映射不存在仍会访问失败。

驱动统一设置 31 位 DMA mask，正好覆盖窗口对应的 2 GiB IOVA；`vram_size < 2 GiB` 的限制保证 VRAM 不伸进窗口。每次分配都用 64 位算术校验完整 DMA 区间，IOVA 0 合法，对应 GPU 物理地址 `0x8000_0000`。用户 Host BO、管理 Ring、MQD、完成区、IH 和通知页遵守同一规则；Host BO 独立使用 `dma_alloc_coherent()`，不建立大块连续池。原始 `dma_addr_t` 供 DMA API 映射和释放，转换后的地址供设备协议使用。

GPUVM 选择应用页表，SMMUv3 处理设备 DMA，不把 GPU VMID 当成系统 IOMMU 的进程标识。本轮保留 SMMUv3 功能，但只在 `IOMMU=none` 下运行既有三个计算示例，不追加 IOMMU 验收矩阵。

<a id="gpuvm-access-paths"></a>

## 4. CP、MQD 与执行器的上下文传递

### 4.1 先读管理 MQD，再按用户 VMID 访问

CP 需要先读取 MQD 才知道用户队列属于哪个地址空间。MQD 自身使用管理域 GPU 物理地址，用户 Ring、R 回报和 W 读取地址使用对象绑定返回的 GPU 地址。下图以开启 GPUVM 为例：

```text
HIQ 命令中的 MQD GPU 物理地址
  → CP 按管理域读取 MQD
  → 取得用户 VMID，以及 Ring / R / W 的 GPUVA
  → 装入 HQD → 后续取包和进度访问按该 VMID 翻译
```

保留 64 字节 MQD、现有 HQD 布局和 VMID 字段。管理 Ring、管理完成标记及 IH 统一使用管理域 GPU 物理地址；页表根放独立上下文寄存器。上下文未就绪或版本不匹配时拒绝装载，不按地址数值猜测所属域。

### 4.2 一次访存保存一次完整上下文

CP 现有访存窗口显式选择管理域或用户域。管理域直接进入物理入口；用户域按设备 GPUVM 状态翻译或直通，不通过 `VMID == 0` 推断访问域。用户 Ring、进度、描述对象、Signal 和通知槽全部走用户域；管理对象走管理域。CP 代码、栈和寄存器访问保持裸机方式。

每次请求开始时保存地址域、VMID、地址、长度和访问类型，跨进程消息与内存回调使用同一快照。切换队列后，在途请求不能重新读取已经变化的“当前 VMID”。翻译失败返回错误，禁止退回物理地址访问。

### 4.3 执行器使用真实参数地址和启动 CSR

<a id="gpuvm-executor"></a>

AQL 的 `kernel_object`、`kernarg_address`、`completion_signal` 统一使用用户 GPU 地址。描述对象为 24 字节：64 位代码入口、代码长度、参数块长度、`kernel_abi_version = 5` 和必须为 0 的保留字段，CP 拒绝其他版本和非零保留字段，再把入口的高、低 32 位分别写入 Dispatch 的 `KERNEL_ADDR_HI/LO`。

执行器是 RV64 解释器：整数寄存器与 PC 为 64 位，参数块每项占 8 字节，Kernel 用 `ld` 读出 64 位指针或标量。以向量加法为例，参数块依次是 X、Y、Z 的地址和元素数，Kernel 读元素数时访问 `a0 + 24`：

```text
CP 读取描述对象 → 保存代码、参数地址及长度、VMID、HQD 槽与 gpuvm_enabled
  → 新建 Warp 时初始化共享与各 Lane 的启动状态
  → 各有效 Lane：a0 = kernarg_address（64 位），pc = 代码入口
  → Kernel 执行 ld 读取 a0 + 24
      → GPUVM 开启：按本次 VMID 翻译
      → GPUVM 关闭：输入直接作为 GPU 物理地址
      → 公共物理入口读取参数中的数组地址
  → 数组读写与代码取指使用同一条用户访存路径
```

保留派发时的代码、参数范围与对齐检查，代码取指还检查声明范围；参数运行时按普通内存处理，不再限定固定窗口。任务分段继续时使用已保存的状态。

每 Warp 的 `startup_sgpr` 保存 Block 编号、Block 大小与 Grid 的 Block 数，各 3 个 32 位值；每 Lane 的 `startup_vgpr` 保存线程三维坐标。坐标按 X 维变化最快的顺序初始化，新 Warp 才重填。CSR `0xCC0～0xCCB` 依次提供线程编号、Block 编号、Block 大小与 Grid 大小的 X/Y/Z；读取通用寄存器被覆盖后，CSR 仍返回原启动值。保留 `mhartid = 0xF14`，仅接受 `CSRRS` 且 `rs1=x0`，未知 CSR、写入及其他形式报指令错误。`rd=x0` 仍校验后丢弃结果。

**[BOUNDARY]** 本轮只提供启动数据及只读接口，不增加可写 SGPR/VGPR、标量 ALU 或向量指令体系。RV64 CP 的代码、栈和本地窗口保持独立 CP 地址空间。完整启动接口见[CP/MEC 规格第 6.2 节](<./CP_MEC 第一版规格.md#spec-kernel-descriptor>)。

<a id="gpuvm-context-tlb"></a>

## 5. 队列驻留时绑定 VMID、同步更新与 TLB 失效

### 5.1 两个用户 VMID 随队列驻留绑定

管理队列使用编号 0，用户上下文编号为 1、2；访问域由显式参数决定。关闭 GPUVM 时仍保留用户编号用于队列来源和资源管理，不创建页表、不提供页权限隔离，也不限制为进程独占。

文件打开时建立软件 VM，首次对象绑定时建立页表，这一步不占 VMID。创建 Queue 时驱动才取一个空闲 VMID 写进 MQD，Queue 确认卸载后归还。VM 的数量因此不受 VMID 限制；两个编号限制的是同时驻留的 Queue，与两个用户 HQD 槽一致。例如进程 A、B、C 都已绑定对象：A、B 建队列时分别取得 VMID 1、2，C 建队列返回资源不足；A 销毁队列后，C 再建队列即可取得 VMID 1。没有空闲编号时不驱逐其他 VM，不轮换页表根。

```text
建 Queue   取空闲 VMID → 写页表根 → 失效整个 VMID → 启用上下文 → 写 MQD、MAP_QUEUES
卸载确认   消费旧 IH → 停用上下文 → 失效整个 VMID → 清页表根 → 归还 VMID
```

TLB 项按 VMID 标记，同一个编号会先后交给不同 VM，所以取得和归还时都失效整个 VMID。VM 不占编号时，TLB 中没有它的翻译，绑定和解绑只写页表项，不发失效；下次取得编号时的整体失效清除可能残留的旧项。

驱动完成页表及根地址配置、发布和必要的旧翻译清理后才使上下文就绪。用户不能修改根地址、VMID 绑定或管理上下文。

> **[SOURCE]** 固定 Linux 源码：[gfxhub_v1_2.c，第 42～61 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gfxhub_v1_2.c#L42-L61)按 VMID 配置页表根的低、高位；[第 328～365 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gfxhub_v1_2.c#L328-L365)配置上下文使能、页表深度及访问故障检查。本设备沿用“VMID 选择根与控制状态”的关系，裁剪实例数量和参数。

**[BOUNDARY]** 管理域直通是本设备简化。AMDGPU 的 VMID 0 也有自己的 GART/页表配置，本期不复制这套管理翻译。

> **[SOURCE]** 固定 Linux 源码：[gfxhub_v1_2.c，第 74～91 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gfxhub_v1_2.c#L74-L91)为 VMID 0 设置 GART/页表根，证明真实驱动的管理上下文也有自己的地址翻译配置。

### 5.2 Queue 存活期间同步修改页表

两种地址模式在 Queue 存活期间都可以绑定和解绑对象；本节页表更新与失效步骤仅用于开启 GPUVM 的情况。Ring 与进度对象由驱动的使用计数保护，Queue 卸载前解绑返回 `-EBUSY`；通知页登记期间同样不能解绑。驱动无法判断其他对象是否仍被任务使用，应用要先等引用它的任务完成、停止提交引用旧地址的包，再解绑。权限调整通过撤销后重新映射完成。驱动串行化队列创建、映射更新和 VM 销毁。

故障 Queue 不要求排空，沿第二阶段已有错误卸载协议确认相关访问结束、保存状态并解除绑定后，再撤销映射；无法确认则继续保留。清理故障队列不恢复原任务，也不要求新增复位功能。

> **[SPEC]** [CP/MEC 第一版规格，错误队列清理后重建](<./CP_MEC 第一版规格.md#spec-queue-error-rebuild>)与[队列卸载](<./CP_MEC 第一版规格.md#spec-unmap-drain>)，2026-10-19 状态：正常队列先排空；故障队列按错误协议清理，释放仍须等待访问和旧通知结束。

```text
应用确认没有任务再使用要解绑的对象（新建映射时无此步）
  → CPU 写页表，保证设备可见
  → 写 VPN 范围和 VM_INV_REQ：绑定、解绑只失效该对象的页，
    取得、归还 VMID 时失效整个 VMID；VM 未占 VMID 时不发失效
  → 设备检查请求和所选上下文，清除匹配的 TLB 项，
    阻止旧页表遍历重新填入旧翻译
  → 驱动读 VM_INV_ACK，确认只含本 VMID 的位
  → 允许后续任务使用新映射，或释放已撤销映射的引用
```

TLB 按 VMID 和 VPN（GPUVA 的 `[47:12]`，36 位）缓存叶子翻译，组织方式与替换顺序见[第 5.2.1 节](#gpuvm-tlb-lru)。

驱动每次只请求一个 VMID。VPN 有 36 位，范围的起点和终点各用一对 LO/HI 寄存器。以失效 VMID 1 中 GPUVA `0x0000_ffff_8a40_0000～0x0000_ffff_8a40_2fff` 为例：先把 `0xf_fff8_a400` 写入 `VM_INV_VPN_START_LO/HI`，把 `0xf_fff8_a402` 写入 `VM_INV_VPN_END_LO/HI`，再写 `VM_INV_REQ` 为 VMID 1 的位加 RANGE。设备在这次寄存器写入内清除 VMID 1 中 VPN 落在这一范围的项，`VM_INV_ACK` 读回 `0x2`。请求含保留位、位图为空，或范围非法（起点大于终点、终点不低于 2^36）时，设备整体拒绝，不清任何项，ACK 只置 ERROR。所选上下文有激活的队列时失效照常完成：设备在主锁下处理请求，此时没有进行到一半的翻译。失效结束保证后续请求使用更新后的翻译。

失败或结果未知时禁止该上下文继续创建 Queue，保留相关页表、映射和存储。本期不增加异步更新。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_vm_cpu.c，第 73～135 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c#L73-L135)提供 CPU 写表与发布路径；[gmc_v9_0.c，第 776～879 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L776-L879)通过失效请求与 ACK 等待完成。本设备采用 CPU 同步更新，并额外限制为卸载队列后更新，省去运行中页表依赖管理。

> **[SOURCE]** 按 VMID 位图请求、按位确认的参考：固定 Linux 源码 [gmc_v9_0.c，第 707～724 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L707-L724)用 `1 << vmid` 组成 `PER_VMID_INVALIDATE_REQ`；[第 810～875 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L810-L875)写请求寄存器并等待 ACK 中本 VMID 的位；[gc_9_4_3_sh_mask.h，第 10326～10343 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/include/asic_reg/gc/gc_9_4_3_sh_mask.h#L10326-L10343)与[第 10668～10671 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/include/asic_reg/gc/gc_9_4_3_sh_mask.h#L10668-L10671)给出 REQ、ACK 的位域。本设备沿用“VMID 位图请求、按位确认”的关系；RANGE 位、VPN 范围寄存器和同步完成方式是本设备设计，不与 AMD 寄存器逐位兼容。

<a id="gpuvm-tlb-lru"></a>

#### 5.2.1 TLB 的组织与替换顺序

设备翻译一个 GPUVA 时，先在 TLB 中找 VMID 与 VPN 都相同的项，找不到才遍历四级页表，再把结果存进 TLB。TLB 容量有限，一组存满后要决定替换哪一项，所以每组除了保存翻译项，还要记下各项的使用先后。

**[DESIGN]** TLB 共 64 组、每组 4 路，组号为 VPN % 64。每组的保存内容如下：

```text
VPN = GPUVA[47:12]，组号 = VPN % 64
  例：GPUVA 0x0040_2000 → VPN 0x402 → 第 2 组
      GPUVA 0x0000_ffff_8a40_2000 → VPN 0xf_fff8_a402 → 也在第 2 组

第 2 组
  ways[4]   4 个存储位置（路），每路保存一项翻译：
            有效位、VMID、VPN、GPU 物理页、R/W/X 权限
  order[4]  4 个路号按使用先后排名，下标是名次，值是路号：
            order[0] = 最近刚用过的路号（MRU）
            order[3] = 最久没用的路号（LRU），本组满了就替换它
```

翻译项填进 `ways[i]` 后一直留在第 i 路，每次使用只改变 `order` 中路号的排列。复位时各路都无效，`order = {0, 1, 2, 3}`。TLB 只缓存叶子翻译，不缓存中间表。

一次查找按下面三步进行：

1. **命中**：本组某一路有效，且 VMID、VPN 都相同。HITS 加 1，返回保存的物理页与权限，再把这一路移到 `order[0]`；
2. **未命中**：MISSES 加 1，遍历页表。遍历失败时报告故障，不写 TLB，`order` 也不变；
3. **填入**：遍历成功后，优先填入本组编号最小的无效路；四路都有效时替换 `order[3]` 记录的那一路，EVICTIONS 加 1。新填入的一路移到 `order[0]`。

把某一路移到 `order[0]` 时，原来排在它前面的路各后移一位，排在它后面的路不动。只有被移动的路原来位于 `order[3]` 时，`order[0..2]` 才整体后移。下面的例子只访问 VPN 5、69、133、197、261，它们的 VPN % 64 都是 5，全部落在第 5 组，逐步写出 `order` 的变化：

```text
复位后：4 路都无效                          order = {0, 1, 2, 3}

访问 VPN 5    未命中，填入路 0              order = {0, 1, 2, 3}
访问 VPN 69   未命中，填入路 1              order = {1, 0, 2, 3}
访问 VPN 133  未命中，填入路 2              order = {2, 1, 0, 3}
访问 VPN 197  未命中，填入路 3              order = {3, 2, 1, 0}
访问 VPN 261  未命中，4 路都有效：
              替换 order[3] 记录的路 0（原为 VPN 5），EVICTIONS 加 1
                                            order = {0, 3, 2, 1}
访问 VPN 133  命中路 2；路 2 原在 order[2]，
              排在前面的路 0、3 各后移一位，路 1 不动
                                            order = {2, 0, 3, 1}
```

失效只把匹配的项标为无效，不改 `order`。被清除的路在下一次填入时按“编号最小的无效路”优先使用，不看它在 `order` 中排第几。接着上例，若失效 VPN 133，路 2 变为无效而 `order` 仍为 `{2, 0, 3, 1}`；下一次第 5 组未命中会填入路 2，而不是替换 `order[3]` 记录的路 1，填入后路 2 照常移到 `order[0]`。

命中、未命中和替换三个计数由全设备共用，32 位回绕，复位时清零，失效不清零。组相联结构参照 ARM 的通用做法，组数、路数和替换策略是本设备的取值，不代表 MI300X UTCL1/UTCL2 的参数。

> **[SOURCE] 可选源码索引**
>
> - 组与项的结构：[hw/gpgpu/mem/gpuvm.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.h:31:1>) [第 31～55 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.h:31:1>)；
> - 查找、未命中计数与填入：[hw/gpgpu/mem/gpuvm.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:262:1>) [第 262～288 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:262:1>) 的 `vm_lookup()`；
> - 选择无效路或替换 `order[3]`：[第 210～236 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:210:1>) 的 `tlb_fill()`；
> - 失效只清有效位：[第 488～500 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:488:1>) 的 `tlb_invalidate()`。

下面的 `tlb_touch()` 维护排名，可以对照上例中“前面的路后移、后面的路不动”：

> **[SOURCE]** [hw/gpgpu/mem/gpuvm.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:199:1>) [第 199～208 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:199:1>)，协议版本 5 的实现。

```c
199	/* 把 way 移到 order[0] 成为 MRU，排在它前面的路依次后移一位。 */
200	static void tlb_touch(GPGPUVMTlbSet *set, uint8_t way)
201	{
202		unsigned int pos = 0;
203	
204		while (pos < GPGPU_VM_TLB_WAYS - 1 && set->order[pos] != way)
205			pos++;
206		memmove(&set->order[1], &set->order[0], pos);
207		set->order[0] = way;
208	}
```

第 204～205 行找出这一路当前的名次 `pos`；第 206 行只把 `order[0..pos-1]` 后移一位，`order[pos+1..3]` 保持原值；第 207 行把这一路放到 `order[0]`。上例命中路 2 时 `pos = 2`，移动的是路 0 和路 3。

### 5.3 结束旧访问和通知后才复用编号

VMID 在每次 Queue 卸载后复用。归还前，先确认 CP 已卸载旧队列、结束旧访问和通知处理，完成翻译失效并撤销旧页表绑定，再交给新 VM；新 VM 取得编号时还要再失效一次整个 VMID。不能只覆盖根地址。归还失败时 VM 进入失败状态，编号保留到会话清理。

IH 继续按源 MQD 查找原队列、上下文与事件。MQD 地址、事件编号和通知槽也各自等待旧引用结束后再复用，旧记录不能按同编号的新对象解释。

<a id="gpuvm-signal"></a>

## 6. 完成通知与基本故障退出

### 6.1 保留 Signal 协议，通知页由用户态创建

沿用 24 字节 Signal、64 位有符号值及 CP 单独执行 `1 → 0`。AQL 中的 Signal 地址与 Signal 中的 `event_mailbox_ptr` 使用所属上下文的 GPU 地址；开启时为 GPUVA，关闭时为 GPU 物理地址。

通知页按 GPU 地址空间划分：每个 VM 一页 4 KiB Host 内存，提供 512 个 8 字节槽，同一进程的各条 Queue 共用。KMT 库在每个进程里只保留一个连接，所以实际是每个进程一页。全设备只有一份的是 IH Ring，它由驱动持有。通知页不能全设备共用：CP 写通知槽时按来源队列的 VMID 翻译地址，这一页必须映射在该进程自己的页表里。

通知页由用户库在 `gpgpuKmtOpen()` 中创建，早于任何 Queue：

```text
gpgpuKmtOpen()（进程第一次打开）
  → BO_CREATE(Host, 4 KiB, flags = GPGPU_BO_EVENT_PAGE)
      驱动检查：一页 Host 对象，本会话还没有通知页
      驱动登记：vm->notify 指向它，持有一个使用计数，此后 UNMAP 返回 -EBUSY
  → 按第 1.2 节映射到 CPU 地址 va，再 MAP(va, 读写)
EVENT_CREATE(队列)
  → 驱动在通知页中分配槽 k，返回 mailbox = 通知页 GPU 地址 + 8k
会话关闭
  → 驱动释放事件，撤销通知页登记，再随普通绑定回收
```

驱动必须知道哪一页是通知页：创建事件时要算槽地址，IH 记录到达时要读槽内容核对，会话结束前这一页不能被解绑。登记并入 BO_CREATE，`struct gpgpu_bo_create` 中原本必须为 0 的 `reserved` 改名为 `flags`，不增加 ioctl。开启 GPUVM 时通知页按普通 Host 对象映射，叶子项保存它的 IOVA 并置 SYSTEM；关闭时 MAP 返回它的 Outbound 物理地址。MAP 通知页时权限必须包含写。创建事件只分配已有槽，不改变页表；通知页未登记、未绑定或槽已用完时返回失败。

> **[SOURCE]** 固定 Linux 源码：[kfd_priv.h，第 956～958 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L956-L958)在 `struct kfd_process` 中保存每个进程一页的 `signal_page`；[kfd_events.c，第 332～360 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdkfd/kfd_events.c#L332-L360)的 `kfd_event_page_set()` 每个进程只登记一次；[kfd_chardev.c，第 867～877 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L867-L877)说明独立显卡的事件页由用户态分配，句柄随第一次 CREATE_EVENT 交给 KFD；[第 1161～1181 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L1161-L1181)用 `KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL`、`MMIO_REMAP` 分配标志创建特殊用途对象。ROCr `ba56a24` 在 `hsa_init()` 中就创建第一个事件（[runtime.cpp 第 2063 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp:2063:1>)、[第 1745 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp:1745:1>)），libhsakmt 在这次调用中分配事件页（[events.c 第 77～89 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/events.c:77:1>)）。本设备的 EVENT_CREATE 要求来源队列已就绪，打开设备时还没有队列，所以借用 BO_CREATE 的标志登记，登记时机与 AMD 相当。

```text
驱动创建事件 → 返回事件编号和通知槽 GPU 地址
  → 应用填写 Signal 并提交 AQL
  → CP 按源队列用户域写 Signal 与通知槽，再提交 IH
  → 驱动核对源队列、事件关联和实际通知槽 → 唤醒等待者
```

开启 GPUVM 时，页权限以 VM 为边界，不增加 CP 专用 PTE 权限位；同一 VM 内的 Kernel 和应用自己的 CPU 代码都能写该 VM 的通知页，写坏只影响本会话的事件。驱动不能仅凭通知内容授予跨上下文访问或唤醒其他上下文的事件。

完成顺序保持为结果写回及执行确认、Signal、通知槽、IH、包槽失效与消费回报。后续步骤失败时保留已有写入，不重新执行整项任务。

> **[SPEC]** [CP/MEC 第一版规格，Signal 布局](<./CP_MEC 第一版规格.md#spec-signal-layout>)、[可见性](<./CP_MEC 第一版规格.md#spec-dispatch-visibility>)及[失败包收尾](<./CP_MEC 第一版规格.md#spec-failure-retirement>)，2026-10-19 状态：完整 64 位读写、发布顺序和部分完成保留均为已有约束。

### 6.2 缺页报错并停止，不补页或重放

检测未映射、权限错误、越界、无效上下文、页表或存储访问失败。故障信息至少定位源队列、VMID、原始 GPU 地址、访问类型和阶段，保留首次故障，读取不自动清除。

```text
CP 取包、描述或完成访问失败 → CP 记录源队列错误
执行器取指、参数或数组访问失败 → 返回失败 → CP 记录源队列错误
  → 停止该队列后续取包和派发 → 经健康 IH 通知驱动
  → 相关等待以错误退出 → 确认访问及引用结束后才能回收

IH 自身故障 → 锁存状态、拒绝新记录、停止新取包和调度
  → 独立 IRQ_ERROR 通知 Host，不再依靠故障 IH Ring
```

可归属单条用户队列的错误停止源队列，保留绑定；另一进程的健康队列继续运行。管理、固件或共享通道故障沿用第二阶段停止范围。页表更新失败则将受影响 VM 置为不可继续使用，直到安全清理。

> **[SOURCE]** 固定 Linux 源码：[gmc_v9_0.c，第 562～565 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L562-L565)从中断记录恢复故障地址；[第 633～679 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L633-L679)读取访问状态，并区分页表遍历、权限及映射错误。本设备借鉴诊断内容，队列停止及资源保留仍按自己的 CP/IH 协议处理。

> **[SPEC]** [CP/MEC 第一版规格，IH 故障与恢复前提](<./CP_MEC 第一版规格.md#spec-ih-fault-reset>)，2026-10-19 状态：IH 故障经已有 `IRQ_ERROR` 交付，不依赖继续写入故障 Ring。

例如数组跨页，已写回部分 Z 后读取 X 的下一页失败，已写入结果保留，尚未发布的成功 Signal 不更新。如果 Signal 已为 `0` 而后续通知失败，则保留 `0` 并报告通知故障，不能重跑任务。

**[BOUNDARY]** 缺页只做检测、报告和错误退出；不按需补页、不恢复故障指令、不自动重放 Dispatch。错误记录和队列停止后，仍须确认已发出的访问结束，才允许释放存储。

<a id="gpuvm-lifetime"></a>

## 7. 正常回收与异常资源保留

两种模式的绑定都持有 BO 引用，绑定状态独立于地址值和页表存在。Queue 持有 Ring、进度对象和上下文引用；CPU 映射、事件、管理请求和旧通知也保留必要引用。关闭句柄先解除句柄引用，实际存储等所有使用结束后回收。

普通关闭由库封装以下顺序，应用只需停止新提交并调用对应关闭操作：

```text
停止提交并完成已预留包的发布 → 等待排空、卸载 Queue
  → 消费旧 IH，结束旧通知处理 → 停用上下文、失效整个 VMID → 归还 VMID
  → 撤销绑定（VM 已不占 VMID，只清页表项）
  → 释放事件、对象引用、DMA 映射和实际存储
  → 归还页表；通知页随会话关闭由驱动撤销登记并回收
```

驱动不解析任意 Kernel 参数来猜测全部依赖，只保护 Ring、进度对象和通知页；其他对象由应用遵守输入、输出和 Signal 的使用寿命，确认任务结束后再释放。正常回收要能重复执行，不能靠每次重启设备清理。

故障队列按第 5.2 节清理，不等待所有包成功完成。进程异常退出、等待超时、管理结果未知或失效未确认时，保留设备仍可能访问的对象、页表和编号。允许返回错误并暂时占用资源，不增加自动复位或挂死恢复；超时只结束本次等待。

[第二阶段未知管理结果码问题](<./QEMU GPGPU 实验大纲.md#stage-two-spec-review>)已修复：完成标记匹配后仍须校验结果属于 `0～6`，未知值按协议错误保留资源。原独立注入结果作为历史记录保留，不再作为当前验收项目。强制中止与运行中复位恢复归第四阶段，不用它们替代正常寿命管理。

<a id="gpuvm-device-host-interface"></a>

## 8. 最小设备接口与 Host API

### 8.1 设备增加上下文、失效和故障入口

用户 VMID 1、2 各自保存页表根和使能状态；管理队列编号为 0；访问域由 CP 显式传递，不按编号推断。关闭 GPUVM 时用户编号仍保留，但不启用这些页表上下文。地址范围和层数固定。共享失效入口提供目标 VMID、忙、ACK 和错误状态。CP 窗口及执行器增加地址域或 VMID，故障入口按源 HQD 保存首次错误信息。

页表根与失效寄存器由 Host 驱动独占，应用不能映射管理寄存器或整个 VRAM BAR。上下文禁用时才能改根，先写完整配置、发布并清旧翻译，再启用。失效请求在写入时同步完成，没有忙状态；所选上下文有激活的队列时照常完成。

冷复位和整体复位清除 GPUVM 状态，包括 TLB、三个计数、ACK 与 VPN 范围；CP 单独复位保留上下文、TLB 与计数，仅清除 CP 访存窗口配置和固件握手，不能据此释放仍可能被执行器访问的存储。BAR0 的 `0x0900～0x09ff` 保存版本、模式、握手、失效请求、TLB 计数和上下文，`0x0a00～0x0aff` 保存故障；完整字段、访问权限及复位行为见[硬件手册第 17 章](<./GPGPU_硬件手册.html#gpuvm-registers>)。

### 8.2 库封装整对象分配、映射与释放

保留设备、内存、队列和事件四组调用。内存申请返回对象句柄、逻辑大小、`cpu` 与 `gpu_address`，内部顺序为预留 CPU 地址、创建 BO、把对象映射到这段 CPU 地址、同步 `MAP`（第 1.2 节），全部成功后才返回；中途失败按实际完成状态撤销或保留，不能遗留可误用的半成品。

`MAP` 是双向 ioctl，输入对象句柄、地址和权限，输出最终 `gpu_address`。开启时地址是库为对象申请的 CPU 地址，驱动校验归属、对齐、重叠与完整范围后建立页表；关闭时地址输入必须为 0，驱动返回对象固定的 GPU 物理地址。两者都建立独立绑定引用，已绑定对象再次绑定失败。

`UNMAP` 用绑定起始地址定位整对象，句柄、权限和保留字段填零，支持 GEM handle 已关闭的对象。开启时等待同步失效后才解除引用，关闭时直接解除绑定。Ring、进度对象和通知页仍被使用时返回 `-EBUSY`，访问结果未知时保留绑定与存储；CPU mmap 另有 GEM 引用，VRAM 对象的 `PROT_NONE` 映射也持有这一引用。创建 Queue 使用已绑定对象，不增加逐任务 Launch ioctl。

> **[SOURCE]** 固定 Linux 源码：[amdgpu_gem.c，第 811～886 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c#L811-L886)校验地址、权限、操作及文件所属对象句柄；[第 944～974 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c#L944-L974)区分映射、解除、清理与替换。本期只采用同步、整对象 `MAP/UNMAP` 子集。

DRM ioctl 的 version 字段及能力查询返回版本均为 5、地址位数（开启为 48、关闭为 32）、4 KiB 页、可分配 GPUVA 区间数量（开启为 1、关闭为 0）、两个用户编号、每上下文一 Queue 及当前绑定数量。默认应用不接触物理 DMA 地址。

库由 `gpgpuKmtAllocMemory()` 接收权限和固定 GPUVA 参数，`fixed_va=0` 时由库申请 CPU 地址并以同一数值作为 GPUVA；代码对象必须显式添加取指权限。`gpgpuKmtGetVmInfo()` 查询能力，`gpgpuKmtGetVmFault()` 查询尚持有的队列对象的首次故障。关闭队列前先保存故障；队列对象销毁后不能再用其编号查询。内存、队列和事件结构须保持有效直至显式释放或 `gpgpuKmtClose()` 完成。

<a id="83-单一-drm-入口与版本-2"></a>

### 8.3 单一 DRM 入口与协议版本

两种状态都只开放 DRM render node。驱动读取设备的 `gpuvm` 属性后固定使用，删除 `queue_mode`、`/dev/gpgpu0`、旧同步提交和旧 Runtime。设备／固件队列协议、DRM 与 GPUVM 编码均为版本 5，旧组件明确拒绝，不增加兼容转换。AQL 包仍为 64 字节，Kernel 描述对象为 24 字节。

公共 HSAKMT 风格库承担槽位预留、Header 发布、8 字节 Doorbell 更新、Signal 等待与事件配合、排空和 Queue 销毁；示例通过 `lib/rt/` 调用 `lib/kmt/`，统一链接 `libgpgpu.a`。应用只使用 `cpu` 和 `gpu_address`，不选择 Host DMA 地址、VRAM 偏移或 GPUVA 三套字段。

<a id="gpuvm-open-interfaces"></a>

## 9. 本轮范围记录与完成条件

### 9.1 首版方案的关键限制

以下保留会影响应用使用和实现规模的核心取舍，正文已经按同一组合展开，便于集中审阅：

<a id="gpuvm-confirm-objects"></a>
<a id="gpuvm-confirm-connection"></a>
<a id="gpuvm-confirm-storage"></a>

- **对象与存储：** 单一 DRM 入口，一个文件一个 VM；GEM 包装固定 Host RAM / VRAM，不接入 TTM。库封装整对象申请与映射，一对象一映射。

<a id="gpuvm-confirm-addresses"></a>
<a id="gpuvm-confirm-va-width"></a>
<a id="gpuvm-confirm-executor"></a>
<a id="gpuvm-scope-rv32"></a>
<a id="gpuvm-confirm-pages"></a>
<a id="gpuvm-confirm-page-format"></a>
<a id="gpuvm-confirm-memory-access"></a>
<a id="gpuvm-confirm-host-dma"></a>

- **地址与页表：** 48 位 GPUVA 等于对象的 CPU 地址，GPU 物理地址仍为 32 位；RV64 执行器、真实 `a0`、只读启动 CSR；开启时使用四级 4 KiB 页表和读写取指权限，关闭时不创建页表；两者共用同一物理布局：VRAM 在低端，Outbound 固定从 `0x8000_0000` 开始，中间的空洞不可路由。

<a id="gpuvm-confirm-access"></a>
<a id="gpuvm-confirm-address-fields"></a>
<a id="gpuvm-confirm-mqd"></a>
<a id="gpuvm-confirm-cp-window"></a>
<a id="gpuvm-confirm-contexts"></a>
<a id="gpuvm-confirm-vmid"></a>
<a id="gpuvm-confirm-reuse"></a>
<a id="gpuvm-confirm-invalidate"></a>
<a id="gpuvm-scope-live-update"></a>

- **驻留与更新：** 管理 GPU 物理地址直通，两个用户 VMID 在 Queue 驻留时绑定，每 VM 一条 Queue；Queue 存活期间也可以改映射（Ring、进度对象和通知页除外），CPU 同步写表并等待失效完成：VM 占有 VMID 时绑定和解绑按对象的 VPN 范围失效，取得和归还 VMID 时按 VMID 全量失效；TLB 为 64 组 × 4 路。

<a id="gpuvm-confirm-completion"></a>
<a id="gpuvm-confirm-signal"></a>
<a id="gpuvm-confirm-fault"></a>
<a id="gpuvm-confirm-release"></a>
<a id="gpuvm-confirm-host-api"></a>
<a id="gpuvm-confirm-host-map"></a>
<a id="gpuvm-confirm-compatibility"></a>
<a id="gpuvm-confirm-observability"></a>

- **完成与退出：** 保留 Signal/IH，通知页由用户态在打开设备时创建并登记；用户访问故障停止源队列并错误退出，不恢复或重放；访问、通知和失效都结束后才能回收和复用。

以上取舍已用于本版实现。更多页表形式、迁移框架、运行中更新和故障恢复移出本期；后续若需改变接口或约束，先说明原因和影响。

### 9.2 明确留到后续的能力

本期不支持存储迁移与驱逐、超额显存驻留、换页、按需补页、故障恢复与任务重放、VMID 超额轮换、异步页表更新、别名与部分撤销、跨进程共享、CP 专用通知权限、抢占及运行中复位恢复。

这些能力的作用和扩展安排集中在[后续 GPUVM 扩展](<./QEMU GPGPU 实验大纲.md#gpuvm-advanced-extension>)。当前只需讲清固定存储、由应用确认对象寿命和显式失败分别替代了什么条件，不展开完整实现设计。

### 9.3 当前核心计算与历史验收

当前唯一计算验证入口为 `test-gpgpu-stack`。固定 ARM virt、A57、1 GiB 客体内存和 `IOMMU=none`，在 `gpuvm=on`、`gpuvm=off` 下分别执行同一组现有计算：向量加法 17,003 个结果、19×17 与 17×23 矩阵乘法的 437 个结果、ReLU 513 个结果。保持原逐元素比较规则，任一计算失败返回非零退出码。

代码、描述对象和数组放 VRAM；参数、Ring、进度、Signal 等 Host 对象经 Outbound 访问。所有对象先绑定再创建 Queue，正常结束按排空、销毁 Queue、事件清理、解绑和释放的顺序执行。

2026-10-19 的多进程隔离、边界、故障注入和生命周期验收保留为[历史记录](<../software/gpgpu/README.md#stage-three-validation>)。相应专项测试与旧组合回归已从当前入口移除，本轮不新增 CSR、Outbound、协议或生命周期专项验收，不扩展数据规模或存储位置组合。
