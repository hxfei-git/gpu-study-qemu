<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->
# 进阶实验一：类 CUDA 最小软件栈

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| QEMU | Quick Emulator | 仿真工具 |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU | Graphics Processing Unit | 图形处理器 |
| CUDA | Compute Unified Device Architecture | 统一计算设备架构 |
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
| MMIO | Memory-Mapped I/O | 内存映射输入输出 |
| SIMT | Single Instruction, Multiple Threads | 单指令多线程 |
| FP32 / FP16 / FP8 / FP4 | Floating Point 32 / 16 / 8 / 4-bit | 对应位宽的浮点格式 |
| BF16 | Brain Floating Point 16-bit | 16 位浮点格式 |
| ReLU | Rectified Linear Unit | 线性整流函数 |
| CP | Command Processor | 命令处理器 |
| TCG | Tiny Code Generator | QEMU 动态翻译后端 |
| TLS | Transport Layer Security | 传输层安全协议 |
| JSON | JavaScript Object Notation | JavaScript 对象表示法 |
| ISO | International Organization for Standardization | 此处指 ISO 9660 光盘镜像 |
| SHA-256 | Secure Hash Algorithm 256-bit | 256 位安全散列算法 |
| BDF | Bus, Device, Function | PCI 总线、设备、功能号 |

第一阶段的成果放在 `software/gpgpu/`。ARM64 Linux 应用通过 libgpgpu
提交数据和 kernel，QEMU 中的 GPGPU 执行 RV32 代码，再把结果传回应用。
Host 在本文中指 Linux 客体里的应用；QEMU 进程运行在物理宿主机上。

## 架构与数据路径

```text
Linux 客体
  demos/gpgpu-demo.c / tests/test-runtime.c
                  │ gpgpuMalloc / gpgpuMemcpy / gpgpuLaunchKernel
                  ▼
  runtime/gpgpu.c：libgpgpu
                  │ ioctl + mmap
                  ▼
  linux/gpgpu_pci.c：/dev/gpgpu0
                  │ BAR0 描述符与派发寄存器
                  │ PCI DMA：客体 RAM ↔ VRAM
                  ▼
QEMU（gpgpu.c 注册 PCI 设备和 BAR）
  hw/gpgpu/gpgpu_regs.c：BAR0 寄存器路由
                  ├─ gpgpu_dma.c：客体 RAM ↔ VRAM
                  └─ gpgpu_dispatch.c：读取代码、Args、grid/block
                  ▼
  hw/gpgpu/gpgpu_core.c：RV32 执行器
                  │ 逐线程读写 VRAM
                  ▼
  kernel 完成 → gpgpu_irq.c：MSI-X → 驱动 completion → ioctl 返回
```

阅读设备代码时，先从 `gpgpu_regs.c` 找到寄存器所属模块，再沿该模块的
读写函数追踪操作。`gpgpu_regs.h` 保存寄存器偏移和枚举，`gpgpu.h` 保存
设备状态与模块接口。`dma.status` 使用 `GPGPUDMAStatus` 枚举，取值为
IDLE、BUSY、COMPLETE、ERROR；中断和全局状态使用可组合的枚举位标志。
BAR4 的占位回调位于 `gpgpu_doorbell.c`。

驱动匹配 PCI `1234:1337`，申请 BAR0、BAR2 和 BAR4。BAR0 用于控制、
DMA 和 kernel 派发，BAR2 表示设备显存资源；驱动保留 BAR4，门铃和命令队列
留到第二阶段。驱动申请三个 MSI-X 向量，分别处理计算完成、DMA 完成和设备
错误。请求等待中断完成，超过 5 秒返回 `-ETIMEDOUT` 并复位寄存器状态。

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

## 运行时接口

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

`module-test.py` 的各入口接受 `--iommu none|smmuv3`。启用时使用
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
`-EOPNOTSUPP`。命令队列、Doorbell、stream、异步事件与多租户隔离留到后续
阶段。驱动检查请求的分配范围，但 RV32 kernel 仍可访问设备 VRAM，不能
作为不可信 kernel 的沙箱。设备已标记不可迁移，需补齐 VRAM、执行上下文
和 DMA 状态保存后才能支持迁移。

单次派发限制为 4096 个 block、每个 block 1024 个线程、总计 65536 个
线程，整体执行预算为 `8 * 1024 * 1024` 条线程指令。num_cus 与
warps_per_cu 是 DEV_CAPS 报告的参数，目前不改变串行调度方式。
