# dcache 第一版规格

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| dcache | Data Cache | 数据缓存；本文指设备共享的数据缓存 |
| icache | Instruction Cache | 指令缓存；本版不实现，取指路径为它保留 |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| GPU / CPU | Graphics Processing Unit / Central Processing Unit | 图形处理器／中央处理器 |
| CP | Command Processor | 命令处理器 |
| RV64 | 64-bit RISC-V | 本实验原生 CP 与计算执行器使用的 64 位 RISC-V 模型 |
| GPUVM / GPUVA | GPU Virtual Memory / GPU Virtual Address | GPU 虚拟内存／GPU 虚拟地址 |
| VMID | Virtual Memory Identifier | 设备虚拟地址空间标识 |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| VRAM / RAM | Video RAM / Random Access Memory | 设备显存／系统内存 |
| BAR | Base Address Register | PCI 设备基址寄存器 |
| DMA | Direct Memory Access | 直接内存访问 |
| IOVA / IOMMU | I/O Virtual Address / Input/Output Memory Management Unit | 设备 DMA 地址／输入输出内存管理单元 |
| PCIe | Peripheral Component Interconnect Express | 高速外设互连总线 |
| IH | Interrupt Handler | 本实验的完成与错误通知模块 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| BO | Buffer Object | 持有固定存储的缓冲对象 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| KMT | Kernel Mode Thunk | 本仓库 `lib/kmt/` 薄层的简称 |
| LRU / MRU | Least Recently Used / Most Recently Used | 最久未用／最近使用 |
| MMIO | Memory-Mapped I/O | 内存映射输入输出 |
| TCG | Tiny Code Generator | QEMU 动态翻译后端 |
| AMDGPU | AMD GPU Linux Kernel Driver | AMD GPU Linux 内核驱动 |
| MTYPE | Memory Type | AMDGPU 页表项中的内存类型字段 |
| ISA | Instruction Set Architecture | 指令集架构 |

本文维护教学设备数据缓存第一版的已确认设计。寄存器偏移、位域与复位值见[硬件手册第 19 章](<./GPGPU_硬件手册.html#dcache-registers>)，协议升级差异见 [PROTOCOL_V6.md](<../software/gpgpu/PROTOCOL_V6.md>)，代码位置、构建命令和实际验证结果见[软件栈说明](<../software/gpgpu/README.md#build-and-validation>)。协议常量以 `software/gpgpu/include/gpgpu/` 为准。

**[BOUNDARY]** 这是自定义教学设备的缓存模型，不声明复现 MI300X 的缓存层级或默认内存类型。与 AMD 的对照见[第 10 节](#dcache-amd)。

<a id="dcache-overview"></a>

## 1. 目标与范围

### 1.1 一次向量加法经过的缓存

以 `Z[i] = X[i] + Y[i]` 为例。应用把 X、Y 上传到 VRAM，在 Host 参数块里写入三个数组的地址，再向 Ring 发布 AQL 包。CP 读描述对象和 Signal，RV64 计算执行器读 X、Y、参数块，写 Z。没有 dcache 时，这些访问每次都直接读写 VRAM 或 Host RAM；参数块在 Host RAM 中，每个线程读一次参数都要经 vfio-user 往返一次。

dcache 加在 GPUVM 翻译之后。CP 与执行器访问同一 GPU 物理行时共用一份数据，Host 数据重复访问时也只在未命中时经 PCIe 读取。整个任务的数据流如下：

```text
应用上传 X、Y ── DMA 写 VRAM 前，设备先回写并失效相关缓存行
  → 应用写参数块（可缓存的 Host 对象），用 CACHE_SYNC 失效设备中的旧副本
  → 应用发布 AQL 包（Ring、W 不可缓存，CP 每次都读内存）
  → CP 读描述对象：经 dcache，未命中时回填
  → 执行器读 X、Y 和参数，写 Z：经 dcache，Z 留在脏行中
  → 执行器结束 → CP 回写整个 dcache → 写 Signal、通知槽，提交 IH
  → 应用看到 Signal 归零，下载 Z ── DMA 读 VRAM 前，设备先回写脏行
```

图中 DMA 前的维护由设备完成，任务结束时的回写由 CP 固件完成。应用只在 CPU 直接改写可缓存对象之后调用维护接口，见[第 6 节](#dcache-handoff)。

### 1.2 本版包含与不包含的内容

**[DESIGN]** 本版提供一份设备级共享数据缓存：32 KiB 数据容量、64 字节行、4 路组相联、True LRU 替换、Write-Back 加 Write-Allocate。CP 用户数据窗口和 RV64 计算执行器的 load/store 使用它，普通 VRAM 数据和普通 Host 数据都可以缓存。

本版不实现指令缓存、多级缓存、预取、MSHR 和周期时序，也不实现多份私有缓存之间的一致性协议。访问仍按现有模型同步完成，执行器继续逐 warp 推进。CPU、DMA、BAR 与缓存之间的可见性由缓存属性和软件维护保证，设备不自动跟踪 CPU 写入。

<a id="dcache-paths"></a>

## 2. 访问路径与缓存范围

用户访问先完成地址翻译和本次权限检查，再按 GPU 物理地址选择数据缓存或取指路径：

```text
CP 用户窗口 / RV64 计算执行器
  │ GPUVA、VMID、READ / WRITE / EXEC
  ▼
GPUVM：查 TLB，必要时遍历页表，检查 R/W/X 权限
  │ GPU 物理地址
  ├─ READ / WRITE → 查不可缓存区间
  │                  ├─ 可缓存 → dcache 组相联查找
  │                  └─ 不可缓存 → 直接读写后备存储
  │
  └─ EXEC → 直接读后备存储；以后接独立的 icache
                         │
              共用原始 GPU 物理路由
                ├─ VRAM → 后备 RAM
                └─ Outbound → Host IOVA → vfio-user DMA

页表遍历、CP 管理域、DMA、IH ─▶ 直接读写后备存储
CP 自身的指令、全局变量和栈 ─▶ 原生 RAM 别名（见第 8 节）
```

沿图说明几条规则：

- **先翻译，再查缓存。** 缓存命中不能代替本次权限检查。`gpuvm=off` 时输入已经是 GPU 物理地址，仍先检查 VRAM 首页、固件保留区和可路由范围。
- **物理标签。** 标签不含 GPUVA、CP 本地窗口地址或 VMID。两个用户地址映射到同一 GPU 物理行时，读写同一份缓存数据。
- **Host 数据。** Host 数据以 Outbound 窗口内的 GPU 物理地址作标签。Outbound 把它换算成 IOVA，系统 IOMMU 可以继续翻译；缓存行存活期间，对应 IOVA 映射必须有效。
- **取指不进入 dcache。** 取指检查代码范围与执行权限后直接读后备存储，不分配数据行，也不改变 dcache 的替换顺序和统计。
- **原始访问方。** 页表遍历和 CP 管理域本来使用 GPU 物理地址，不再经过 GPUVM。它们与 DMA、IH 直接读写后备存储，回填与回写也只走这条路由，不会再次进入缓存。

> **[SOURCE] 访问分流**
>
> - 翻译后按访问类型分流：[gpuvm.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:295:1>) [第 295 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:295:1>) 的 `user_phys_rw()`，由[第 360 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:360:1>)（`gpuvm=on`）与[第 391 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:391:1>)（`gpuvm=off`）在权限检查之后调用；
> - 数据访问入口：[dcache.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:182:1>) [第 182 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:182:1>) 的 `gpgpu_dcache_rw()`。

<a id="dcache-geometry"></a>

## 3. 几何参数与地址拆分

容量、行大小和相联度集中定义在 `gpgpu_regs.h`，组数、位宽和掩码都由它们推导：

- 数据容量 32 KiB，行大小 64 字节，4 路；
- 组数 = 32 KiB ÷ 64 ÷ 4 = 128，缓存行总数 = 32 KiB ÷ 64 = 512。

GPU 物理地址为 32 位，按下面拆分：

```text
地址位   31 ........ 13 | 12 ..... 6 | 5 ..... 0
用途          Tag      |    组号    |  行内偏移
位数           19      |     7      |     6
```

例：地址 `0x0123_4568` 所在行的起点是 `0x0123_4540`，行内偏移 `0x28`，组号 `0x15`，Tag `0x91a`。同一组的相邻行相隔 128 × 64 = 8 KiB，`0x0123_6540`、`0x0123_8540` 也落在组 `0x15`。替换和维护时，回写地址由旧行的 Tag 与组号重建，不用新请求的地址。

缓存状态分三个数组：

- `data[组][路][字节]` 保存实际数据，共 32 KiB；
- `meta[组][路]` 每行一个 32 位值，低 19 位是 Tag，其上是 VALID 与 DIRTY，共 2 KiB；
- `order[组][路]` 按 MRU 到 LRU 保存路号，每项 1 字节，共 512 字节。

32 KiB 只指数据容量，三个数组合计 34.5 KiB，统计、命令和不可缓存区间另计。缓存属于整个设备，不随某个任务、队列或 VMID 创建和销毁。复位后各行无效，各组的 `order` 为 `{0, 1, 2, 3}`。

<a id="dcache-replacement"></a>

## 4. 查找、替换与失败处理

缓存控制器在主锁下同步执行，不建立新线程，也没有异步未命中。一个请求跨行时逐行处理，跨页由 GPUVM 拆开。

### 4.1 命中与 True LRU

先算组号，再逐路比较 VALID 和 Tag。读命中复制请求的字节；写命中修改对应字节并置 DIRTY。成功后把这一路移到 `order[0]`，原来排在它前面的路依次后移，数据阵列不搬动。

以组 `0x15` 为例，四次未命中依次把 A、B、C、D 填进第 0～3 路，`order` 变为 `{3, 2, 1, 0}`，即 D 最近、A 最久。再读 A 时命中第 0 路，`order` 变为 `{0, 3, 2, 1}`。下一次未命中的 E 没有空路可用，替换 `order` 末尾的第 1 路，也就是 B；E 写入后 `order` 为 `{1, 0, 3, 2}`。

### 4.2 未命中、回填与回写

1. 优先选编号最小的无效路；四路都有效时选 LRU 路。
2. 旧行有效且脏时，按旧行地址回写整行。回写成功才清 DIRTY；失败时旧行保持有效且脏，本次请求失败。
3. 把新行读进临时缓冲。读失败时旧行的 Tag 和数据都不变，第 2 步已经完成的回写也不回退。
4. 读到完整的新行后才写入数据与 Tag。读请求得到干净行；写请求先保留整行原有内容，再改写本次的字节并置 DIRTY。整行写入也走同一流程。
5. 新行成为 MRU。只有替换了有效旧行时才计一次替换。

请求的后续行失败时，前面已经完成的行保留，整个请求不当作原子操作。设备不会留下“新 Tag + 旧数据”的半成品行。现有自然对齐的 4、8 字节共享字段不跨行，在主锁下一次读写完成。

> **[SOURCE] 查找与替换**：[dcache.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:119:1>) [第 119 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:119:1>) 的 `dcache_fill()` 实现第 4.2 节的步骤，[第 155 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:155:1>) 的 `dcache_line_rw()` 处理命中、写入置脏与 MRU 更新，[第 65 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:65:1>) 的 `dcache_touch()` 维护替换顺序。

<a id="dcache-uncached"></a>

## 5. 普通数据与持续共享的控制对象

### 5.1 控制对象不能缓存

Ring、W/R 进度、Signal 和通知页由 CPU 与 CP 持续交替读写。CP 每轮轮询 W 与 Header，Host 可能在 CP 运行时随时发布新包。如果这些对象可以缓存，CP 读到的 W 可能一直是旧值，新包永远不会被取走。

整行回写还会覆盖 CPU 的新数据。现行协议允许 R、W 落在同一条 64 字节行内：

```text
缓存行（CP 一侧）     内存（CPU 一侧）
W=10  R=9             W=10  R=9
                      CPU 发布新包：W=11
CP 回收一个包：R=10    W=11  R=9
整行回写               W=10  R=10   ← CPU 写入的 W=11 被覆盖
```

所以这类对象整体不可缓存，CP 每次都读写内存中的字段。

### 5.2 不可缓存区间表

**[DESIGN]** 设备保存一张不可缓存的 GPU 物理区间表，表外地址默认可以缓存。数据访问完成翻译和权限检查后，按行起点查这张表，`gpuvm=on` 与 `gpuvm=off` 使用同一规则，页表项不再保存另一份缓存属性。一段物理地址的所有用户地址别名因此属性相同。

区间表只接受 Host 的登记与撤销，规则如下：

- 起点与长度按缓存行对齐，长度非零，区间完整落在 VRAM 或 Outbound 窗口的固定布局内；
- 不能与已登记的区间重叠；撤销时必须与登记时的区间完全相同；
- 登记前，设备先回写并失效区间内已经缓存的行，回写失败时不登记；
- 表项数为 `GPGPU_DCACHE_UC_RANGES`，表满时登记失败。

设备只保存物理范围，不保存 GEM 句柄或对象引用。整体复位清空区间表。

### 5.3 登记哪些对象

驱动在把对象交给设备之前登记：

- **驱动自己的控制对象**：KIQ、HIQ 的 Ring 与进度页，各队列的 MQD，管理请求结果区，IH Ring。CP 管理域和 IH 本来就直接读写后备存储；登记后，`gpuvm=off` 时用户域经物理地址访问它们也不会产生缓存副本。
- **用户的控制对象**：带 `GPGPU_BO_UNCACHED` 创建的 BO。通知页（`GPGPU_BO_EVENT_PAGE`）总是不可缓存。`QUEUE_CREATE` 要求 Ring 与进度对象都不可缓存，否则返回 `-EINVAL`。Signal 由用户库放进不可缓存对象，驱动不知道 Signal 的位置，不做检查。

对象的缓存属性在创建后不变。登记失败时对象不发布；对象释放时，驱动在存储仍有效时撤销登记。

> **[SOURCE] 区间表与登记**
>
> - 设备登记规则：[dcache.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:306:1>) [第 306 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:306:1>) 的 `dcache_uc_add()`；
> - 驱动控制对象：[gpgpu_dev.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:117:1>) [第 117 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:117:1>) 的 `gpgpu_ctrl_buf_alloc()`；
> - 用户对象：[gpgpu_bo.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:265:1>) [第 265～278 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:265:1>)；Ring 与进度的检查见 [gpgpu_queue.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:91:1>) [第 91 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:91:1>)。

<a id="dcache-handoff"></a>

## 6. 维护命令与软件交接

### 6.1 回写与失效

维护命令作用于与给定物理范围重叠的整行，也可以作用于整个缓存：

- **回写**：把脏行写回后备存储，行保持有效，之后可以继续命中；
- **失效**：丢弃干净行。遇到脏行时命令失败并返回 DIRTY，设备不会静默丢弃尚未写回的数据；
- **回写并失效**：先回写，成功后再失效。

维护不改变替换顺序，也不计命中与未命中；成功回写的脏行计入回写次数。遇到第一个失败的行即停止，之前已维护的行保持新状态。回写失败时该行仍有效且脏，设备锁存 `ERROR_STATUS.DCACHE_FAULT`。

小范围维护按组号与 Tag 直接定位，BAR 的单次写入不必扫描整份缓存；范围覆盖的行数不少于缓存行总数时，扫描一遍全部行。

### 6.2 Host 与 CP 的命令通道

Host 经 BAR0、CP 经本地寄存器窗口访问同一组寄存器。两方各有一份地址、长度和结果：Host 先写地址和长度，CP 在两次写之间插入自己的命令，也不会改掉 Host 的参数。写命令寄存器时设备同步执行，随后读到的状态就是本次结果。

CP 只能发维护命令，登记和撤销区间只接受 Host。驱动的所有 Host 命令都在设备锁下串行发出。仍有在途物理访问时，设备拒绝命令并返回 BUSY；vfio-user DMA 同步等待期间不释放主锁，这项检查只作防御。

### 6.3 CPU 与 GPU 之间的交接

可缓存数据以整条缓存行为单位交接，CPU 与 GPU 不能同时修改同一行的不同字节。以运行时的参数块为例，上一次派发结束时 CP 已回写缓存，参数块所在行在缓存中是干净的：

```text
CPU 写入本次参数
  → CACHE_SYNC 失效参数块在设备中的旧副本，确认完成
  → 发布 AQL 包（Publish 中的屏障保证参数先于 Header 可见）
  → 执行器读参数：未命中，从内存回填本次的值
  → 执行器写结果，留在 dcache 脏行中
  → 执行器结束，CP 回写整个 dcache 并确认成功
  → CP 发布 Signal
  → CPU 等到 Signal 归零后读取结果
```

CPU 修改一段数据之前，GPU 对它的访问必须已经结束、脏数据已经写回。CPU 写完后再回写 GPU 的旧脏行，会用旧内容覆盖 CPU 的新字节，所以交接的顺序不能颠倒。

CP 固件的 `fence` 与用户库的 `gpgpuKmtHostFence()` 只保证访存顺序，不会触发缓存维护。Host BO 仍使用 `dma_alloc_coherent()` 分配；这份 dcache 由设备模拟，一致性内存的 CPU 侧机制不负责它，交接依靠上面的维护命令。

### 6.4 驱动与用户库的接口

- `BO_CREATE` 的 `GPGPU_BO_UNCACHED` 标志创建不可缓存对象；
- `CACHE_SYNC` 按对象内的范围回写或失效。驱动把范围换算成对象的 GPU 物理地址，用户态不能维护任意物理范围。对象存储按页对齐，按整行处理不会触及其他对象。不可缓存对象直接返回成功；只失效时遇到脏行返回 `-EBUSY`；
- 用户库 `gpgpuKmtAllocMemory()` 增加 `flags` 参数，`gpgpuKmtSyncMemory()` 封装 `CACHE_SYNC`；
- 运行时把 Ring、进度页和 Signal 建为不可缓存对象。参数块和暂存区可以缓存：参数块每次派发前失效，暂存区只经 COPY 访问，由设备的 DMA 维护两端。

<a id="dcache-bypass"></a>

## 7. 旁路访问、释放与复位

直接读写后备存储的一方遵守同一条规则：读之前回写重叠的脏行，写之前回写并失效重叠的行。维护失败时不开始原始访问。

### 7.1 BAR2

BAR2 读之前回写重叠的脏行，再读后备存储。BAR2 写之前回写并失效重叠的行，再精确写入请求的字节，同一行的其余字节保持最新值。写入仍经 VRAM 的内部地址空间，原生 CP 已翻译的代码照常失效。驱动对页表项、对象清零和固件的写入都经过这条路径，之后的页表遍历和数据访问都读到新值。

### 7.2 DMA

DMA 两端都直接读写后备存储，两端都要维护：

- Host 到 VRAM：Host 源回写，VRAM 目的回写并失效；
- VRAM 到 Host：VRAM 源回写，Host 目的回写并失效。

维护完成后才映射 VRAM、开始搬运；维护失败时 DMA 置 ERROR，不搬运。搬运在写寄存器的同一次调用中完成，期间不释放主锁，其他客户端不会在维护之后重新缓存这两段地址。

### 7.3 对象释放与存储复用

驱动释放可缓存 BO 的存储前，先对整个对象回写并失效；不可缓存 BO 撤销区间登记。维护失败时，设备可能仍缓存这段物理地址，驱动把存储转入保留列表，到设备释放时才归还，不分给新对象。

设备移除时，驱动先停止接受请求，再整体复位设备；复位的第 3 步会回写缓存中残留的脏行。以一个已解绑但仍有脏行的 Host 对象为例：

```text
驱动拒绝新请求（present 清零）
  → 应用此时关闭对象：驱动不再发维护命令，存储转入保留列表
  → 驱动整体复位设备：脏行回写到仍属于这个对象的存储
  → 设备释放：确认复位完成后，归还保留列表中的存储
```

如果第二步直接归还存储，第三步的回写会落进已经交还给内核的内存。所以设备移除后释放的可缓存对象都先保留。不可缓存对象登记时已清掉区间内的旧行，之后不会产生缓存行，也不会被复位回写。

### 7.4 Outbound 关闭与整体复位

Outbound 关闭后，窗口内的缓存行再也无法回写。所以关闭前先回写并失效窗口内的全部行，失败时窗口保持打开，写入被拒绝。

整体复位按下面的顺序执行：

1. 有在途物理访问时拒绝复位；
2. CP 保持复位，执行器停止，不再产生新访问；
3. 在 Outbound 和后备存储仍有效时回写并失效全部行。回写失败时停止复位：数据留在缓存中，`DCACHE_FAULT` 保持锁存，已停止的 CP 与执行器不自动恢复；
4. 清空 GPUVM、HQD、IH 等状态；
5. 复位全局寄存器、中断、DMA、Outbound 与 dcache，清空区间表和统计。

保留 VRAM 的复位因此不丢失已完成的写入。QEMU 冷复位在第 3 步成功后才清零 VRAM，不会在清零之后又写回旧脏行。CP 单独复位不清缓存、区间表或统计；CP 保持复位并不代表执行器已经停止。

### 7.5 任务完成与错误

执行器给出终态时，线程已经停止访问内存。CP 固件随后发出“回写整个缓存”的命令并确认成功，干净行和替换顺序保留，供后续任务继续命中。之后才按原顺序发布 Signal、通知槽、IH、Header 与消费进度。

执行失败时，CP 同样先回写，失败前已经完成的写入对 Host 可见，再记录队列错误。回写失败时，CP 记 `EXECUTOR` 错误，不发布成功的 Signal。用户访问中的回填或替换回写失败，沿原访问记 GPUVM 的 `STORAGE` 故障。ACK 只确认执行结果，不清缓存或统计。

> **[SOURCE] 旁路与收尾**
>
> - BAR2：[vram.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/vram.c:15:1>) [第 15 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/vram.c:15:1>)、[第 33 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/vram.c:33:1>)；DMA：[dma.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dma.c:78:1>) [第 78～83 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dma.c:78:1>)；
> - Outbound 关闭：[phys.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/phys.c:106:1>) [第 106 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/phys.c:106:1>)；整体复位：[gpgpu.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/gpgpu.c:73:1>) [第 73 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/gpgpu.c:73:1>)；
> - 对象释放：[gpgpu_bo.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:48:1>) [第 48 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:48:1>)、[第 64 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:64:1>)；
> - 固件收尾：[aql.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/firmware/aql.c:272:1>) [第 272 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/firmware/aql.c:272:1>) 的 `aql_clean()`，由[第 368 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/firmware/aql.c:368:1>)在执行器终态之后、发布完成之前调用。

<a id="dcache-cp-native"></a>

## 8. CP 原生访存与固件保留区

CP 有两条访存路径，只有经访存窗口的数据访问会进入 dcache：

```text
原生 RV64 CP
  ├─ 固件取指、全局变量、栈
  │    → CP 本地 RAM 别名 → VRAM 后备存储（不经 GPUVM，不经 dcache）
  │
  └─ cp_read* / cp_write* 访问 GPU 对象
       → CP 本地 64 KiB 访存窗口
       ├─ 用户域 → GPUVM → 数据访问前端（dcache 或不可缓存）
       └─ 管理域 → 直接读写后备存储
```

固件代码、数据和栈位于固定保留区 `[0x03c0_0000, 0x0400_0000)`。用户域访问这段地址时被 GPUVM 拒绝，管理域又不经过 dcache，所以固件区的字节不会有缓存副本，dcache 也不需要按地址范围排除它。

CP 在固件区原生写入临时 IH 记录，设备 IH 模块直接读它；驱动经 BAR2 装载固件。两者都读写后备存储，按原有发布顺序进行。

原生 RAM 别名覆盖整个 VRAM，复位向量仍沿用现有入口规则。现有固件经访存窗口访问公共 GPU 对象；自定义 CP 程序若用原生指针访问可缓存对象，必须先完成第 6 节的交接，不能与 dcache 同时访问同一行。本版不提供强制隔离。

代码由数据写入产生时，执行前要先回写 dcache，以后加入 icache 时再失效 icache。经 BAR2 或 DMA 上传的代码由旁路维护处理。本版不支持执行期间修改自身代码，取指前也不自动回写 dcache。QEMU 原生 CP 的 TCG 翻译状态不作为本设备的 icache。

<a id="dcache-stats"></a>

## 9. 统计口径

四个统计计数全设备共用，32 位无符号回绕：

- **命中**：可缓存数据请求按行查找命中的次数；
- **未命中**：按行查找未命中的次数，之后回填失败也算一次；
- **替换**：成功用新行替换有效旧行的次数，填入空路和主动失效都不算；
- **回写**：成功写回脏行的次数，包括替换、维护和任务收尾，失败的回写不算。

取指、页表遍历、不可缓存访问、DMA、IH、回填与维护本身都不计命中和未命中；入口范围检查失败的请求也不计数。

命中率由读取者计算：命中 ÷（命中 + 未命中），分母为 0 表示还没有样本。统计区间用前后两次读数之差，求和使用足够的位宽。运行中只保证每个寄存器单独读取的值，需要准确采样时在没有新数据访问时读取。整体复位清零统计；CP 复位、ACK、TLB 失效和普通维护不清零。

<a id="dcache-amd"></a>

## 10. 与 AMD MI300X 的对照

本设备允许普通 Host 数据进入写回缓存，是按教学目标选择的模型。AMD 的对应行为需要分开看实际的驱动选择和硬件能力。

> **[SOURCE]** Linux 固定提交 `248951ddc14de84de3910f9b13f51491a8cd91df`，[gmc_v9_0.c，第 1101～1158 行](https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1101-L1158)：GC 9.4.3 的独立 GPU 分支中，不属于本地 VRAM 的系统内存对象选择 `MTYPE_UC`，并设置 snoop 位。

这段代码说明，固定 Linux 基线为 MI300X 对应的独立 GPU 系统内存选择了不可缓存类型。它不能说明硬件在任何配置、任何缓存层级都不能缓存系统内存。

> **[SPEC]** AMD Instinct MI300 ISA，封面日期 2025-08-05，§9.1.10.2（原文第 73～75 页）与 §10.1（原文第 79 页）：Flat 地址空间包括系统内存，访存指令可用 `SC[1:0]`、`NT` 控制各级缓存的命中与旁路；`BUFFER_WBL2`、`BUFFER_INV` 写回或失效 GPU 缓存。

规范中的 system scope 是同步作用域，不表示数据位于 Host RAM。访存经过缓存路径也不意味着数据一定驻留缓存。规范里的维护指令只作用于 GPU 缓存，不负责 CPU 缓存，也不能单独保证 CPU 与 GPU 之间的可见性。

**[BOUNDARY]** 本文的不可缓存区间表、维护命令和交接顺序都是本设备的设计，用来在这份自定义缓存上保证正确性，不是 AMD 硬件接口的复现。

<a id="dcache-implementation"></a>

## 11. 实现位置

> **[SOURCE] 实现索引**
>
> - 缓存核心：[dcache.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.h:1:1>)、[dcache.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:1:1>)；维护命令的范围处理见 dcache.c [第 245 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:245:1>)，Host／CP 两方命令见[第 368 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/dcache.c:368:1>)；
> - 寄存器与协议常量：[gpgpu_regs.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_regs.h:441:1>) [第 441 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_regs.h:441:1>) 起；用户接口：[gpgpu_drm.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/uapi/gpgpu_drm.h:261:1>) [第 261 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/uapi/gpgpu_drm.h:261:1>)；
> - CP 本地寄存器窗口的转发：[bar0.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/bar0.c:196:1>) [第 196 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/bar0.c:196:1>)、[第 223 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/bar0.c:223:1>)；
> - 驱动命令通道：[gpgpu_dev.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:80:1>) [第 80 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:80:1>)；`CACHE_SYNC`：[gpgpu_bo.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:471:1>) [第 471 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:471:1>)；
> - 运行时的对象属性与参数块失效：[runtime.c](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/rt/runtime.c:95:1>) [第 95 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/rt/runtime.c:95:1>)、[第 379 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/rt/runtime.c:379:1>)。

核心行为由现有 QTest 与 Linux 全栈覆盖，命令与实际结果只记录在[软件栈说明第 5 节](<../software/gpgpu/README.md#build-and-validation>)。
