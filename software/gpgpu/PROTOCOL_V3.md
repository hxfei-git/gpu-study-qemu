# GPGPU 协议版本 3 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU / GPUVM / GPUVA | Graphics Processing Unit / GPU Virtual Memory / GPU Virtual Address | 图形处理器／GPU 虚拟内存／GPU 虚拟地址 |
| ABI | Application Binary Interface | 应用二进制接口 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| RV32 / RV64 | 32 / 64-bit RISC-V | 计算执行器／原生 CP 使用的 RISC-V 位宽 |
| CSR | Control and Status Register | 控制与状态寄存器 |
| MQD / HQD | Memory Queue Descriptor / Hardware Queue Descriptor | 内存／硬件队列描述符 |
| PCI | Peripheral Component Interconnect | 外设互连总线 |
| UAPI | User-space Application Programming Interface | 内核向用户态提供的接口 |
| BAR | Base Address Register | PCI 基址寄存器 |
| CP | Command Processor | 命令处理器 |
| DMA | Direct Memory Access | 直接内存访问 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| GEM | Graphics Execution Manager | DRM 内存对象管理 |
| IH | Interrupt Handler | 本实验中的通知记录 Ring |
| IOVA | I/O Virtual Address | 设备 DMA 虚拟地址 |
| IRQ | Interrupt Request | 中断请求 |
| MSI-X | Message Signaled Interrupts Extended | 扩展消息中断 |
| VRAM | Video RAM | 设备本地显存 |

本文列出协议从版本 2 到版本 3 的全部差异，供更新 [CP/MEC 规格](<../../docs/CP_MEC 第一版规格.md>)、[GPUVM 规格](<../../docs/GPUVM 第一版规格.md>)与[硬件手册](<../../docs/GPGPU_硬件手册.html>)时逐条核对。设备模型、RV64 CP 固件、Linux 驱动与用户库必须一起更新，混用版本时驱动 probe 或固件握手会失败。

协议常量的唯一来源是 `include/gpgpu/`；下文引用的名称都定义在那里。

## 1. 版本号合一

版本 2 有六个数值都为 2 的版本常量：设备的 `DEV_VERSION`、统一访存的 `GPGPU_VM_VERSION`、队列 ABI 的 `GPGPU_QUEUE_ABI_VERSION`、Kernel 描述对象的 `GPGPU_KERNEL_ABI_VERSION`、用户库的 `GPGPU_QABI_VERSION` 和 DRM 的 `GPGPU_DRM_VERSION`。代码里看不出哪几个必须同时修改。

版本 3 只保留 `GPGPU_PROTOCOL_VERSION = 3`（`gpgpu_version.h`），其余都由它派生：

```text
GPGPU_PROTOCOL_VERSION = 3
  ├─ BAR0 DEV_VERSION      = 0x0003_0000（高 16 位为协议版本）
  ├─ BAR0 VM_VERSION       = 3，固件读到 3 才写回 VM_FIRMWARE = 3
  ├─ Kernel 描述对象 kernel_abi_version = 3
  ├─ DRM ioctl 参数 version = 3，INFO 返回 version = 3
  └─ DRM driver major      = 3
```

示例程序输出 `GPGPU DRM ABI=3 GPUVM=on|off`，`test-gpgpu-stack` 检查这一行。

## 2. 错误、中断与 MSI-X 编号连续

版本 2 中这三组编号各空着一位，原先分给早期已删除的 Kernel 完成中断。版本 3 从 0 开始连续编号：

```text
ERROR_STATUS        版本 2      版本 3
  INVALID_CMD       bit 0       bit 0
  VRAM_FAULT        bit 1       bit 1
  DMA_FAULT         bit 3   →   bit 2
  IH_FAULT          bit 4   →   bit 3
  FIRMWARE          bit 5   →   bit 4

IRQ_ENABLE/STATUS/ACK
  DMA_DONE          bit 1   →   bit 0
  ERROR             bit 2   →   bit 1
  IH                bit 3   →   bit 2

MSI-X 向量（总数 4 → 3）
  DMA               1       →   0
  ERROR             2       →   1
  IH                3       →   2
```

驱动按 `GPGPU_MSIX_VECTORS` 申请中断，所有向量共用一个处理函数，重新编译即可。

## 3. Outbound 布局只读

版本 2 的五个 Outbound 布局寄存器（`0xb00`～`0xb10`）可写，但打开窗口时设备只接受复位值。版本 3 把它们改为只读，读回的仍是同一固定布局：

```text
GPU 物理地址 [vram_size, 0x1_0000_0000) ──Outbound──▶ Host IOVA [0, 4 GiB - vram_size)
```

写布局寄存器置 `OUTBOUND_STATUS.ERROR` 并锁存 `INVALID_CMD`。驱动启动时只读回布局核对，然后写 `OUTBOUND_CONTROL = ENABLE`。`OUTBOUND_CONTROL` 与 `OUTBOUND_STATUS` 的偏移和语义不变。

## 4. IH 设备错误记录

版本 2 中，固件提交的 `GPGPU_IH_DEVICE_ERROR` 记录的 `data` 字段被原样写入 `ERROR_STATUS`，固件可以借此置任意全局错误位。

版本 3 要求该记录的 `data` 必须等于 `GPGPU_ERR_FIRMWARE`，否则按 `GPGPU_IH_ERR_RECORD` 拒绝；设备只锁存 `GPGPU_ERR_FIRMWARE`。固件原本就只提交这一个值，行为不变。

## 5. DRM ioctl 编号写法

版本 2 直接写 `_IOWR('d', 0x40 + n, …)`。版本 3 按 DRM 的约定改写为 `GPGPU_DRM_IOWR(n, type)`：类型码为 DRM 的 `'d'`，编号为驱动私有命令基址 `DRM_COMMAND_BASE`（0x40）加序号 n。展开后的编号与版本 2 相同，参数结构体布局也相同，`version` 字段改填 3。

用户库运行在只有 musl 头文件的客体中，无法包含 `<drm/drm.h>`。`uapi/gpgpu_drm.h` 自行展开上述宏，`GEM_CLOSE` 仍按 DRM 核心命令 0x09 定义。

## 6. 未改变的部分

- BAR0 各寄存器组的偏移（`0x000`～`0xb18`）、BAR2 与 BAR4。
- MQD、管理命令与结果区、AQL 包、Kernel 描述对象、Signal、IH 记录的布局与字段偏移。
- CP 私有地址空间（`gpgpu_cp_map.h`）、HQD 字段偏移、GPUVM 页表项格式与 GPUVA 布局。
- 全部错误码枚举（管理结果 0～6、队列错误 0～10、IH 错误、执行器错误）。
