# Linux 驱动内存管理

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPU | Graphics Processing Unit | 图形处理器 |
| CPU | Central Processing Unit | 中央处理器 |
| QEMU | Quick Emulator | 仿真与虚拟化工具 |
| AMDGPU | AMD GPU Linux Kernel Driver | AMD GPU Linux 内核驱动 |
| KFD | Kernel Fusion Driver | AMD GPU 计算相关内核驱动组件 |
| ROCr | ROCm Runtime | ROCm 的异构计算运行时 |
| GC | Graphics and Compute | AMD 图形与计算硬件模块，9.4.3 对应 MI300 |
| GPUVM / GPUVA / VA | GPU Virtual Memory / GPU Virtual Address / Virtual Address | GPU 虚拟内存／GPU 虚拟地址／虚拟地址 |
| VM / VMID | Virtual Memory / Virtual Memory Identifier | GPU 地址空间／硬件地址空间上下文编号 |
| PASID | Process Address Space Identifier | 进程地址空间标识 |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| PDE / PTE | Page Directory Entry / Page Table Entry | 页目录项／页表叶子项 |
| VPN | Virtual Page Number | 虚拟页号，即 GPUVA 除以 4 KiB |
| HDP | Host Data Path | Host 经 BAR 访问显存的通路，带写缓存 |
| CP / MEC | Command Processor / Micro Engine Compute | 命令处理器／计算命令处理微引擎 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| AQL | Architected Queuing Language | 异构系统架构定义的队列包格式 |
| IH | Interrupt Handler | 中断记录 Ring 与分发模块 |
| DRM / GEM / TTM | Direct Rendering Manager / Graphics Execution Manager / Translation Table Maps | Linux 图形设备框架／对象管理／存储放置与迁移框架 |
| BO | Buffer Object | 保存实际存储及其属性的缓冲对象 |
| GTT | Graphics Translation Table | AMD 中可被 GPU 访问的系统内存域 |
| SDMA / IB | System DMA / Indirect Buffer | AMD 的 DMA 引擎／间接命令缓冲区 |
| DMA / IOVA | Direct Memory Access / I/O Virtual Address | 直接内存访问／设备访存使用的地址 |
| IOMMU / SMMUv3 | Input/Output Memory Management Unit / System Memory Management Unit version 3 | 输入输出内存管理单元／Arm 系统内存管理单元第三版 |
| BAR | Base Address Register | PCI 基址寄存器 |
| VRAM / HBM | Video RAM / High Bandwidth Memory | 本设备显存／MI300X 的高带宽显存 |
| RAM | Random Access Memory | 随机访问内存，本文用于客体主存 |
| CMA | Contiguous Memory Allocator | Linux 的连续内存分配器 |
| API | Application Programming Interface | 应用编程接口 |
| VMA | Virtual Memory Area | CPU 进程的虚拟内存区域 |
| ACK / REQ | Acknowledgment / Request | 失效完成确认／失效请求寄存器 |

本文讲解本项目 QEMU 教学设备的 Linux 驱动内存管理，不属于待确认规格。应用执行向量加法 `Z[i] = X[i] + Y[i]`，驱动需要分配 GPU 可访问的存储、建立地址映射，并在访问结束后安全回收。下文沿 X 数组和 kernarg 参数块的寿命，讲解 DRM/GEM 对象、引用、锁和页表，再与 AMDGPU 的对应做法对照。

阅读基线：

- 本设备：提交 `ec9a9c00a5e9562437cff455b494f35a9d9c3289`，协议版本 5。存储分配以[第 3 节](#memory-storage)标注的 Git blob（文件内容版本）为准，其余本仓库源码行号对应上述提交。
- AMD 对照：外部 Host CPU + MI300X 独立 GPU（GC 9.4.3），Linux `248951ddc14de84de3910f9b13f51491a8cd91df`、ROCr `ba56a24c6132c5d195686ae4adf969ca1222fbba`。

**[BOUNDARY]** 本设备是自定义教学设备，简化的存储、页表和同步模型不作为 MI300X 的硬件结论。文中的地址和数组规模是教学取值，不是实测日志。指向父仓库源码与笔记的链接只在本仓库作为 gpu_study 子模块检出时可用。

接口约定见 [GPUVM 规格](<./GPUVM 第一版规格.md>)和 [CP/MEC 规格](<./CP_MEC 第一版规格.md>)；构建、运行与验证结果见[软件栈第 5 节](<../software/gpgpu/README.md#build-and-validation>)。

<a id="memory-lifecycle"></a>

## 1. 阅读顺序与对象寿命总图

驱动中与内存有关的代码，建议按下面的顺序读：

1. `gpgpu_drv.h` 的文件头，以及 `struct gpgpu_bo`、`struct gpgpu_vm`：对象关系、引用种类和锁的顺序；
2. `gpgpu_bo.c`：对象创建、句柄关闭、CPU 映射和 COPY；
3. `gpgpu_vram.c`：用 gpu_buddy 管理的 VRAM；
4. `gpgpu_dev.c` 中的 `gpgpu_dma_buf_alloc()`：Host 内存的分配和 Outbound 地址换算；
5. `gpgpu_vm.c`：页表、MAP/UNMAP、VMID 的取得与归还、会话清理；
6. `gpgpu_drm.c` 中的 `gpgpu_postclose()`：文件关闭时能否回收，不能回收时转为 orphan。

先分清三个动作：BO_CREATE 取得后备存储，MAP 为这份存储建立 GPU 地址绑定，建队列才让设备使用这个 VM。用户库在 BO_CREATE 前先申请 CPU 地址，再把 BO 映射到同一地址；开启 GPUVM 时，这个数值也作为 GPUVA。关闭 GEM 句柄只撤销应用查找对象的一个入口；对象是否释放，还要看绑定、mmap 等引用是否结束。

下面取 `gpuvm=on`、4 KiB 页。X 有 17,003 个 `float`，GPUVA 为 `0x0000_ffff_8a40_0000`，存储从 VRAM 偏移 `0x0020_0000` 开始；kernarg 本次保存 X、Y、Z 的地址和元素个数，共四个 64 位值、32 字节。图中的申请与绑定顺序由用户库完成：

```text
申请 CPU 地址  mmap(PROT_NONE) 取得对齐地址 va
               X.va = 0x0000_ffff_8a40_0000；kernarg 另取一段地址
                        ↓
BO_CREATE      X：buddy 保留 0x1_1000 字节连续 VRAM 并分段清零
                  phys_addr = 0x0020_0000，映射长度为 17 页
               kernarg：dma_alloc_coherent 分配并清零，映射长度为 1 页
                  phys_addr = 0x8000_0000 + IOVA
               两个对象各有会话列表引用和 GEM 句柄引用
                        ↓
CPU mmap       用 BO 的 mmap_offset、MAP_FIXED 替换 va 处的占位
               X：PROT_NONE，只保留地址；kernarg：可读写
               两者的 mmap 都持有 GEM 引用
                        ↓
GPU MAP        先准备完整表路径 → 登记绑定并取引用 → 写叶子 PTE
               X：17 个 VRAM PTE；kernarg：1 个带 SYSTEM 的 Host PTE
               gpu_addr = va；VM 此时未占 VMID，不发范围失效
                        ↓
准备内容       X：COPY 从 Host 暂存 BO 搬入；kernarg：CPU 直接写四个参数
                        ↓
建队列         取得 VMID 1：写根 → 失效整个 VMID → 启用上下文
队列存活       可以 BO_CREATE、MAP、UNMAP 与当前任务无关的对象
               应用保证任务仍使用的数组、参数和代码不被解绑或释放
                        ↓
单对象释放     确认没有任务再使用 X → UNMAP → munmap → 关闭句柄
               有 VMID：清 PTE 后完成范围失效，再放下绑定引用
整会话结束     停止提交、排空并卸载队列 → 消费旧 IH
               → 停用上下文、整体失效、归还 VMID → 清理剩余对象与页表
最后引用结束   bo_free() 归还后备存储
```

通知页在这些普通对象之前准备：库在 `gpgpuKmtOpen()` 中用 `GPGPU_BO_EVENT_PAGE` 创建一页 Host BO，再完成 CPU mmap 与 GPU MAP。驱动把它登记到 `vm->notify`；`vm_init()` 只准备地址空间，开启 GPUVM 时分配根表，不负责创建通知页。图中的队列存活期允许修改无关映射，但 Ring、进度对象和已登记通知页仍受使用计数保护。

> **[SOURCE]** 本仓库 `ec9a9c0`：先申请 CPU 地址、再创建对象并同址映射，见 [memory.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:42:1) [第 42～70 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:42:1)、[第 109～178 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:109:1)；打开连接时创建通知页，见 [device.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/device.c:197:1) [第 197～212 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/device.c:197:1)；驱动登记通知页，见 [gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:198:1) [第 198～229 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:198:1)。

<a id="memory-objects"></a>

## 2. 对象模型、引用与锁

**本设备。** 应用通过 DRM render node 发出 ioctl，GEM 提供句柄、对象引用计数和 mmap 入口。驱动的 BO 把 GEM 对象与实际存储放在一起：BO 管“哪份存储还活着”，VM 管“某个 GPUVA 应找到哪份存储”。下面是只保留本节核心成员的简化定义，用于说明关系，不是完整源码摘录：

```c
struct gpgpu_bo {
	struct drm_gem_object gem; /* GEM 引用计数保护对象寿命。 */
	u64 size;                 /* 应用请求的字节数。 */
	u64 mapped_size;          /* 向上对齐到页的映射长度。 */
	u64 phys_addr;            /* 固定存储的 GPU 物理地址。 */
	u64 gpu_addr;             /* MAP 后提供给 Kernel 的地址。 */
	bool bound;               /* 是否持有 GPU 绑定。 */
	u32 permissions;          /* GPU 访问权限。 */
	u32 users;                /* 队列基础对象、通知页登记或 COPY 的占用计数。 */
};

struct gpgpu_vm {
	u32 vmid;                 /* 当前借用的槽位；0 表示未借用。 */
	u32 root;                 /* 页表根在 VRAM 中的偏移。 */
	bool ready;               /* 地址空间已准备；开启 GPUVM 时已有根表。 */
	bool failed;              /* 禁止继续修改或建队列。 */
	struct gpgpu_bo *notify;   /* 用户库创建、驱动登记的通知页。 */
};
```

完整 BO 还记录 Host／VRAM 种类，以及 `host` 或 `vram_blocks` 中的存储信息。对 X 来说，`phys_addr` 一直是 VRAM 偏移 `0x0020_0000`，MAP 后的 `gpu_addr` 是 CPU 占位地址 `0x0000_ffff_8a40_0000`；GPUVA 映射把二者联系起来。`gpuvm=off` 时没有页表，但仍建立绑定并持有引用，此时 `gpu_addr` 使用固定的 `phys_addr`。

对象寿命由四类长期引用保护：会话列表、GEM 句柄、GPU 绑定和 CPU mmap。会话列表持有初始化 GEM 时取得的引用，其余三类在各自建立时增加引用计数；最后一个引用结束时才调用 `bo_free()` 归还存储。例如应用先关闭 X 的句柄，绑定引用仍在，X 的页表目标就不会提前被分配给别人。

```text
会话列表引用 ── 管理本会话拥有的 BO ───────┐
GEM 句柄引用 ── 让应用按 handle 查找 BO ──┤
GPU 绑定引用 ── 保住 GPUVA 指向的存储 ────┼→ GEM 引用计数 → 最后归零时 bo_free()
CPU mmap 引用 ─ 保住映射关联的 BO ────────┘
```

Host 对象解绑后，CPU mmap 仍能保活其存储；VRAM 的 `PROT_NONE` mmap 虽然不能读写内容，也持有同样的对象引用。

`users` 解决的是另一个问题：**现在能否撤销绑定**。队列占用 Ring、进度对象，通知页仍在登记，或 COPY 正在使用对象时，计数非零，UNMAP 必须拒绝。`users` 本身不是 GEM 引用；驱动同时保留会话列表或绑定引用，让这些对象继续存活。句柄关闭回调只有在对象未绑定且 `users == 0` 时，才放下会话列表引用。

驱动没有逐项追踪任意 Kernel 的参数和数组。X 的 `users == 0` 只说明没有上述驱动已知占用；应用还要等使用 X 的任务结束，并停止提交引用旧 X 地址的新包，才能解绑 X。队列可以继续存在，并运行不使用 X 的任务。

设备级 `dev->lock` 保护对象状态，外层 `vm_serial` 将建队列、MAP、UNMAP 等操作串行化。等待设备完成时，驱动会释放 `dev->lock`，让 IH 处理取得锁并报告完成；这期间仍持有 `vm_serial`，阻止另一个线程建队列或改映射。

`vm_serial` 只覆盖一次 ioctl，不随队列持续持有。ioctl 返回后队列可继续运行，后续 BO_CREATE、MAP、UNMAP 也可进入。

创建 GEM 句柄还有一个独立的竞争点：`drm_gem_handle_create()` 返回前，句柄已对其他线程可见，另一个线程可能立即关闭它；关闭回调 `bo_close()` 又需要 `dev->lock`。所以创建方先取临时对象引用，再解锁创建句柄，重新加锁后检查对象是否已被关闭。解锁避免回调死锁，临时引用保证对象在这段时间不会被释放。

> **[SOURCE]** 本仓库 `ec9a9c0`：
>
> - 对象关系、引用与锁顺序：[gpgpu_drv.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:17:1) [第 17～29 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:17:1)、[第 108～124 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:108:1)、[第 206～214 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:206:1)；
> - ioctl 的外层串行化与内层对象锁：[gpgpu_drm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drm.c:122:1) [第 122～147 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drm.c:122:1)、[第 213～225 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drm.c:213:1)；
> - 句柄关闭时检查绑定和使用计数、创建句柄时取得临时引用：[gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:71:1) [第 71～82 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:71:1)、[第 236～258 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:236:1)；
> - 通知页登记期间取得一个使用计数：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:525:1) [第 525～542 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:525:1)。

**AMDGPU。** AMDGPU 把存储和映射分开得更细。`amdgpu_bo` 内嵌 TTM 的 `ttm_buffer_object`，TTM 对象再内嵌 GEM 对象；GEM 管对象寿命与用户接口，TTM 管当前存储位置以及搬移。BO 记录首选域、允许域（VRAM、GTT 等）和 placement（放置约束），因此可以在允许的域之间迁移。

在某个 VM 中，`amdgpu_bo_va` 关联 BO 和这个地址空间，下面可以有多段 `amdgpu_bo_va_mapping`。每段映射记录起止页号、BO 内偏移和权限等标志，挂在 VM 的区间树上：

```text
VM 的区间树 → 某段 GPUVA 映射 → amdgpu_bo_va → BO → TTM 当前存储
                起止页号          关联 BO 与 VM      HBM 或系统内存页
                BO 内偏移、标志
```

同一个 BO 因而可以在不同地址建立多段映射。`amdgpu_bo_va` 的 `queue_refcount` 非零时，说明队列基础资源还在使用这份映射，不能解除；它也不是每次 Kernel 访存的计数。KFD 再用 `kgd_mem` 管理计算内存对象及其在各 GPU 上的映射。

后文反复出现的 **fence** 是异步操作的完成凭据：操作尚未结束时，依赖它的一方等待；操作结束后 fence 被置为完成，也可以携带错误。比如 SDMA 搬运 X 后返回一个 fence，下一步必须等它完成，才能按新位置使用 X。fence 本身不搬数据，也不直接修改页表。

内存对象的同步以 reservation 为单位：BO 的 `dma_resv` 同时提供锁和 fence 容器，VM 的页表 BO 共享根目录的 reservation。锁使用 ww_mutex 协议；一次操作要取得多个对象锁时，用同一个 ww_acquire 上下文协调冲突，必要时退让重试，避免互相等待形成死锁。这部分内存管理不靠一把覆盖所有会话的设备级大锁。

> **[SOURCE]** Linux `248951dd`：`amdgpu_bo_va_mapping`、`amdgpu_bo_va` 及 `queue_refcount` 见 [amdgpu_object.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.h:64:1) [第 64～101 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.h:64:1)；`amdgpu_bo` 见[第 103～128 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_object.h:103:1)。延伸阅读：父仓库笔记 [06_AQL 应用的完整运行流程（上）](<../../../1.笔记/06_AQL 应用的完整运行流程：驱动初始化、执行与资源释放（上）.md>) §0.3。

> **[SOURCE]** Linux `248951dd`：TTM 内嵌 GEM 对象并保存当前 placement 资源，见 [ttm_bo.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/include/drm/ttm/ttm_bo.h:101:1) [第 101～125 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/include/drm/ttm/ttm_bo.h:101:1)；`dma_resv` 的 ww_mutex 和 fence 容器，见 [dma-resv.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/include/linux/dma-resv.h:155:1) [第 155～179 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/include/linux/dma-resv.h:155:1)；页表 BO 共用根目录 reservation，见 [amdgpu_vm_pt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:469:1) [第 469～479 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:469:1)。

**取舍。** 本设备用一把锁保护主要对象状态，省去了逐个 BO 加锁的组合问题，代码更容易跟读；`vm_serial`、DMA 锁和管理锁仍须遵守规定的锁顺序。全设备只有两个**同时驻留的用户队列**，不是只能创建两个 VM。所有会话的内存操作仍会在设备锁上串行，大量会话并发申请内存时会受限制。AMD 用 per-BO 的 reservation，让无关对象的操作可以并行，同时协调搬移、驱逐和提交中的对象依赖。

<a id="memory-storage"></a>

## 3. 存储分配与对齐

**本设备。** BO_CREATE 就为整个对象取得存储，之后不迁移、不驱逐。VRAM 对象占一段连续显存，Host 对象占一段连续 DMA 地址；有 IOMMU 时，连续 DMA 地址背后的物理页可以分散。应用申请的 `size` 先按页取整为 `mapped_size`，驱动再取 `max(mapped_size, alignment)` 作为分配请求长度。起点对齐与保留长度分别计算。

以 X 为例，取 4 KiB 页、对齐要求不超过一页。17,003 个 `float` 共 68,012 字节，页对齐后需要 17 页（`0x1_1000`）：

```text
BO_CREATE：请求长度 17 页
  ├─ VRAM：size = 17 页，起点对齐 = 32 页
  │    → gpu_buddy 内部取得 32 页块，裁掉尾部 15 页
  │    → 对象保留连续的 17 页，尾部可供其他对象分配
  │    → 只清零保留的 17 页，释放时归还实际块列表
  └─ Host：把 17 页交给 dma_alloc_coherent()
       → DMA API 保证地址对齐，实际物理占用由底层分配路径决定
```

下面的函数只确定请求长度并选择存储类型；VRAM 的起点对齐由 `gpgpu_vram_alloc()` 单独计算：

> **[SOURCE]** 本仓库 `gpgpu_bo.c` 的 Git blob `a8cc6777369a02a537f135e4c4f8ad8cb82ca16c`：[gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:151:1) [第 151～163 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:151:1)，存储长度的计算。

```c
151	/* 请求长度至少覆盖对象与对齐要求，底层分配器负责内部取整。 */
152	static int bo_storage_alloc(struct gpgpu_bo *bo, u64 alignment)
153	{
154		u64 size = max_t(u64, bo->mapped_size, alignment);
155		int ret;
156
157		if (bo->kind == GPGPU_MEMORY_HOST) {
158			ret = gpgpu_dma_buf_alloc(bo->dev, &bo->host, size, alignment);
159			bo->phys_addr = bo->host.addr;
160			return ret;
161		}
162		return gpgpu_vram_alloc(bo->dev, size, &bo->vram_blocks, &bo->phys_addr);
163	}
```

[第 154 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:154:1)保证请求长度不小于对象的映射长度和对齐要求。两种存储分别按以下方式保证起点对齐：

- **Host 对象**交给 `gpgpu_dma_buf_alloc()`。它调用 `dma_alloc_coherent()`，检查返回的 DMA 地址整段落在 2 GiB 窗口内，再把 `0x8000_0000 + DMA 地址` 作为设备协议使用的 GPU 物理地址，最后清零。DMA API 保证一致性分配按“不小于请求长度的最小页阶”对齐，所以只要请求长度不小于对齐要求，返回的地址就已经满足对齐。
- **VRAM 对象**交给 `gpgpu_vram_alloc()`。请求长度原样交给 gpu_buddy，起点对齐单独取为不小于该长度的最小 2 的幂，并保留连续分配标志。由于请求长度已经不小于应用要求的对齐值，这种自然对齐也满足应用要求。gpu_buddy 在内部取整后裁掉多余尾部，驱动经 BAR2 只清零请求长度，每段最多 64 KiB，段间调用 `cond_resched()` 让出 CPU。清零期间仍持有 `dev->lock`，其他 ioctl 与 IH 处理仍需等待。gpu_buddy 只管理固件区之前的 60 MiB，首页预先占住，页表页也从同一个池中分配。

X 保留的 17 页可以由一个 16 页块和一个相邻的 1 页块记录，设备地址仍然连续；释放时归还这个块列表。起点按 32 页边界对齐，并不要求整个对象长期占有 32 页。若应用要求的对齐值大于 `mapped_size`，请求长度仍取该对齐值，保留原有对齐保证。

> **[SOURCE]** 本仓库：Host 分配与地址换算保持 `ec9a9c0` 的实现，见 [gpgpu_dev.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:41:1) [第 41～56 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:41:1)；VRAM 分配对应 Git blob `53284e4653806b4555c43b5c3ad7d0726d629f1c`，见 [gpgpu_vram.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vram.c:5:1) [第 5～25 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vram.c:5:1)、[第 37～73 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vram.c:37:1)，分别说明池范围、首页保留，以及独立的长度与对齐、分段清零和按块列表释放。
>
> **[SOURCE]** Linux `248951dd`：[Documentation/core-api/dma-api-howto.rst](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/Documentation/core-api/dma-api-howto.rst:451:1) [第 451～453 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/Documentation/core-api/dma-api-howto.rst:451:1)：“The CPU virtual address and the DMA address are both guaranteed to be aligned to the smallest PAGE_SIZE order which is greater than or equal to the requested size.” 即一致性分配返回的 CPU 地址和 DMA 地址，都按不小于请求长度的最小页阶对齐。

**AMDGPU。** 普通数据 BO 通过 GPU 页表呈现连续 GPUVA，后备存储通常不必整块连续：

- **GTT 对象（系统内存）**由 TTM 管理一组页，并取得各页的 DMA 地址。写 PTE 时填对应页的地址（见[地址翻译对照](<./第三阶段规格评审.md#review-address>)），GPUVA 的连续性由页表提供，分配接口不必把整个对象长度取成 2 的幂；按页对齐仍然需要。
- **VRAM 对象**由 `amdgpu_vram_mgr_new()` 使用 gpu_buddy 分配，通常允许多个不连续块。必须连续的对象带 `VRAM_CONTIGUOUS` 标志；连续分配路径先在内部取整到 2 的幂，分配后裁掉多余部分，不把它一直算在 BO 的占用中。管理器尝试连续分配失败时，只有未被要求必须连续的对象才能退回不连续分配。页表也是 BO，但这条路径为页表选择 VRAM 并要求连续，便于硬件按表基址读取。

> **[SOURCE]** Linux `248951dd`：
>
> - gpu_buddy 对连续分配先取整、再裁剪：[drivers/gpu/buddy.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/buddy.c:1353:1) [第 1353～1357 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/buddy.c:1353:1)、[第 1451～1471 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/buddy.c:1451:1)；
> - VRAM 管理器按标志选择连续或不连续，连续失败时回退：[amdgpu_vram_mgr.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c:499:1) [第 499～503 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c:499:1)、[第 547～553 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c:547:1)；
> - 页表 BO 放在 VRAM 并要求连续：[amdgpu_vm_pt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:441:1) [第 441～460 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:441:1)。
>
> 延伸阅读：父仓库笔记 [02_GPU 内存管理基础](<../../../1.笔记/02_GPU 内存管理基础.md>) §2.2。

**差异与代价。** 本设备要求设备地址连续，是因为 `gpuvm=off` 时用一个 GPU 物理起点加长度描述整个对象；Host 对象对应连续 IOVA。这也简化了 COPY 和管理域访问，但不要求启用 IOMMU 后的 Host 物理页也连续。两条路径都按对象和对齐要求确定请求长度，但底层保留存储的方式不同：

- VRAM：gpu_buddy 可在保持起点对齐和地址连续的同时裁掉取整尾部。对象只保留请求长度，仍要求一段满足原有自然对齐的连续地址，因此没有解除连续分配受碎片影响的限制。
- Host：驱动按请求长度申请、清零和释放，不承诺底层物理占用恰好等于该长度。分配器实际用多少物理页还取决于路径：`IOMMU=none` 时走直接映射，有 CMA 区域就从 CMA 按页数分配，否则用 `alloc_pages()` 按页阶整块分配；启用 SMMUv3 时走 IOMMU 路径，物理页可以离散，只要求 IOVA 连续。

> **[SOURCE]** Linux `248951dd`：直接映射路径先试 CMA、再按页阶分配，见 [kernel/dma/direct.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/kernel/dma/direct.c:119:1) [第 119～150 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/kernel/dma/direct.c:119:1)；IOMMU 路径用离散页拼出连续 IOVA，见 [drivers/iommu/dma-iommu.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/iommu/dma-iommu.c:1660:1) [第 1660～1671 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/iommu/dma-iommu.c:1660:1)。

DMA mask 还会限制 Host 分配可用的地址。**[INFERENCE]** 本例取 QEMU aarch64 的 `virt` 机型、1 GiB 客体内存和 `IOMMU=none`。RAM 从物理地址 `0x4000_0000` 开始，整个区间是 `[0x4000_0000, 0x8000_0000)`，恰好落在 31 位 DMA mask 内；从地址范围看，没有一段 RAM 因位宽而被排除，实际分配仍受内存占用和连续性限制。加大客体 RAM 后，物理地址达到或超过 `0x8000_0000` 的部分不能用于这条直接 DMA 分配路径。启用 SMMUv3 时，应检查 IOVA 是否在窗口内，不能再把低 IOVA 等同于低物理页。

> **[SOURCE]** 本仓库 `ec9a9c0`：`virt` 机型的内存基址见 [virt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/arm/virt.c:208:1) [第 208 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/arm/virt.c:208:1)；31 位 DMA mask 见 [gpgpu_main.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_main.c:151:1) [第 151～158 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_main.c:151:1)。


<a id="memory-cpu-access"></a>

## 4. CPU 映射与 COPY 搬运

**本设备。** Host BO 可由 `dma_mmap_coherent()` 映射给用户态，要求覆盖整个映射长度、使用共享映射且不可执行。应用直接在这段 CPU 地址中填写 kernarg：X、Y、Z 的 64 位地址，加上以 64 位值保存的元素个数 17,003，本次共 32 字节。

VRAM BO 也支持 mmap，但只接受 `PROT_NONE`：这段 VMA 占住与 GPUVA 相同的 CPU 地址，不映射可读写的数据页。驱动还清除 `VM_MAYREAD`、`VM_MAYWRITE`，防止之后用 `mprotect()` 改成可访问；库的公开 `cpu` 字段为 NULL。因此准备 X 的内容仍要搬运一次：

```text
CPU 写 Host 暂存 BO → COPY(暂存 BO, X)
  → 驱动给两端 users 加一，防止解绑和句柄关闭时放下列表引用
  → 释放 dev->lock，持 dma_lock，用两端 GPU 物理地址启动 DMA
  → 等待完成中断 → 回到 dev->lock 下处理计数
  → 确认成功后，X 的 VRAM 内容可供后续任务使用
```

COPY 不走用户 GPUVA，使用的是两个 BO 固定的 GPU 物理地址。等待最长 5 秒；只有确认成功后，调用者才能把数据当作已到位。超时只说明本次等待结束，不能证明 DMA 已停止，驱动会置全局 `dma_uncertain` 并继续保留本次两端的使用计数。

收尾依据本次返回值决定计数去留：只有 `ret == -ETIMEDOUT` 保留本次增加的 `users`；成功、已确认的 DMA 错误，以及启动前因已有 `dma_uncertain` 被拒绝，都减回本次计数。全局历史故障继续阻止后续操作，但不会让一个从未启动的 COPY 多留一次计数。

> **[SOURCE]** 本仓库 `ec9a9c0`：Host 与 VRAM mmap 的限制见 [gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:95:1) [第 95～124 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:95:1)；COPY 的参数检查、启动、等待和按本次超时保留计数见 [gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:352:1) [第 352～398 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:352:1)；等待上限见 [gpgpu_drv.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:52:1) [第 52～53 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drv.h:52:1)。

**AMDGPU。** 用户态 mmap 对象后，访问时触发缺页，`amdgpu_gem_fault()` 交给 TTM 按需把页映射进来。VRAM 只要落在 CPU 可见的 BAR 范围内，就能直接映射。在 GPU 搬运路径中，对象复制和填充作为 SDMA 任务提交，并返回 fence；CPU 能否直接访问 VRAM 与这些搬运任务的完成状态要分别判断。

> **[SOURCE]** Linux `248951dd`：[amdgpu_gem.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:119:1) [第 119～137 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:119:1)，缺页时调用 `ttm_bo_vm_fault_reserved()`。

> **[SOURCE]** Linux `248951dd`：复制任务提交后返回 fence，见 [amdgpu_ttm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c:2463:1) [第 2463～2507 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c:2463:1)；填充任务使用相同的完成方式，见 [amdgpu_ttm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c:2515:1) [第 2515～2550 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c:2515:1)。

**差异与代价。** 同步 COPY 把“提交搬运”和“等待成功”放在一次调用里，应用不需要再管理搬运 fence。代价是准备 VRAM 数据必须经过 Host 暂存对象，全设备的 COPY 又由一把 DMA 锁串行执行；一次超时留下的全局 `dma_uncertain` 还会阻止所有会话解绑和清理，边界见第 8 节。

<a id="memory-map"></a>

## 5. GPU 映射的建立

**本设备。** X 的 CPU 占位地址和实际 VRAM 存储都已取得，库接下来用同一个地址建立 GPU 绑定。可分配 GPUVA 区间为 `[0x1000, 0x1_0000_0000_0000)`，有效 48 位；库通过 `mmap()` 取得地址，驱动校验页对齐、范围和重叠，不替库另选地址，也不检查该数值是否等于 CPU 地址。同址约定由库保证。

X 的地址取 `0x0000_ffff_8a40_0000`，映射长度为 17 页，当前顺序如下：

```text
MAP(X, address = 0x0000_ffff_8a40_0000, perms = R | W)
  1. 检查 VM 未失败、对象未绑定、地址合法且不与已有绑定重叠
  2. vm_init()（仅首次）：分配根表，置 ready；不取 VMID
  3. vm_prepare_tables()：先为整个区间准备四级表路径，不写叶子 PTE
       L0[VA 47:39] → L1[38:30] → L2[29:21] → L3 叶表
       缺目录时分配已清零的表页，写入指向下一级的 PDE
  4. 记录 gpu_addr、permissions、bound，并取得绑定引用
  5. vm_set_ptes()：沿已准备的表路径，按 VA[20:12] 定位叶子项
       写 X 的 17 个 PTE；第 0 页为
       0x0020_0000 | VALID | R | W = 0x0020_0061
  6. vm_invalidate_range()
       vmid == 0：不发失效；下次建队列时配置根并整体失效
       vmid != 0：写 VPN 范围 → wmb() → 写失效请求 → 读 ACK 确认
```

第 6 步先填写 VPN 范围寄存器，`vm_invalidate_req()` 再执行 `wmb()` 并写 `VM_INV_REQ`，保证页表写入先于失效请求发布。

如果队列尚未创建，MAP 成功表示绑定和页表已经准备好；建队列时 `gpgpu_vm_attach()` 还要取得 VMID、写根、失效整个 VMID 并启用上下文。如果队列已经存活，本次 MAP 会完成相应 VPN 范围的失效，成功返回后，应用才可提交使用新映射的任务。队列存在本身不会使 MAP 返回 `-EBUSY`。

表页分配放在绑定登记之前，因此根表或中间表空间不足都属于确定失败：本次对象未绑定，也没有写入它的叶子 PTE，`vm->failed` 保持不变。首次根表分配失败时 `ready` 仍为 false，下次 MAP 可以重试；中间路径分配失败时，已建立的空表页留在 `vm->tables` 中复用，寿命见第 7 节。只有登记绑定、写入 PTE 之后无法确认失效结果，才保留绑定并把 VM 置为 failed。

> **[SOURCE]** 本仓库当前源码（协议版本 6）：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:273:1) [第 273～323 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:273:1)，完整的 MAP 入口，包含准备表路径、登记引用、写 PTE 和失效的先后顺序。

```c
273	int gpgpu_vm_map(struct gpgpu_fpriv *fpriv, struct gpgpu_vm_map *args)
274	{
275		struct gpgpu_device *dev = fpriv->dev;
276		struct gpgpu_vm *vm = &fpriv->vm;
277		struct gpgpu_bo *bo = gpgpu_bo_lookup(fpriv, args->handle);
278		u64 flags;
279		int ret;
280
281		/* 1. 先检查 VM 状态，再检查参数。 */
282		if (vm->failed)
283			return -EBUSY;
284		ret = map_args_valid(fpriv, bo, args);
285		if (ret)
286			return ret;
287
288		/* 2. 首次绑定时建立地址空间，gpuvm=on 时分配根表。 */
289		ret = vm_init(dev, vm);
290		if (ret)
291			return ret;
292
293		/* 3. 绑定前分配全部表页，失败时不留下这个对象的任何叶子项。 */
294		if (dev->gpuvm) {
295			ret = vm_prepare_tables(dev, vm, args->address, bo->mapped_size);
296			if (ret)
297				return ret;
298		}
299
300		/* 4. 绑定引用让对象在句柄关闭后继续存活。 */
301		bo->gpu_addr = dev->gpuvm ? args->address : bo->phys_addr;
302		bo->permissions = args->permissions;
303		bo->bound = true;
304		drm_gem_object_get(&bo->gem);
305		args->address = bo->gpu_addr;
306
307		if (!dev->gpuvm)
308			return 0;
309
310		/*
311		 * 5. 写叶子 PTE，再失效 TLB，设备才会看到新映射。Host 对象写
312		 *    IOVA 并置 SYSTEM，VRAM 对象写 VRAM 偏移；bo->phys_addr 仍是
313		 *    GPU 物理地址。不可缓存对象另置 UNCACHED。中间级已在第 3 步
314		 *    建好，vm_set_ptes() 不再分配表页，不会失败。
315		 */
316		flags = args->permissions | (bo->uncached ? GPGPU_VM_UNCACHED : 0);
317		if (bo->kind == GPGPU_MEMORY_HOST)
318			vm_set_ptes(dev, vm, bo->gpu_addr, bo->mapped_size, bo->host.dma,
319				    GPGPU_VM_SYSTEM | flags);
320		else
321			vm_set_ptes(dev, vm, bo->gpu_addr, bo->mapped_size, bo->phys_addr, flags);
322		return vm_invalidate_range(dev, vm, bo->gpu_addr, bo->mapped_size);
323	}
```

这段代码处在持有 `vm_serial` 与 `dev->lock` 的 ioctl 中，主要证明分配失败发生在绑定登记之前：

- [第 281～298 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:281:1)检查状态并准备根表和全部中间表；这里返回错误时尚未修改 X 的绑定状态。
- [第 300～308 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:300:1)登记地址、权限和绑定引用。`gpuvm=off` 在此返回固定物理地址，仍保留绑定引用。
- [第 310～322 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:310:1)沿已存在的表路径写叶子项，不再分配表页；Host 写 IOVA 并置 SYSTEM，VRAM 写显存偏移，不可缓存对象另置 UNCACHED。最后根据是否占有 VMID 完成必要的失效。

> **[SOURCE]** 本仓库 `ec9a9c0`：
>
> - 根表初始化与按 2 MiB 区段预分配路径：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:168:1) [第 168～213 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:168:1)；
> - 四级目录遍历、叶子写入、屏障与失效确认：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:70:1) [第 70～166 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:70:1)；GPUVA 布局与 PTE 权限位：[gpgpu_vm_abi.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_vm_abi.h:16:1) [第 16～69 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_vm_abi.h:16:1)；
> - 驱动校验地址和重叠：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:215:1) [第 215～253 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:215:1)；库的 CPU 选址与同址绑定：[memory.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:42:1) [第 42～70 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:42:1)、[第 127～164 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:127:1)；
> - 库将 `ENOMEM`、`ENOSPC` 作为确定失败撤销，将 `EIO`、`ETIMEDOUT` 作为结果未知保留地址：[memory.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:80:1) [第 80～88 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:80:1)、[第 155～188 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:155:1)。

**AMDGPU（KFD 路径）。** libhsakmt 先在用户态选好 GPUVA，调用 `AMDKFD_IOC_ALLOC_MEMORY_OF_GPU` 时把 VA 交给 KFD；之后用 `AMDKFD_IOC_MAP_MEMORY_TO_GPU` 建立设备映射。在 libhsakmt 的同址映射路径中，CPU 地址先由 `PROT_NONE` 预留，BO 再以 `MAP_FIXED` 映射到这里；没有 HostAccess 的 VRAM 对象也可以只占地址。下面区分驱动内部的映射登记、页表更新，以及 ioctl 返回前的等待与刷新：

```text
KFD 处理 MAP_MEMORY_TO_GPU
  → map_bo_to_gpuvm()
      1. amdgpu_vm_bo_map()：检查区间重叠，把 [start, last] 登记到 VM 的区间树
      2. update_gpuvm_pte() → amdgpu_vm_bo_update() → amdgpu_vm_update_range()
           页表游标沿树下行，缺失的页表 BO 按需分配，子表持有父表引用
           CPU 后端：等已有 fence → 写 PTE → mb() → 刷新 HDP
           SDMA 后端：把写表命令放进 IB 提交，返回更新 fence
         失败 → unmap_bo_from_gpuvm() 撤销该映射
  → KFD 的 ioctl 处理函数继续收尾：等待页表更新完成 → 按 PASID 刷新 TLB（heavyweight）
  → 返回用户态，调用者才可按新映射提交访问
```

区间树中的记录说明“软件希望建立什么映射”；PTE 才是硬件实际读取的翻译依据。SDMA 后端提交成功时，写表操作还可能在执行，所以后面的等待不能省。即使 PTE 已更新，TLB 中也可能留有旧翻译，还要完成相应刷新。

> **[SOURCE]** Linux `248951dd`：
>
> - 先登记、后写表、失败回滚：[amdgpu_amdkfd_gpuvm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1317:1) [第 1317～1349 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1317:1)；登记时检查重叠：[amdgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1843:1) [第 1843～1879 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1843:1)；
> - 按需分配页表 BO：[amdgpu_vm_pt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:496:1) [第 496～531 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:496:1)；
> - CPU 后端等 fence、写表、`mb()` 与 HDP 刷新：[amdgpu_vm_cpu.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c:73:1) [第 73～135 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c:73:1)；
> - SDMA 后端提交写表任务并返回更新 fence：[amdgpu_vm_sdma.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c:106:1) [第 106～145 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c:106:1)；
> - 映射后等更新完成再刷新 TLB：[kfd_chardev.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c:1362:1) [第 1362～1376 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c:1362:1)。
>
> 延伸阅读：父仓库笔记 [02_GPU 内存管理基础](<../../../1.笔记/02_GPU 内存管理基础.md>) §2.3。

> **[SOURCE]** ROCr `ba56a24c`：libhsakmt 预留并裁出对齐 CPU 地址，见 [fmm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:770:1) [第 770～815 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:770:1)；按 HostAccess 决定可读写或 `PROT_NONE`，再用 `MAP_FIXED` 映射 BO，见 [fmm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:1569:1) [第 1569～1584 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:1569:1)、[第 1799～1812 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/rocr-runtime/libhsakmt/src/fmm.c:1799:1)。

**差异与代价。**

1. **失败时怎样收尾。** AMD 写表失败时会调用 `unmap_bo_from_gpuvm()`，撤销登记并尝试清除已写表项；个别清理步骤的错误会被忽略，因此不能概括成任何故障下都完全回滚。本设备先完成可能失败的表页分配，再登记绑定和写叶子项；失效未确认时仍保留资源。
2. **同步与异步。** AMD 可以把写表交给 SDMA，由 fence 表示完成。本设备由 CPU 经 BAR2 同步写表，没有独立的异步写表任务；有驻留队列时在 MAP 内确认范围失效，没有驻留队列时到下次建队列再配置根并整体失效。
3. **写入可见性。** AMD 的 CPU 经 BAR 写显存会经过 HDP，所以这条 CPU 写表路径在 `mb()` 后还要刷新 HDP。本设备没有这层 HDP 缓存，驱动在发 REQ 前用 `wmb()` 保证写表先发布，设备在失效回调中同步清除 TLB 并写 ACK。这依赖本设备的执行模型，不能推广成真实 GPU 写页表只需屏障。
4. **写表效率。** 已有目录路径时，本设备每写一个叶子项仍要从根读三级目录，17 页的写表循环对应 51 次 BAR2 目录读和 17 次叶子写；预分配检查还会按每个 2 MiB 区段遍历一次目录，新建表页另有分配和写入。AMD 用页表游标继续处理相邻范围，减少重复访问上层目录。这项简化主要增加写表开销。
5. **GPUVA 由谁选。** 两侧都由用户态选址、内核校验。本设备借助进程的 `mmap()` 申请地址，内核用链表查重叠；libhsakmt 还有自己的 aperture 管理，AMDGPU 内核用区间树查重叠。本设备只采用“先申请 CPU 地址、再同址映射”的顺序，没有复制整套 aperture 管理。

> **[SOURCE]** 本仓库 `ec9a9c0`：设备允许所选上下文有激活队列，在同一次回调中清除 TLB 并发布 ACK，见 [gpuvm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:525:1) [第 525～554 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/hw/gpgpu/mem/gpuvm.c:525:1)；驱动的目录读取与预分配路径见本节前述 `vm_leaf()`、`vm_prepare_tables()` 证据。

<a id="memory-unmap"></a>

## 6. 解除映射与释放

**本设备。** 应用已经用完 X，但“撤销 GPUVA”和“归还 X 的存储”仍是两个动作。释放单个数据对象时，应用先确认使用 X 的任务已结束，也不会继续提交引用旧地址的包；无关任务和队列可以继续存在：

```text
应用确认 X 不再被任务使用
  → UNMAP(X.gpu_addr)：VM 健康、没有未知 DMA、X.users == 0
  → 清 X 的叶子 PTE
      VM 占有 VMID：写屏障 → 范围失效 → 读 ACK
      VM 未占 VMID：跳过失效，下次建队列时整体失效
  → 结束绑定并放下绑定引用
  → 若句柄已关闭，同时放下会话列表引用
  → 库 munmap，再关闭句柄；其余引用结束后由 bo_free() 归还存储

整会话关闭时另有前置顺序：
停止提交、排空并卸载队列 → 消费旧 IH → 停用上下文、整体失效、归还 VMID
  → 销毁事件、撤销通知页登记 → 清理剩余绑定、表页和对象
```

UNMAP 清表时不分配新表页，所以不会因表页空间不足而中途失败。有驻留队列时，范围失效只针对 X；ACK 未确认就不结束绑定，也不放下绑定引用。对象句柄此前已关闭时，驱动仍可按 GPU 地址找到这份绑定。Host 的可访问 mmap 和 VRAM 的 `PROT_NONE` mmap 若仍存在，都会继续保活 BO。

Ring 和进度对象要等所属队列确认卸载后才能解绑；通知页要等会话清理撤销登记。COPY 不依赖用户队列，另由 `users` 和 `dma_uncertain` 阻止过早解绑。对于普通数组和参数，`users == 0` 不能替代应用的任务完成判断。

> **[SOURCE]** 本仓库 `ec9a9c0`：UNMAP 的检查、清表、失效和引用释放见 [gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:334:1) [第 334～367 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:334:1)；句柄关闭与最后释放见 [gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:37:1) [第 37～82 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:37:1)；库在 UNMAP 成功后才 munmap、关闭句柄，见 [memory.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:247:1) [第 247～298 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/lib/kmt/memory.c:247:1)；队列卸载确认后消费旧 IH、放下使用计数并归还 VMID，见 [gpgpu_queue.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:447:1) [第 447～456 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:447:1)。

**AMDGPU。** KFD 解除映射时，`queue_refcount` 非零（队列自身正在使用的对象）就返回 `-EBUSY`；否则先把映射移到待清除列表，写入无效 PTE，等页表更新的 fence。MI300X 的 9.4.3 路径满足驱动“9.4.2 及以后”的条件：在页表更新完成后再按 PASID 做 heavyweight 刷新，最后才撤销 DMA 映射。BO 若仍有未完成的 fence，TTM 会延迟删除；删除工作先等 `dma_resv` 中相关 fence 都结束，再释放资源。

这里的 fence 要按提交路径区分。DRM 命令提交由内核跟踪任务，会把任务 fence 放进相关 BO 的 reservation，回收方可以等待这些已知访问结束。KFD 用户队列则由用户态直接提交 AQL，内核不会为每次派发自动生成一份“正在读 X”的 fence，也无法仅靠 BO 的 fence 判断任意 Kernel 是否还会使用 X。

KFD 为这类 BO 挂的是进程级 **eviction fence**：TTM 要搬移或驱逐对象时，通过这套机制要求 KFD 先驱逐该进程的队列，让它们暂时停止使用旧存储；它不是某次 Kernel 已正常执行完的标记。释放 BO 前，KFD 会移除这个 fence。应用仍须先保证没有任务继续使用要释放的内存；驱动负责把解除映射、等待写表、刷新 TLB、撤销 DMA 映射和归还存储按顺序完成。

> **[SOURCE]** Linux `248951dd`：
>
> - `queue_refcount` 非零拒绝解除映射，随后清除并同步：[amdgpu_amdkfd_gpuvm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1269:1) [第 1269～1292 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1269:1)；
> - 解除映射后等待、刷新 TLB、再撤销 DMA 映射：[kfd_chardev.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c:1446:1) [第 1446～1467 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c:1446:1)；刷新方式与适用版本：[kfd_priv.h](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_priv.h:1560:1) [第 1560～1574 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdkfd/kfd_priv.h:1560:1)；
> - 分配时挂上进程的 eviction fence、释放前移除：[amdgpu_amdkfd_gpuvm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1858:1) [第 1858～1866 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1858:1)、[第 1956～1957 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c:1956:1)；
> - 延迟删除先等 fence：[ttm_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/ttm/ttm_bo.c:235:1) [第 235～247 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/ttm/ttm_bo.c:235:1)。

> **[SOURCE]** Linux `248951dd`：DRM 提交把任务的完成 fence 放入相关 BO 的 reservation，见 [amdgpu_cs.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c:1331:1) [第 1331～1348 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c:1331:1)；KFD 的 eviction fence 以进程队列停止为搬移前提，并安排驱逐与恢复工作，见 [amdgpu_amdkfd_fence.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_fence.c:36:1) [第 36～56 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_fence.c:36:1)、[第 113～143 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_fence.c:113:1)。

**差异与代价。** 两侧都要确认旧访问结束后才能复用存储。AMDGPU 对 DRM 任务用 fence 追踪内核已知的对象依赖；对 KFD 用户队列，仍由应用保证普通数据对象不再被任务使用。本设备也允许队列存活期间释放无关对象，但只实现固定存储、同步失效和少量基础对象的使用计数，没有 TTM 搬移、reservation 依赖或 eviction fence。

<a id="memory-page-tables"></a>

## 7. 页表与 VMID 的寿命

**本设备。** 表页属于软件 VM，从首次建立后一直保留到会话清理；UNMAP 只清叶子项，不拆空目录、不归还空表页。以 X 所在的 2 MiB 区段为例：

```text
X 的翻译路径
  L0 根页 → L1 目录页 → L2 目录页 → L3 叶表页 → X 的 17 个数据页
  （路径由 4 张表页组成；已有部分复用，只分配缺少的表页；数据页由 BO 持有）

X 及同区段其他对象全部 UNMAP
  → 叶子 PTE 清零；有 VMID 时完成范围失效
  → L3 叶表已空，但仍在 vm->tables，L0/L1/L2 也继续保留
  → 同一 2 MiB 区段再次 MAP 时复用已有路径

队列卸载 → 归还 VMID，页表和通知页仍由会话持有
会话安全清理 → 销毁事件、撤销通知页登记 → 放下剩余绑定 → 归还全部表页
```

换到另一个 2 MiB 区段时，可能再分配一张 L3 叶表；跨越更大的目录范围还会增加上级表页。因此表页占用取决于 VM 曾经触及过哪些区段，而不只取决于当前还映射了多少数据。绑定前预分配中途失败时，已建立的空路径也按这条寿命保留，但不会留下该 BO 的叶子映射。

当前是 48 位 GPUVA、四级页表，每级有 512 项。完整覆盖 256 TiB 地址空间的理论表页数为 `1 + 512 + 512² + 512³`，远超本设备的 VRAM 池；实际分配会先受到表页和 BO 共用的 60 MiB 池限制。VM 总数不受两个 VMID 槽位限制，多个长期存在的 VM 会共同占用这个池。

`vm->ready` 只表示地址空间已准备，开启 GPUVM 时根表已经存在；`vm->vmid` 表示当前借用的硬件槽位。`ready = true`、`vmid = 0` 是正常状态：页表准备好，但尚未建队列，或队列已经卸载。通知页由用户库在打开连接时创建，驱动通过 `vm->notify` 单独记录其登记状态。

空表回收的范围与重新评估条件见[大纲的空表回收边界](<./QEMU GPGPU 实验大纲.md#page-table-reclaim-extension>)。

> **[SOURCE]** 本仓库 `ec9a9c0`：表页登记及保留方式见 [gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:33:1) [第 33～67 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:33:1)；`ready` 的建立与预分配失败行为见 [gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:168:1) [第 168～213 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:168:1)；VMID 的取得和归还见 [gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:402:1) [第 402～456 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:402:1)；会话清理时撤销通知页登记、绑定及全部表页见 [gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:477:1) [第 477～522 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:477:1)。

**AMDGPU。** 页表 BO 按需分配。一次更新如果完整覆盖了某棵子树，例如换成大页或撤销了整段区间，这棵子树就被移入待释放列表；页表 BO 的 reservation 上挂有 TLB fence，保证存储不会在刷新完成前被释放。没有被完整覆盖的页表可能仍在使用，继续保留；整个 VM 销毁时，再释放根目录及其下的全部页表。页表 BO 本身也可以被驱逐，之后再验证并恢复。VMID 的动态绑定见[地址空间身份与队列绑定](<./第三阶段规格评审.md#review-identity>)。

> **[SOURCE]** Linux `248951dd`：完整覆盖的子树进入待释放列表：[amdgpu_vm_pt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:916:1) [第 916～930 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:916:1)；刷新完成前不释放页表，由更新时挂到根目录 reservation 上的 TLB fence 保证，见 [amdgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1074:1) [第 1074～1079 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1074:1)；待释放列表的处理：[第 559～575 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:559:1)、[amdgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1232:1) [第 1232 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:1232:1)。

> **[SOURCE]** Linux `248951dd`：VM 销毁时调用页表树释放入口，见 [amdgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:2787:1) [第 2787～2789 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c:2787:1)；释放根目录及其下全部页表，见 [amdgpu_vm_pt.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:601:1) [第 601～615 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c:601:1)。

<a id="memory-uncertain"></a>

## 8. 结果未知时的资源保留

释放资源前，驱动要确认相关访问已结束。失败时先判断请求执行到了哪一步：尚未写入叶子项的分配失败可以撤销；已知的卸载拒绝允许按状态重试；设备访问结果未知时则保留资源：

```text
表页分配失败 → 本次 BO 未绑定、叶子 PTE 未写 → 撤销本次对象申请，可重试
卸载命令未发布或已收到合法拒绝 → 已知处理结果 → 保留仍在使用的资源，可再次卸载
命令已发布但结果未知 / 失效 ACK 未确认 / COPY 超时
  → 继续保留设备可能访问的对象、表页和编号
  → 文件关闭且仍无法安全清理：会话转为 orphan
  → 设备确认停止旧访问后，才统一回收
```

等待超时只结束软件等待，不能证明设备操作已经完成。当前驱动用几个不同状态阻止继续使用或过早释放：

- `vm->failed` 表示地址空间已不能安全继续使用，禁止修改和建队列。失效 ACK 不符、上下文启用或停用未确认会设置它；根表或中间表的确定分配失败直接返回错误，不设置该标志。
- 队列的 `uncertain` 保护 CP 可能仍持有的资源。卸载命令已经发布，但完成结果未确认时继续保留；命令未发布，或取得了合法结果码时清除。合法拒绝后仍可能需要保留队列，只是可以依据已知状态再次发起卸载。
- 设备的 `dma_uncertain` 表示不能确认 DMA 已结束。COPY 超时会设置它，设备停止未确认时也会设置它。该标志是全局的，后续 COPY 返回 `-EIO`，所有会话的 UNMAP 和运行中的会话清理都被阻止；本次 COPY 的计数去留仍按第 4 节的返回值规则处理。
- 会话的 `orphan` 是清理归属状态：文件已关闭，但驱动还不能安全回收，便把会话留在设备中管理。它不是又一种硬件故障，也不会自动重试失败任务。

合法的卸载结果还要区分 CP 是否仍持有队列。`RECOVERY_REQUIRED` 表示未排空而拒绝停用，驱动恢复原状态，应用停止提交、排空后可重试；`MQD_FAULT` 保持队列故障和资源占用，但允许再次卸载。`OK` 或 `NOT_MAPPED` 表示 CP 当前不持有该队列，驱动消费旧 IH 后放下 Ring、进度的使用计数，再尝试停用上下文并归还 VMID；停用或失效未确认时，VMID 仍保留。等待期间 IH 已报告的故障不会被恢复原状态覆盖。

> **[SOURCE]** 本仓库 `ec9a9c0`：
>
> - VM 失效或上下文控制未确认时保留：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:112:1) [第 112～120 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:112:1)、[第 402～456 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:402:1)；
> - 卸载命令的未知结果、合法拒绝和已卸载结果分别处理：[gpgpu_queue.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:346:1) [第 346～460 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_queue.c:346:1)；
> - COPY 超时及后续拒绝：[gpgpu_bo.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:371:1) [第 371～397 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_bo.c:371:1)；全局未知 DMA 阻止 UNMAP 和会话清理：[gpgpu_vm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:334:1) [第 334～341 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:334:1)、[第 477～496 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_vm.c:477:1)；
> - 文件关闭时转为 orphan：[gpgpu_drm.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drm.c:91:1) [第 91～112 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_drm.c:91:1)。

当前 orphan 会话要等设备确认停止，才能统一回收。驱动移除路径先保持 CP 复位、等待 Outbound 无在途访问，再尝试整体复位并核对状态；如果仍不能确认停止，就继续保留设备引用和相关存储。仅仅发出复位命令或进入驱动移除路径，还不足以释放这些资源。

AMDGPU 另有运行期的超时与复位恢复流程：停止设备活动，按故障情况复位并重新初始化，再恢复调度。资源能否回收仍取决于旧访问是否已经结束，以及相关 fence 和引用是否解除；单独把 fence 设为错误不能替代设备停止。

> **[SOURCE]** 本仓库 `ec9a9c0`：停机确认见 [gpgpu_dev.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:306:1) [第 306～332 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:306:1)；仅在 `stopped` 为真时回收 orphan，否则继续保留，见 [gpgpu_dev.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:350:1) [第 350～390 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/linux/gpgpu_dev.c:350:1)。
>
> **[SOURCE]** Linux `248951dd`：GPU 恢复入口中的停止活动、复位与恢复调度顺序，见 [amdgpu_device.c](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:5874:1) [第 5874～5904 行](vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/2.%E6%BA%90%E7%A0%81/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:5874:1)。

**取舍。** 当前实现宁可继续占用资源，也不释放设备可能仍在访问的内存。尤其是全局 `dma_uncertain`，会把一次 COPY 超时的影响扩展到所有会话；这条保守边界仍在。管理员可以请求[整设备中止与复位恢复](<./CP_MEC 第一版规格.md#spec-device-recovery>)，确认旧访问停止后再解除资源保留；不能只清除软件标志。旧任务终止，新连接重新计算，不恢复旧任务现场。
