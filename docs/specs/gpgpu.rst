
GPGPU device
============

..
   Copyright (c) 2024-2025
   This document is licensed under the GPLv2 (or later).

This is an educational GPGPU (General-Purpose Graphics Processing Unit) device
for learning GPU architecture, driver development, and parallel programming.
The device implements a simplified SIMT (Single Instruction, Multiple Threads)
execution model inspired by Vortex GPGPU, using standard RISC-V instructions.

Overview
--------

The GPGPU device is a PCIe device that provides:

- SIMT execution model with configurable warps and threads
- Dedicated video memory (VRAM) for GPU computations
- DMA engine for host-device data transfer
- Synchronous kernel dispatch registers
- MSI/MSI-X interrupt support

The device reuses the RISC-V RV32IMF instruction set, avoiding custom GPU
instructions. Thread divergence and synchronization are handled through
a dedicated CTRL (Control) unit accessible via memory-mapped I/O.

Architecture
------------

::

    +------------------------------------------------------------------+
    |                        GPGPU Device                              |
    |  +------------------------------------------------------------+  |
    |  |                    PCIe Interface                          |  |
    |  +------------------------------------------------------------+  |
    |         |              |              |              |           |
    |         v              v              v              v           |
    |  +-----------+  +-----------+  +-----------+  +-----------+     |
    |  |  BAR 0    |  |  BAR 2    |  |  BAR 4    |  |    DMA    |     |
    |  |  Control  |  |   VRAM    |  | Doorbell  |  |  Engine   |     |
    |  |   Regs    |  |           |  |           |  |           |     |
    |  +-----------+  +-----------+  +-----------+  +-----------+     |
    |         |              |              |              |           |
    |         v              v              v              v           |
    |  +------------------------------------------------------------+  |
    |  |                  Compute Unit Array                        |  |
    |  |  +----------+  +----------+  +----------+  +----------+    |  |
    |  |  |   CU 0   |  |   CU 1   |  |   CU 2   |  |   CU N   |    |  |
    |  |  | +------+ |  | +------+ |  | +------+ |  | +------+ |    |  |
    |  |  | |Warp 0| |  | |Warp 0| |  | |Warp 0| |  | |Warp 0| |    |  |
    |  |  | +------+ |  | +------+ |  | +------+ |  | +------+ |    |  |
    |  |  | |Warp 1| |  | |Warp 1| |  | |Warp 1| |  | |Warp 1| |    |  |
    |  |  | +------+ |  | +------+ |  | +------+ |  | +------+ |    |  |
    |  |  +----------+  +----------+  +----------+  +----------+    |  |
    |  +------------------------------------------------------------+  |
    |                             |                                    |
    |                             v                                    |
    |  +------------------------------------------------------------+  |
    |  |                    Shared Memory                           |  |
    |  +------------------------------------------------------------+  |
    +------------------------------------------------------------------+

SIMT Execution Model
--------------------

Thread Hierarchy
~~~~~~~~~~~~~~~~

The GPGPU uses a hierarchical thread organization:

Grid
    A grid represents the entire computation, consisting of multiple thread
    blocks. Grid dimensions are specified by ``GRID_DIM_X``, ``GRID_DIM_Y``,
    and ``GRID_DIM_Z`` registers.

Thread Block (Workgroup)
    A thread block is a group of threads that can cooperate via shared memory
    and synchronization barriers. Block dimensions are specified by
    ``BLOCK_DIM_X``, ``BLOCK_DIM_Y``, and ``BLOCK_DIM_Z`` registers.

Warp
    A warp is the basic scheduling unit. All threads in a warp execute the
    same instruction in lockstep (SIMT). The warp size is fixed at 32 threads
    (configurable via device property).

Thread
    The smallest execution unit. Each thread has its own register file and
    program counter (for divergence handling).

Thread Divergence
~~~~~~~~~~~~~~~~~

When threads in a warp take different branch paths, the hardware handles
divergence by:

1. Executing each path sequentially with appropriate thread masks
2. Reconverging threads at the immediate post-dominator
3. Using a divergence stack to track active thread masks

Special Registers (via CTRL unit)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Threads can read their identity through CTRL MMIO reads:

- ``THREAD_ID_X/Y/Z``: Thread index within block
- ``BLOCK_ID_X/Y/Z``: Block index within grid
- ``BLOCK_DIM_X/Y/Z``: Block dimensions
- ``GRID_DIM_X/Y/Z``: Grid dimensions
- ``WARP_ID``: Warp index within block
- ``LANE_ID``: Thread index within warp (0-31)

Command Line Options
--------------------

``-device gpgpu[,vram_size=SIZE][,num_cus=N][,warps_per_cu=M][,warp_size=W]``

``vram_size``
    Video memory size in bytes. Default: 64 MiB. Must be power of 2.

``num_cus``
    Number of compute units. Default: 4.

``warps_per_cu``
    Number of warps per compute unit. Default: 4.

``warp_size``
    Number of threads per warp. Default: 32. Must be power of 2.

双进程与原生 CP
----------------

**[DESIGN]** 原单进程入口继续提供 A57 与 GPGPU 设备。本轮增加
A57 Host 进程和 RISC-V 设备进程：Host 使用 ``vfio-user-pci``，设备进程使用
``gpgpu-cp`` machine，自动创建 ``gpu0`` 和默认保持复位的原生 RV64 CP。
CP 的指令由 TCG（Tiny Code Generator，QEMU 动态代码生成基础设施）执行；
CP 在 M-mode 执行裸机固件，不启用页表；原 RV32 计算解释器仍运行应用 kernel。

::

    进程 A：A57 Linux
      └─ PCIe 代理 ── 配置空间、BAR 请求 ──→ 进程 B：gpu0 + RV64 CP
           ├─ PCI DMA 接口 ←─ B 的 DMA 请求       └─ 同一份 VRAM
           │    └─ SMMUv3（启用时）→ Host RAM
           └─ Linux 驱动 ←─ B 的中断通知

``none`` 与 ``smmuv3`` 均使用 ``x-dma-via-socket=on``。B 发送完整设备
DMA 地址、方向和数据请求，A 调用代理设备的 PCI DMA 接口，并返回结果或错误。
SMMUv3 翻译、权限检查和故障记录继续发生在 A。此选项不增加硬件寄存器。
该模式关闭 posted writes，串行发送 Host 请求，同步等待中允许处理反向 DMA。
MSI-X 表和屏蔽状态在两端同步，计算、DMA 完成和错误通知保留原有向量。

服务端使用 ``subprojects/libvfio-user.wrap`` 固定的
``0b28d205572c80b568a1003db2c8f37ca333e4d7`` 版本。wrap 登记的最小补丁
``subprojects/packagefiles/libvfio-user/0001-message-dma-window.diff``
允许无共享 fd 的消息 DMA 区域登记大地址窗口；共享内存区域仍保留原限制，
区域容量仍为 16。补丁还修正消息地址遗漏区域内偏移 ``sg->offset`` 的问题。
消息 DMA 窗口不绕过 A 的 IOMMU，也不表示所有地址已映射。

构建需要 QEMU 原有依赖，以及 ``libjson-c-dev``、``libcmocka-dev``。
从仓库根目录配置 AArch64 和 RV64 两个目标，构建产物仍放在 ``build/``：

.. code-block:: sh

    mkdir -p build/arm64-qemu
    cd build/arm64-qemu
    ../../configure --target-list=aarch64-softmmu,riscv64-softmmu \
        --enable-vfio-user-server --disable-rust --disable-docs \
        --disable-tools --disable-guest-agent
    ninja -j4 qemu-system-aarch64 qemu-system-riscv64
    cd ../..
    make -C software/gpgpu/linux prepare
    make -C software/gpgpu/linux run REMOTE=1 IOMMU=smmuv3

交互入口由 ``scripts/gpgpu_remote.py`` 启动 B，再连接 A，并保存两端命令和
设备日志。Host 退出、启动超时和异常会清理两个进程。本版不支持断线重连和迁移，
也不扩展 CP 发起异步 Host DMA 的功能。

PCI Configuration
-----------------

PCI IDs
~~~~~~~

=============== ================
Field           Value
=============== ================
Vendor ID       0x1234 (QEMU)
Device ID       0x1337
Revision        0x01
Class Code      0x030200 (3D Controller)
Subsystem VID   0x1234
Subsystem ID    0x1100
=============== ================

PCI Capabilities
~~~~~~~~~~~~~~~~

- MSI-X capability (4 vectors)
- MSI capability
- Standard PCIe Endpoint capability

BAR Layout
~~~~~~~~~~

======= ============= ============= =================================
BAR     Type          Size          Description
======= ============= ============= =================================
BAR 0   Memory, 64b   1 MiB         Control registers (MMIO)
BAR 2   Memory, 64b   64-256 MiB    Video RAM (prefetchable)
BAR 4   Memory, 32b   64 KiB        Doorbell registers
======= ============= ============= =================================

Control Registers (BAR 0)
-------------------------

BAR 0 contains the main control and status registers. All registers are
little-endian. Access size requirements are specified for each register.

Device Information (0x0000 - 0x00FF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x0000
     - 4
     - RO
     - **DEV_ID**: Device identification.
       Value: ``0x47505055`` ("GPPU" in ASCII)
   * - 0x0004
     - 4
     - RO
     - **DEV_VERSION**: Device version.
       Format: ``[31:16]=major, [15:8]=minor, [7:0]=patch``
   * - 0x0008
     - 4
     - RO
     - **DEV_CAPS**: Device capabilities.

       - Bits [7:0]: Number of compute units
       - Bits [15:8]: Warps per compute unit
       - Bits [23:16]: Threads per warp (warp size)
       - Bits [31:24]: Reserved
   * - 0x000C
     - 4
     - RO
     - **VRAM_SIZE_LO**: VRAM size low 32 bits (in bytes)
   * - 0x0010
     - 4
     - RO
     - **VRAM_SIZE_HI**: VRAM size high 32 bits

Global Control (0x0100 - 0x01FF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x0100
     - 4
     - RW
     - **GLOBAL_CTRL**: Global control register.

       - Bit 0: Device enable (1=enabled, 0=disabled)
       - Bit 1: Soft reset (write 1 to reset, auto-clears)
       - Bits [31:2]: Reserved
   * - 0x0104
     - 4
     - RO
     - **GLOBAL_STATUS**: Global status register.

       - Bit 0: Device ready
       - Bit 1: Device busy (kernel executing)
       - Bit 2: Error occurred
       - Bits [31:3]: Reserved
   * - 0x0108
     - 4
     - RW1C
     - **ERROR_STATUS**: Error status (write 1 to clear).

       - Bit 0: Invalid command
       - Bit 1: VRAM access fault
       - Bit 2: Kernel execution error
       - Bit 3: DMA error
       - Bits [31:4]: Reserved

Interrupt Control (0x0200 - 0x02FF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x0200
     - 4
     - RW
     - **IRQ_ENABLE**: Interrupt enable mask.

       - Bit 0: Kernel completion interrupt
       - Bit 1: DMA completion interrupt
       - Bit 2: Error interrupt
       - Bits [31:3]: Reserved
   * - 0x0204
     - 4
     - RO
     - **IRQ_STATUS**: Interrupt status (pending interrupts).
   * - 0x0208
     - 4
     - WO
     - **IRQ_ACK**: Interrupt acknowledge (write 1 to clear).

Kernel Dispatch (0x0300 - 0x03FF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x0300
     - 4
     - RW
     - **KERNEL_ADDR_LO**: Kernel code address in VRAM (low 32 bits)
   * - 0x0304
     - 4
     - RW
     - **KERNEL_ADDR_HI**: Kernel code address (high 32 bits)
   * - 0x0308
     - 4
     - RW
     - **KERNEL_ARGS_LO**: Kernel arguments address in VRAM (low)
   * - 0x030C
     - 4
     - RW
     - **KERNEL_ARGS_HI**: Kernel arguments address (high)
   * - 0x0310
     - 4
     - RW
     - **GRID_DIM_X**: Grid dimension X (number of blocks in X)
   * - 0x0314
     - 4
     - RW
     - **GRID_DIM_Y**: Grid dimension Y
   * - 0x0318
     - 4
     - RW
     - **GRID_DIM_Z**: Grid dimension Z
   * - 0x031C
     - 4
     - RW
     - **BLOCK_DIM_X**: Block dimension X (threads per block in X)
   * - 0x0320
     - 4
     - RW
     - **BLOCK_DIM_Y**: Block dimension Y
   * - 0x0324
     - 4
     - RW
     - **BLOCK_DIM_Z**: Block dimension Z
   * - 0x0328
     - 4
     - RW
     - **SHARED_MEM_SIZE**: Shared memory size per block (bytes)
   * - 0x0330
     - 4
     - WO
     - **DISPATCH**: Write any value to start kernel execution.
       Kernel parameters must be configured before writing.

DMA Engine (0x0400 - 0x04FF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x0400
     - 8
     - RW
     - **DMA_SRC_ADDR**: 系统 DMA 地址或 VRAM 字节偏移，由方向选择
   * - 0x0408
     - 8
     - RW
     - **DMA_DST_ADDR**: VRAM 字节偏移或系统 DMA 地址，由方向选择
   * - 0x0410
     - 4
     - RW
     - **DMA_SIZE**: Transfer size in bytes
   * - 0x0414
     - 4
     - RW
     - **DMA_CTRL**: DMA control register.

       - Bit 0: Start transfer (write 1 to start)
       - Bit 1: Direction (0=host to VRAM, 1=VRAM to host)
       - Bit 2: Interrupt on completion
       - Bits [31:3]: Reserved
   * - 0x0418
     - 4
     - RO
     - **DMA_STATUS**: DMA status.

       - Bit 0: DMA busy
       - Bit 1: DMA complete
       - Bit 2: DMA error

系统 IOMMU 与 DMA 地址
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Linux 驱动将 ``dma_alloc_coherent()`` 返回的 DMA 地址写入系统内存端，
显存端填写 VRAM 字节偏移。启用系统 IOMMU（输入输出内存管理单元）时，
系统内存端是 IOVA（设备 DMA 虚拟地址），由 ARM64 virt 的 SMMUv3
（Arm 系统内存管理单元第三版）翻译为客体物理地址。数据路径为：

::

    BAR0 系统 DMA 地址 → PCI 地址空间 → SMMUv3 → 客体 RAM 暂存区
    BAR0 显存偏移      → GPGPU VRAM

``hw/gpgpu/gpgpu_dma.c`` 使用 ``pci_dma_read/write()`` 并检查
``MemTxResult``。未映射访问使 ``DMA_STATUS`` 为 ERROR，置位
``ERROR_STATUS.DMA_FAULT`` 并报告错误中断，不发送 DMA 完成通知。
向 ``ERROR_STATUS`` 和 ``IRQ_ACK`` 写 1 清除锁存状态后，可用合法映射
重新提交描述符；不需要重置设备或重建页表。

从仓库根目录运行：

.. code-block:: sh

    make -C software/gpgpu/linux prepare
    make -C software/gpgpu/linux test IOMMU=smmuv3
    make -f Makefile.camp test-gpgpu-stack IOMMU=smmuv3
    make -C software/gpgpu/linux run IOMMU=smmuv3
    make -f Makefile.camp test-gpgpu-iommu JOBS=4

默认 ``IOMMU=none``，脚本参数为 ``--iommu none|smmuv3``。启用时使用
``-machine virt,gic-version=3,iommu=smmuv3,default-bus-bypass-iommu=off``，
并追加内核参数 ``iommu.passthrough=0 iommu.strict=1``。自举和编译保持
原配置，两个模式共用内核、驱动与编译器磁盘。

客体必须具备内建 SMMUv3 和 IOMMU DMA 支持。测试按 PCI ID ``1234:1337``
查找设备，读取其 ``iommu_group/type``，要求为 ``DMA``，并检查非预期
SMMU fault。结果和日志分别保存在 ``build/gpgpu-linux-module/none/`` 与
``build/gpgpu-linux-module/smmuv3/``。系统 IOMMU 不改变应用设备指针和
kernel 参数的 VRAM 偏移语义，不提供 GPU 内部地址隔离。

2026-10-08 在 ``main`` 分支以 ARM64 Linux ``6.18.55-0-virt`` 实测：
两种模式各通过 17,953 个示例输出、68/68 运行时检查、5/5 装卸载和
2/2 卸载重载；SMMUv3 下设备 domain 类型为 ``DMA``，无非预期 SMMU
fault。ARM64、RISC-V 原有 GPGPU QTest 各 21/21，新专项 2/2，均无
跳过，命令退出码均为 0。专项覆盖非恒等映射的 DMA 往返、未映射写入
的错误通知与无完成通知，以及清错后的原映射恢复。没有增加跨页、
只读权限或撤销／重映射用例；大块搬运沿用软件栈测试。

CP 控制寄存器（0x0500）
~~~~~~~~~~~~~~~~~~~~~~~~

**[DESIGN]** Host 已映射 BAR0、BAR2，接下来经 BAR2 装载固件，并解除
CP 复位。CP 独立地址空间中的 ``0～vram_size-1`` 映射现有 VRAM 的同偏移。
例如固件位于 ``0x4000``，Host 写 ``BAR2 基址 + 0x4000``，CP 从地址
``0x4000`` 取指，两者访问同一份存储。Host BAR 地址不能用作 CP 入口。

::

    写 CP_CTRL.RESET_HOLD = 1，读回确认
      → 经 BAR2 装载固件与初始数据
      → 写 CP_RESET_VECTOR
      → 执行访问屏障和读回，确认写入完成
      → 清零 RESET_HOLD
      → CP 执行，并把结果写入 VRAM
      → Host 经 BAR2 读取结果

``0x0500 CP_CTRL`` 是对齐、小端的 32 位寄存器。bit 0 ``RESET_HOLD``
可读写、默认 1；bit 1 ``PRESENT`` 只读。连接 CP 时默认读回 3，解除复位后
读回 2。保留位读零、写入忽略，写只读位也忽略。置位恢复原生 CPU 复位状态并
保持不取指；清零后从入口执行。重复写入相同复位状态不会重新启动 CPU。

``0x0504 CP_RESET_VECTOR`` 是对齐、小端的 32 位启动地址寄存器，默认 0。
只有保持复位时可修改；地址须位于 VRAM 内，按 2 字节对齐。有效修改同步更新
原生 CPU 复位向量和复位态 PC。非法入口或运行期间修改入口保留原值，并置位
已有 ``ERROR_STATUS.INVALID_CMD``。没有连接 CP 的单进程设备对这两个
寄存器读零、写入忽略。

复位与启动在目标 CPU 线程中同步完成。CP 单独复位清零 Host 窗口基址，保留
VRAM、入口及原 DMA、派发、中断状态。``GLOBAL_CTRL.RESET`` 复位设备状态，并使 CP 保持
复位、入口恢复 0，VRAM 保留。设备冷复位先停止 CP，再清零 VRAM 并恢复
全部默认状态。``GLOBAL_CTRL.ENABLE`` 不自动启动 CP。

BAR、DMA、计算解释器和冷复位的 VRAM 写入使对应 TCG 翻译失效，支持复位后
在相同地址替换固件。本轮不永久预留固件分区、不缩小可分配显存；CP 固件与
原软件栈测试分别运行，明确各自代码和结果地址。

**[BOUNDARY]** CP 已能执行基本固件，访问设备本地 VRAM 和下述 Host 窗口。
队列、Doorbell 事件和计算派发尚未接入。这是 QEMU 教学接口，不表示
MI300X 的控制核指令集、寄存器或内部结构。

CP 访问 Host RAM 的窗口
~~~~~~~~~~~~~~~~~~~~~~~

**[DESIGN]** Host 已为内存建立有效的设备 IOVA 映射后，CP 配置窗口基址，
用普通 ``load/store`` 访问 Host RAM。IOVA 是设备访存使用的虚拟地址；启用
SMMUv3 时，由 Host 侧的系统内存管理单元翻译并检查权限。

::

    CP 窗口地址 → IOVA 基址 + 窗口内偏移
      → vfio-user 消息访存 → Host 的 PCI DMA 接口
      → SMMUv3（启用时）→ Host RAM

CP 本地 ``0x8000_0000～0x8000_ffff`` 为 64 KiB 窗口，支持自然对齐的
1、2、4、8 字节小端读写。``0xfffe_0000`` 与 ``0xfffe_0004`` 分别保存
64 位 IOVA 基址的低、高 32 位，采用自然对齐的 32 位访问。基址按 64 KiB
对齐，低 16 位读零、写入忽略。固件应写完两个寄存器后再使用窗口，更新
期间中断处理程序也不能访问窗口。地址 ``0xffff_0000`` 留给后续 HQD。

RV64 的通用寄存器为 64 位，本地地址仍按原有 4 GiB 布局解释。
例如参数区保存 32 位本地地址 ``0x8000_1234``，固件用 ``LWU`` 读取，
得到 ``0x0000_0000_8000_1234``，再访问 Host 窗口。使用 ``LUI`` 构造
地址后也须清除高 32 位的符号扩展。Host 地址仍完整保存在基址高低寄存器中。

只有实际访问的范围需要有效映射。访问失败返回原生 load/store access fault，
成功返回读值或完成写请求；失败不保证撤销已经发生的写入。CP 复位将基址
清零，零基址表示访问 IOVA 0 附近，不表示关闭窗口。

窗口直接调用 ``vfu_object_dma_rw()``，不使用原批量 DMA 描述符、状态和完成
中断，不依赖 ``GLOBAL_CTRL.ENABLE``。Host 仍可同时使用 BAR2 与原 DMA
访问 VRAM，不需要移交 DMA 使用权；软件负责共享数据的访问顺序和寿命。
CP 的 VRAM 区域是原存储的别名，窗口和地址空间容器均不分配新的 RAM。

当前沿用 PCIe 设备的 IOMMU 上下文；按队列 VMID 选页表、跨处理器原子操作、
动态 64 位进度的发布与并发访问协议仍未实现。普通 ``LD/SD`` 不能提供
Signal 的原子读改写。固件代码与栈继续放在 VRAM。

Thread Context Registers (0x1000 - 0x1FFF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These registers provide thread identity information when read by GPU threads.
The values returned depend on the executing thread context.

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x1000
     - 4
     - RO
     - **THREAD_ID_X**: Thread ID within block (X dimension)
   * - 0x1004
     - 4
     - RO
     - **THREAD_ID_Y**: Thread ID within block (Y dimension)
   * - 0x1008
     - 4
     - RO
     - **THREAD_ID_Z**: Thread ID within block (Z dimension)
   * - 0x1010
     - 4
     - RO
     - **BLOCK_ID_X**: Block ID within grid (X dimension)
   * - 0x1014
     - 4
     - RO
     - **BLOCK_ID_Y**: Block ID within grid (Y dimension)
   * - 0x1018
     - 4
     - RO
     - **BLOCK_ID_Z**: Block ID within grid (Z dimension)
   * - 0x1020
     - 4
     - RO
     - **WARP_ID**: Warp index within the block
   * - 0x1024
     - 4
     - RO
     - **LANE_ID**: Lane (thread) index within warp (0 to warp_size-1)

Thread Synchronization (0x2000 - 0x2FFF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 15 10 10 65

   * - Offset
     - Size
     - Access
     - Description
   * - 0x2000
     - 4
     - WO
     - **BARRIER**: Block-level barrier. Write any value to synchronize
       all threads in the current thread block.
   * - 0x2004
     - 4
     - RW
     - **THREAD_MASK**: Active thread mask for warp-level operations
   * - 0x2010
     - 4
     - RW
     - **BALLOT**: Warp vote - returns mask of threads with non-zero value
   * - 0x2020
     - 4
     - RW
     - **SHUFFLE**: Warp shuffle - exchange data between lanes.

       Write: ``[4:0]=source_lane, [31:5]=data``

       Read: returns data from source lane

Video RAM (BAR 2)
-----------------

BAR 2 maps the device's video RAM, which stores:

- Kernel code (RISC-V instructions)
- Kernel arguments
- Input/output data buffers
- Shared memory (mapped per thread block during execution)

Memory Layout (suggested)
~~~~~~~~~~~~~~~~~~~~~~~~~

::

    VRAM Base
    +------------------+ 0x00000000
    |   Kernel Code    |
    +------------------+ 0x00100000 (1 MiB)
    |  Kernel Args     |
    +------------------+ 0x00101000
    |                  |
    |   Data Buffers   |
    |                  |
    +------------------+ VRAM_SIZE

The driver is responsible for managing VRAM allocation.

Doorbell Registers (BAR 4)
--------------------------

BAR 4 provides doorbell registers for command submission (future extension).

::

    0x0000: Command Queue 0 Doorbell (write queue tail pointer)
    0x0004: Command Queue 1 Doorbell
    ...

Interrupt Handling
------------------

The device supports MSI-X interrupts with the following vectors:

======= ================================
Vector  Description
======= ================================
0       Kernel execution complete
1       DMA transfer complete
2       Error interrupt
3-63    Reserved for future use
======= ================================

Programming Model
-----------------

Basic Operation Sequence
~~~~~~~~~~~~~~~~~~~~~~~~

1. **Initialize device**: Set ``GLOBAL_CTRL.enable = 1``

2. **Upload kernel code**: Use DMA to copy kernel binary to VRAM

3. **Upload input data**: Use DMA to copy input buffers to VRAM

4. **Configure kernel dispatch**:

   - Set ``KERNEL_ADDR`` to point to kernel code in VRAM
   - Set ``KERNEL_ARGS`` to point to arguments structure in VRAM
   - Set grid and block dimensions

5. **Dispatch kernel**: Write to ``DISPATCH`` register

6. **Wait for completion**: Poll ``GLOBAL_STATUS.busy`` or wait for interrupt

7. **Download results**: Use DMA to copy output buffers from VRAM

Example: Vector Addition
~~~~~~~~~~~~~~~~~~~~~~~~

Host code (pseudocode)::

    // Upload kernel code
    dma_write(kernel_binary, VRAM_KERNEL_ADDR, kernel_size);

    // Upload input vectors A and B
    dma_write(vector_a, VRAM_DATA_A, vector_size);
    dma_write(vector_b, VRAM_DATA_B, vector_size);

    // Setup arguments (addresses in VRAM)
    struct args {
        uint32_t a_addr = VRAM_DATA_A;
        uint32_t b_addr = VRAM_DATA_B;
        uint32_t c_addr = VRAM_DATA_C;
        uint32_t n = vector_length;
    };
    dma_write(&args, VRAM_ARGS, sizeof(args));

    // Configure dispatch
    write_reg(KERNEL_ADDR_LO, VRAM_KERNEL_ADDR);
    write_reg(KERNEL_ARGS_LO, VRAM_ARGS);
    write_reg(GRID_DIM_X, (vector_length + 255) / 256);
    write_reg(GRID_DIM_Y, 1);
    write_reg(GRID_DIM_Z, 1);
    write_reg(BLOCK_DIM_X, 256);
    write_reg(BLOCK_DIM_Y, 1);
    write_reg(BLOCK_DIM_Z, 1);

    // Dispatch
    write_reg(DISPATCH, 1);

    // Wait for completion
    while (read_reg(GLOBAL_STATUS) & STATUS_BUSY);

    // Download result
    dma_read(VRAM_DATA_C, vector_c, vector_size);

Kernel code (RISC-V assembly concept)::

    # Load thread/block IDs from CTRL registers
    li      t0, CTRL_BASE + THREAD_ID_X
    lw      a0, 0(t0)           # thread_id_x
    li      t0, CTRL_BASE + BLOCK_ID_X
    lw      a1, 0(t0)           # block_id_x
    li      t0, CTRL_BASE + BLOCK_DIM_X
    lw      a2, 0(t0)           # block_dim_x

    # Calculate global thread ID: gid = block_id * block_dim + thread_id
    mul     a3, a1, a2
    add     a3, a3, a0          # a3 = global_id

    # Load kernel arguments
    lw      a4, 0(ARGS_BASE)    # a_addr
    lw      a5, 4(ARGS_BASE)    # b_addr
    lw      a6, 8(ARGS_BASE)    # c_addr
    lw      a7, 12(ARGS_BASE)   # n

    # Bounds check
    bge     a3, a7, done

    # Calculate addresses
    slli    t0, a3, 2           # offset = gid * 4
    add     t1, a4, t0          # &A[gid]
    add     t2, a5, t0          # &B[gid]
    add     t3, a6, t0          # &C[gid]

    # C[gid] = A[gid] + B[gid]
    lw      t4, 0(t1)
    lw      t5, 0(t2)
    add     t6, t4, t5
    sw      t6, 0(t3)

    done:
    # Thread exit (return to scheduler)
    ebreak

Implementation Notes
--------------------

QEMU Device Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~

The device is implemented in ``hw/gpgpu/gpgpu.c`` with the following components:

- ``GPGPUState``: Main device state structure
- ``gpgpu_mmio_ops``: MMIO operations for BAR 0
- ``gpgpu_vram_ops``: Memory operations for BAR 2
- ``gpgpu_realize()``: Device initialization
- ``gpgpu_dispatch_kernel()``: Kernel execution simulation

Compute Unit Simulation
~~~~~~~~~~~~~~~~~~~~~~~

Each compute unit maintains:

- Warp schedulers
- Register files (per thread)
- Program counters (per warp, with divergence stack)
- Shared memory

The simulation can operate in:

1. **Functional mode**: Execute threads sequentially for correctness
2. **Cycle-approximate mode**: Model warp scheduling and memory latency

Future Extensions
-----------------

- Command queue support (via BAR 4 doorbells)
- Multiple kernel concurrent execution
- Texture sampling units
- L1/L2 cache simulation
- Atomic operations
- Warp shuffle/vote instructions
- Debug registers and breakpoints

References
----------

- Vortex GPGPU: https://github.com/vortexgpgpu/vortex
- RISC-V ISA Specification: https://riscv.org/specifications/
- CUDA Programming Guide (for execution model concepts)
- AMD GCN Architecture Whitepaper
