# GPGPU 协议版本 8 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| ABI | Application Binary Interface | 应用二进制接口 |
| BAR | Base Address Register | PCI 基址寄存器 |
| PCI | Peripheral Component Interconnect | 外设互连总线 |
| CP | Command Processor | 命令处理器 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| FP32 / FP64 | 32-bit / 64-bit Floating Point | 32 位／64 位浮点数 |
| W8 | 8-bit Weight Storage | 8 位权重存储 |
| FMA | Fused Multiply-Add | 融合乘加 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| GPUVM | GPU Virtual Memory | GPU 虚拟内存 |
| SIMD | Single Instruction, Multiple Data | 单指令多数据 |

本文说明版本 7 到版本 8 的组件升级。此前的显存与固件布局变化见
[PROTOCOL_V7.md](./PROTOCOL_V7.md)。构建命令、运行结果与数值问题的定位记录只在
[软件栈第 5 节](<./README.md#build-and-validation>)维护。

## 1. 组件统一使用版本 8

设备执行 W8 张量块指令时，必须按公开的补偿累加规则产生结果。旧设备仍会把同一条
指令解释为逐项 FMA，得到不同的逐位结果。即使编码和参数布局没有改变，也需要通过
版本检查拒绝混用；设备、固件、驱动、用户库与 Kernel 描述对象必须一起更新。

版本值仍从同一个头文件派生：

```text
GPGPU_PROTOCOL_VERSION = 8
  ├─ BAR0 DEV_VERSION = 0x0008_0000
  ├─ BAR0 VM_VERSION = 8，固件核对后写回 VM_FIRMWARE = 8
  ├─ Kernel 描述对象 kernel_abi_version = 8
  ├─ DRM ioctl 参数 version = 8，INFO 返回 version = 8
  └─ DRM driver major = 8
```

> **[SOURCE]** 协议 8 工作区，基线 `8a435ac`，含未提交改动：
> [gpgpu_version.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_version.h:17:1>)，
> [第 17～22 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_version.h:17:1>)。
> 版本号只有一处定义，设备版本值与描述对象版本由它推导。

## 2. W8 固定 K 块的补偿累加

W8 权重仍先逐项乘 scale，独立舍入到 FP32；没有把 scale 移到点积末尾，也没有使用
FP64 作为设备累加器。每条指令在自己的 K 块内用 FP32 TwoProduct/TwoSum 保存乘积与
加法的低位，块末只写回一个 FP32 结果。下一块从这个结果继续，不跨块保存补偿量。
末块的 bias 仍独立相加。

整数指数预筛不通过时，该输出直接从原 seed 按 K 递增执行严格 FMA，不先试算可能
溢出的补偿中间值。SoftFloat、允许的宿主 SIMD 路径和软件标量后端使用同一公式及
块边界。重新划分 K 块可能改变逐位结果；精确公式、共享指数界及异常处理见
[硬件手册的张量块指令](<../../docs/GPGPU_硬件手册.html#tensor-block>)。

> **[SOURCE]** 同一协议 8 工作区：
> [gpgpu_tensor_abi.h](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_tensor_abi.h:10:1>)，
> [第 10～18 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_tensor_abi.h:10:1>)
> 定义数学合同；[第 66～84 行](<vscode://vscode-remote/wsl+Ubuntu/home/book/1.project/gpu_study/4.experiments/gpu-study-qemu/software/gpgpu/include/gpgpu/gpgpu_tensor_abi.h:66:1>)
> 定义块上限与预筛边界。

## 3. 保留的接口与应用边界

FP32 线性投影、批次矩阵乘和卷积继续按 K 递增逐次 FMA。张量指令编码、通用参数前缀、
AQL、队列、完成记录、GPUVM、缓存属性以及显存和固件布局均不变。标准浮点指令的
语义也不改变。

客体聊天、图像处理和常驻模型会话属于应用层。模型程序文件仍是应用私有格式，
不增加硬件模型名称、模型加载指令或新的设备协议字段；应用分层见
[软件栈第 4.1 节](<./README.md#full-model-runtime>)。
