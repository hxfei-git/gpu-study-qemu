# GPGPU 协议版本 7 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU / GPU | General-Purpose Computing on Graphics Processing Units / Graphics Processing Unit | GPU 通用计算／图形处理器 |
| ARM64 | Arm 64-bit Architecture | Arm 64 位架构 |
| HSA | Heterogeneous System Architecture | 异构系统架构 |
| PCI | Peripheral Component Interconnect | 外设互连总线 |
| RAM | Random Access Memory | 系统内存 |
| ABI | Application Binary Interface | 应用二进制接口 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| BAR | Base Address Register | PCI 基址寄存器 |
| BO | Buffer Object | 持有固定存储的缓冲对象 |
| CP | Command Processor | 命令处理器 |
| DMA | Direct Memory Access | 直接内存访问 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| GPUVM | GPU Virtual Memory | GPU 虚拟内存 |
| RV64 | 64-bit RISC-V | 64 位 RISC-V |
| PC | Program Counter | 程序计数器 |
| FP32 / INT8 | 32-bit Floating Point / 8-bit Integer | 32 位浮点数／8 位整数 |
| FMA | Fused Multiply-Add | 融合乘加，只舍入一次 |
| SIMD | Single Instruction, Multiple Data | 单指令多数据 |
| TCG | Tiny Code Generator | QEMU 动态翻译后端 |
| VRAM | Video RAM | 设备本地显存 |

本文说明版本 6 到版本 7 的组件升级。上一版的缓存接口见
[PROTOCOL_V6.md](./PROTOCOL_V6.md)。构建、运行命令及实际验证结果只在
[软件栈第 5 节](<./README.md#build-and-validation>)维护。

## 1. 组件统一使用版本 7

驱动需要把固件装到新的固定地址，再解除 CP 复位。固件仍是裸二进制，没有单独的
布局标识，因此版本 6 的握手不能用来确认新布局。设备、固件、驱动、用户库和
Kernel 描述对象必须一起更新，不保留旧地址兼容路径。

版本号的唯一来源仍是 `include/gpgpu/gpgpu_version.h`：

```text
GPGPU_PROTOCOL_VERSION = 7
  ├─ BAR0 DEV_VERSION = 0x0007_0000
  ├─ BAR0 VM_VERSION = 7，固件核对后写回 VM_FIRMWARE = 7
  ├─ Kernel 描述对象 kernel_abi_version = 7
  ├─ DRM ioctl 参数 version = 7，INFO 返回 version = 7
  └─ DRM driver major = 7
```

旧设备在驱动 probe 时被拒绝；旧固件不能完成新版本握手；旧用户程序的 ioctl
版本不符，旧描述对象也不能提交计算。不要只替换固件文件而保留旧驱动或旧设备。

## 2. 固定显存容量与固件地址

`gpgpu_cp_map.h` 增加固定容量 `GPGPU_VRAM_SIZE`，值为 2 GiB。固件保留大小、栈大小
和容量是布局的定义，入口、栈顶及驱动分配上界由它们推导；链接脚本也使用这份定义。
完整布局见 [CP/MEC 固件存储](<../../docs/CP_MEC 第一版规格.md#spec-firmware-layout>)。

- BAR2 扩为 2 GiB，设备属性和驱动均核对固定容量；ARM64 客体 RAM 不随之增大。
- 固件保留区移到 `[0x7fc0_0000, 0x8000_0000)`，容量仍为 4 MiB，末尾 64 KiB 为栈。
  固件装载和复位入口为 `0x7fc0_0000`，初始栈顶为 `0x8000_0000`。
- 首页继续保留。旧 60～64 MiB 区域恢复为普通可分配区，用户访问保护不再排除旧固件地址。
- Outbound 仍固定从 `0x8000_0000` 开始，容量 2 GiB；VRAM 与 Outbound 之间不再有空洞。
  GPU 物理地址位宽与 Host DMA 地址约束不变。

BAR2 是 64 位 PCI 内存 BAR。客体把它放在高位 PCI 窗口，不要求客体具有同等容量的
RAM；VRAM 后备存储由设备进程持有。

## 3. BO 分配与分块搬运

BO 不再共用旧的单对象大小上限。驱动检查请求能否页对齐、地址和长度是否可表示，
再按存储类型申请：VRAM 扣除首页、固件和已用空间；Host 对象受客体内存、DMA 范围及
实际分配结果约束。VRAM 仍要求连续区间，页表也占用同一分配池；对齐和碎片可能使
总空闲量足够的请求仍然失败。

运行时新增 `gpgpu_rt_upload_range()`、`gpgpu_rt_download_range()`，允许指定对象内偏移。
原有上传、下载入口使用偏移 0；两种入口均可通过小 staging 分块复制，最后一块可以
小于 staging。完整范围先校验，复制失败时已经完成的块不回退；不能确认复制已结束时，
禁止复用 staging，资源回收继续遵循原有停止与保留规则。

复制 ioctl 的结构、偏移和方向编码不变。分块是运行时行为，不是新增 DMA 协议。

## 4. 保留的接口约定

AQL、队列描述、Signal、完成记录与 Kernel 描述对象的字段布局不变，只更新共用版本值。
GPUVM 地址位宽、页表编码、缓存属性和关闭模式的 DEVICE 访问语义沿用版本 6；
固件保留区保护按新地址生效。版本 7 不引入完整模型应用或新的模型文件加载 ABI。

## 5. 计算子集与张量块指令

真实算子 Kernel 需要的字节读写、整数索引、FP32 除法与平方根、比较、原始位搬运和 R4 FMA 已加入计算子集；完整编码见[硬件手册第 15 章](<../../docs/GPGPU_硬件手册.html#fragment-page-stage2--gpu--gpu-datasheet--13>)，不据此声明完整 RV64IMF 支持。

有界矩阵和布局操作使用独立的 custom-0 指令，公共参数前缀由
`include/gpgpu/gpgpu_tensor_abi.h` 定义。执行器只解释通用维度、地址和格式；
模型评估操作编号仍由 `gpgpu_eval_abi.h` 与软件 Kernel 解释，硬件不依赖该评估头。
边界、编码与 FP32／INT8 计算规则见[张量块指令](<../../docs/GPGPU_硬件手册.html#tensor-block>)。

`shared_fetch` 在一轮内复用相同 PC 的取指与字段译码，仍逐 Lane 执行。
`tensor_native` 允许张量指令在安全条件满足时使用宿主 SIMD，其他情况使用 SoftFloat。
这两个启动属性不改变 AQL、Kernel 描述对象、队列错误或完成协议；计算核仍不是 TCG 执行路径。
应用选择方式与完整计时范围见[软件栈的算子回放说明](<./README.md#model-operator-replay>)。
