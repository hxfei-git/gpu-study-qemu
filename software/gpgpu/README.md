<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->
# 进阶实验一：类 CUDA 最小软件栈

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
QEMU
  hw/gpgpu/gpgpu.c：PCI、BAR、DMA、MSI-X
                  │ 代码、Args、grid/block、VRAM
                  ▼
  hw/gpgpu/gpgpu_core.c：RV32 执行器
                  │ 逐线程读写 VRAM
                  ▼
  kernel 完成 → MSI-X → 驱动 completion → ioctl 返回
```

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
```

Host 构建需要 C 编译器、ar 和 Python 3；QEMU 依赖见仓库 README。
首次 prepare 下载并校验官方 Alpine ARM64 ISO，在客体里安装匹配的 Linux
内核开发包。之后使用保存的 developer.raw 增量编译，无需重新安装客体。
客体程序由 ARM64 gcc 原生构建；RV32 kernel 使用同一份汇编源码生成。

所有二进制、镜像与日志保存在仓库 `build/`。端到端测试记录在
`build/gpgpu-linux-module/stack-test.json`，详细输出见同目录的
`stack-console.log` 与 `stack-test.log`。脚本按 demo、测试数量与退出码
判定成功，并检查中断计数和客体内核日志。

本轮单独构建了 RISC-V QTest 环境，基础测试与新增回归可这样重跑：

```sh
QTEST_QEMU_BINARY=build/gpgpu-qtest/qemu-system-riscv64 \
  build/gpgpu-qtest/tests/qtest/qos-test \
  -p /riscv64/virt/generic-pcihost/pci-bus-generic/pci-bus/gpgpu/gpgpu-tests \
  --tap -k
```

## 本轮验证记录

2026-10-07 在当前仓库的 `hxfei-qemu` 分支验证。Linux 客体为 ARM64
Alpine，内核 `6.18.55-0-virt`，QEMU 使用 TCG 的 `virt,gic-version=3`。

三个 demo 全部通过，逐元素核对 17,953 个输出：vector add 17,003 个，
矩阵乘 437 个（`19×17` 乘 `17×23`），ReLU 513 个。计算期间 MSI-X
计数增量为 kernel=3、DMA=21、error=0。矩阵使用 `grid=(3,3,1)`、
`block=(8,8,1)`；vector add 使用 `grid=(266,1,1)`、`block=(64,1,1)`；
ReLU 使用 `grid=(6,1,1)`、`block=(96,1,1)`。

驱动与运行时测试 68/68 通过，包括 `64 KiB + 37` 字节的分块搬运、两个
线程交错 DMA、360 个三维索引输出（8 个 block，每个 45 个线程）、非法
mmap/ioctl/分配/派发参数，以及坏 kernel 的错误 MSI-X 与后续恢复。
装卸载检查 5/5，端到端流程末尾卸载重载 2/2，客体日志未发现 Oops、BUG
或 panic。验证记录包含 module/demo/test 二进制 SHA-256。

RISC-V 和 ARM64 `virt` 上均为 21/21 GPGPU QTest 通过：原有评分测试
17/17、已有 `simt-reset-all` 回归 1/1、新增 DMA、kernel ABI 与 fault
回归 3/3。日志分别是 `build/gpgpu-device-riscv-qtest.log` 与
`build/gpgpu-device-qtest.log`。Host 与 ARM64 客体 C 构建均启用
`-Wall -Wextra -Werror`，汇编器的 4 个编码测试方法通过。

设备提交 checkpatch 为 0 errors、0 warnings。Linux 驱动提交出现 4 个
`void __user *` 参数的指针空格误报：仓库检查器只识别 `__force`，未识别
Linux 的 `__user` 注解；保留标准 Linux 注解，原生编译通过。

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
