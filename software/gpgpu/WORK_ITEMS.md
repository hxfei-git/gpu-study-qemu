# 进阶实验一工作项

各项完成后单独提交。提交正文说明做了什么、核心思想、完成方法和验证结果。

1. **软件栈设计与 ABI**：规定运行时、驱动和设备的调用关系，固定 ioctl
   数据结构、VRAM 地址语义和 RV32 kernel 入口。
2. **设备的软件接口**：补齐参数寄存器、RV32 数据读取与分支、真实 DMA
   和 MSI-X 通知；保留基础测题的行为。
3. **Linux PCI 驱动**：注册 PCI 与字符设备，管理 BAR 和 MSI-X，以可
   mmap 的一致性 DMA 缓冲区提交搬运描述符，并管理 VRAM 分配与同步派发。
4. **运行时与编程模型**：实现 libgpgpu API，提供 RV32 汇编头文件、kernel
   构建方式和三个 FP32 demo；应用逐元素核对设备返回的数据。
5. **客体端到端验证**：复用 ARM64 Linux 环境，自动构建和执行驱动、运行时、
   demo 与错误路径测试，核对 MSI-X 计数并检查基础 QTest 的实际通过数。
6. **第一阶段成果文档**：记录架构、数据路径、运行命令、验证结果和能力边界，
   同步父目录的四阶段大纲与仓库 AGENTS.md。

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

基础 17 道测题涵盖的是已有教学设备能力，本阶段将逐项说明这些能力如何进入
软件栈。不会把仅检查寄存器配置的 QTest 通过当作 DMA 与中断链路完成的依据。
RV32 kernel 可访问设备 VRAM，第一阶段的独占会话不提供多进程地址隔离。
