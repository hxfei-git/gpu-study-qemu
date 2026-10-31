# GPGPU 协议版本 4 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU / GPUVM / GPUVA | Graphics Processing Unit / GPU Virtual Memory / GPU Virtual Address | 图形处理器／GPU 虚拟内存／GPU 虚拟地址 |
| ABI | Application Binary Interface | 应用二进制接口 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| BAR | Base Address Register | PCI 基址寄存器 |
| CP | Command Processor | 命令处理器 |
| DMA | Direct Memory Access | 直接内存访问 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| IH | Interrupt Handler | 本实验中的通知记录 Ring |
| IOVA | I/O Virtual Address | 设备 DMA 虚拟地址 |
| LRU / MRU | Least / Most Recently Used | 最久未用／最近使用 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| RV64 | 64-bit RISC-V | 原生 CP 使用的 RISC-V 位宽 |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| VMID | Virtual Memory Identifier | 虚拟地址空间编号 |
| VPN | Virtual Page Number | 虚拟页号，即 GPUVA 除以 4 KiB |
| VRAM | Video RAM | 设备本地显存 |

本文列出协议从版本 3 到版本 4 的全部差异，供核对 [CP/MEC 规格](<../../docs/CP_MEC 第一版规格.md>)、[GPUVM 规格](<../../docs/GPUVM 第一版规格.md>)与[硬件手册](<../../docs/GPGPU_硬件手册.html>)时使用。版本 2 到版本 3 的差异见 [PROTOCOL_V3.md](./PROTOCOL_V3.md)。

协议常量的唯一来源是 `include/gpgpu/`，下文引用的名称都定义在那里。版本号仍只有 `gpgpu_version.h` 中的一个常量，其余版本值都由它派生：

```text
GPGPU_PROTOCOL_VERSION = 4
  ├─ BAR0 DEV_VERSION      = 0x0004_0000（高 16 位为协议版本）
  ├─ BAR0 VM_VERSION       = 4，固件读到 4 才写回 VM_FIRMWARE = 4
  ├─ Kernel 描述对象 kernel_abi_version = 4
  ├─ DRM ioctl 参数 version = 4，INFO 返回 version = 4
  └─ DRM driver major      = 4
```

设备模型、RV64 CP 固件、Linux 驱动、用户库和 Kernel 描述对象必须一起更新，混用版本 3 的组件时，驱动 probe 或固件握手会失败。示例程序输出 `GPGPU DRM ABI=4 GPUVM=on|off`，`test-gpgpu-stack` 从 `gpgpu_version.h` 读取版本号后核对这一行。

## 1. Outbound 固定为 0x8000_0000 起的 2 GiB

版本 3 的 Outbound 紧接在 VRAM 之后，起点等于 `vram_size`，Host 对象的 GPU 物理地址随 VRAM 容量变化。版本 4 把窗口固定下来，VRAM 末端到窗口起点之间是空洞：

```text
版本 3（64 MiB VRAM）                     版本 4
[0, 0x0400_0000)        VRAM              [0, vram_size)                VRAM
[0x0400_0000, 4 GiB)    Outbound          [vram_size, 0x8000_0000)      空洞，不可路由
                                          [0x8000_0000, 0x1_0000_0000)  Outbound，2 GiB

Host IOVA 0x4100_2000 → 0x4500_2000       Host IOVA 0x4100_2000 → 0xc100_2000
```

`GPGPU_OUTBOUND_BASE` 和 `GPGPU_OUTBOUND_SIZE` 都是 `0x8000_0000`。BAR0 `0x0b00～0x0b10` 仍只读，读回 GPU_BASE = `0x8000_0000`、SIZE = `0x8000_0000`、HOST = 0；驱动启动时核对这些值。访问空洞与访问未使能的窗口按同样的方式失败：用户访问记 GPUVM 故障，CP 管理域访问记队列错误，DMA 置 `DMA_FAULT`。驱动的 DMA mask 仍为 31 位，正好覆盖窗口对应的 2 GiB IOVA。

## 2. 页表项 bit 1 定义为 SYSTEM

版本 3 的页表项直接保存 GPU 物理地址，bit 1 必须为 0。版本 4 把 bit 1 定义为 `GPGPU_VM_SYSTEM`，中间项和叶子项都可以设置：

```text
叶子项 0x4100_2000 | SYSTEM | VALID | R | W
  → 地址域是 Host IOVA，GPU 物理页 0x8000_0000 + 0x4100_2000 = 0xc100_2000

叶子项 0x0001_1000 | VALID | R | X
  → 地址域是 VRAM 偏移，GPU 物理页 0x0001_1000

版本 3 写法 0xc100_2000 | VALID | R
  → 版本 4 按 VRAM 偏移解释，超出 VRAM，报 GPGPU_VM_FAULT_TABLE
```

SYSTEM=1 时整页须在 2 GiB IOVA 窗口内，SYSTEM=0 时整页须在 VRAM 内并避开首页和固件区；地址域与 SYSTEM 不符时报 `GPGPU_VM_FAULT_TABLE`。bit 2 及其他未定义位仍保留。中间项允许的位为 `GPGPU_VM_DIR_FLAGS`，叶子项为 `GPGPU_VM_FLAGS`。页表根没有 SYSTEM 位，上下文寄存器仍保存根页的 GPU 物理地址。

驱动给 Host 对象和通知页写 DMA 地址并置 SYSTEM，VRAM 对象写 VRAM 偏移，表页仍从 VRAM 分配。`GPGPU_VM_PERMS` 不含 SYSTEM，用户映射请求带这一位时仍被拒绝。

## 3. TLB 改为 64 组 × 4 路并增加计数

版本 3 的 TLB 是 64 项直接映射。版本 4 改为 `GPGPU_VM_TLB_SETS` 组、每组 `GPGPU_VM_TLB_WAYS` 路，即 64 组 × 4 路，组号为 VPN % 64，标签为 VMID 与 VPN，只缓存叶子翻译。命中时该路成为 MRU；未命中时遍历页表，成功后先填本组的无效路，四路都有效时替换 LRU 路。

```text
第 5 组依次访问 VPN 5、69、133、197：四路占满，VPN 5 最久未用
再访问 VPN 261：替换 VPN 5 所在的路，EVICTIONS 加 1
```

每组怎样记录各路的使用先后、命中和失效时顺序如何变化，见 [GPUVM 规格第 5.2.1 节](<../../docs/GPUVM 第一版规格.md#gpuvm-tlb-lru>)。

新增三个只读计数：`0x0920` VM_TLB_HITS、`0x0924` VM_TLB_MISSES、`0x0928` VM_TLB_EVICTIONS。它们全设备共用，32 位回绕，设备复位时清零，失效不清零。

## 4. 失效接口改为 REQ/ACK 并支持 VPN 范围

版本 3 写 `0x0910` VM_INVALIDATE 一个 VMID，从 `0x0914` VM_STATUS 读 BUSY/ACK/ERROR，只能整 VMID 失效。版本 4 换成下面四个寄存器：

```text
0x0910 VM_INV_REQ        写触发：bit[2:1] 为 VMID 位图，bit 16 为 RANGE
0x0914 VM_INV_ACK        只读：bit[2:1] 为完成位图，bit 31 为 ERROR
0x0918 VM_INV_VPN_START  起始 VPN
0x091c VM_INV_VPN_END    结束 VPN，闭区间
```

例：失效 VMID 1 的 GPUVA `0x0040_0000～0x0040_2fff`，依次写 START = `0x400`、END = `0x402`、REQ = `GPGPU_VM_INV_VMID(1) | GPGPU_VM_INV_RANGE`，ACK 读回 `0x2`。

设备在写 REQ 的同一次寄存器访问中完成失效，没有忙状态。请求含保留位（包括 VMID 0 的 bit 0）、位图为空、RANGE=1 且 START 大于 END 或 END 不小于 2^20，或任一所选 VMID 仍有激活的队列或在途访问时，整体拒绝，不清任何项，ACK 只置 `GPGPU_VM_INV_ERROR`。未启用的 VMID 可以失效。gpuvm=off 时写 REQ、START、END 锁存 `INVALID_CMD`。

驱动一次只请求一个 VMID，核对 ACK 等于 `GPGPU_VM_INV_VMID(vmid)`；绑定和解绑对象只失效该对象的 VPN 范围，启用和停用上下文失效整个 VMID。

## 5. 未改变的部分

- VRAM 默认 64 MiB、`vram_size < 2 GiB` 的限制、31 位 DMA mask，以及 Outbound CONTROL、STATUS 的偏移和语义。
- 三级 4 KiB 页表、GPUVA 布局、R/W/X 权限位、故障组与故障原因编码、上下文寄存器。
- 除 `0x0910～0x0928` 外的 BAR0 寄存器偏移，BAR2 与 BAR4。
- MQD、管理命令与结果区、AQL 包、Kernel 描述对象、Signal、IH 记录的布局与字段偏移。
- CP 私有地址空间（`gpgpu_cp_map.h`），包括 CP 本地访存窗口 `0x8000_0000～0x8000_ffff`。它属于 CP 自己的地址空间，与 GPU 物理地址中的 Outbound 起点没有关系。
- DRM ioctl 编号与参数结构体布局；只有 `version` 字段改填 4。
