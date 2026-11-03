# GPGPU 协议版本 5 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU / GPUVM / GPUVA | Graphics Processing Unit / GPU Virtual Memory / GPU Virtual Address | 图形处理器／GPU 虚拟内存／GPU 虚拟地址 |
| CPU | Central Processing Unit | 中央处理器 |
| ABI | Application Binary Interface | 应用二进制接口 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| BAR | Base Address Register | PCI 基址寄存器 |
| BO | Buffer Object | 持有固定存储的缓冲对象 |
| CP | Command Processor | 命令处理器 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| IH | Interrupt Handler | 本实验中的通知记录 Ring |
| RV32 / RV64 | 32-bit / 64-bit RISC-V | 32／64 位 RISC-V |
| TLB | Translation Lookaside Buffer | 地址翻译缓存 |
| VPN | Virtual Page Number | 虚拟页号，即 GPUVA 除以 4 KiB |
| VRAM | Video RAM | 设备本地显存 |

本文列出协议从版本 4 到版本 5 的全部差异，供核对 [CP/MEC 规格](<../../docs/CP_MEC 第一版规格.md>)、[GPUVM 规格](<../../docs/GPUVM 第一版规格.md>)与[硬件手册](<../../docs/GPGPU_硬件手册.html>)时使用。版本 3 到版本 4 的差异见 [PROTOCOL_V4.md](./PROTOCOL_V4.md)。

协议常量的唯一来源是 `include/gpgpu/`，下文引用的名称都定义在那里。版本号仍只有 `gpgpu_version.h` 中的一个常量，其余版本值都由它派生：

```text
GPGPU_PROTOCOL_VERSION = 5
  ├─ BAR0 DEV_VERSION      = 0x0005_0000（高 16 位为协议版本）
  ├─ BAR0 VM_VERSION       = 5，固件读到 5 才写回 VM_FIRMWARE = 5
  ├─ Kernel 描述对象 kernel_abi_version = 5
  ├─ DRM ioctl 参数 version = 5，INFO 返回 version = 5
  └─ DRM driver major      = 5
```

设备模型、RV64 CP 固件、Linux 驱动、用户库和 Kernel 必须一起更新。混用版本 4 的组件时，驱动 probe 或固件握手会失败；版本 4 的 16 字节描述对象因 `kernel_abi_version` 不符被 CP 拒绝。示例程序输出 `GPGPU DRM ABI=5 GPUVM=on|off`。

## 1. GPUVA 扩为 48 位，页表改为四级

版本 4 的 GPUVA 只有 32 位，三级页表的根表只用 4 项。ARM64 客体的用户地址有 48 位，CPU 指针放不进 32 位 GPUVA。版本 5 的 GPUVA 有效 48 位，页表改为四级，每级 9 位索引：

```text
GPUVA 位   63..48    47..39   38..30   29..21   20..12   11..0
用途       必须为 0   L0 索引   L1 索引   L2 索引   L3 索引   页内偏移

例：0x0000_ffff_8a40_2234 → L0 0x1ff、L1 0x1fe、L2 0x52、L3 0x2，页内偏移 0x234
```

相关常量为 `GPGPU_VM_LEVELS`（4）、`GPGPU_VM_INDEX_BITS`（9）、`GPGPU_VM_LEVEL_SHIFT(level)`、`GPGPU_VM_VA_BITS`（48）和 `GPGPU_VM_VA_LIMIT`（`0x1_0000_0000_0000`）。版本 4 的 `GPGPU_VM_ADDRESS_LIMIT` 同时当作 GPUVA 上限和物理上限，现在拆成 `GPGPU_VM_VA_LIMIT` 与 `GPGPU_PHYS_ADDRESS_LIMIT`（4 GiB）：用户地址按前者检查，MQD、管理队列和物理布局按后者检查。位 63..48 非零的用户访问记 `GPGPU_VM_FAULT_RANGE`。

以下保持不变：页表项格式（地址域 [31:12]，[63:32] 保留）、SYSTEM 与 R/W/X 位、32 位的页表根、32 位 GPU 物理地址布局与 Outbound 窗口、TLB 组织（64 组 × 4 路，组号 VPN % 64）。VPN 从 20 位变为 36 位。

## 2. 失效范围寄存器改为 LO/HI 对

36 位的 VPN 放不进一个 32 位寄存器，BAR0 又只接受 4 字节访问，所以范围寄存器改为两对：

```text
版本 4                                版本 5
0x0918 VM_INV_VPN_START              0x0930 / 0x0934  VM_INV_VPN_START_LO / HI
0x091c VM_INV_VPN_END                0x0938 / 0x093c  VM_INV_VPN_END_LO / HI
                                     0x0918、0x091c 保留，写入锁存 INVALID_CMD
```

RANGE 请求的终点上限从 2^20 改为 2^36。REQ、ACK、三个 TLB 计数和上下文寄存器的偏移与语义不变。

## 3. 执行器改为 RV64，描述对象改为 24 字节

计算执行器从 RV32 改为 RV64：整数寄存器和 PC 为 64 位，现有指令子集按 RV64 语义执行：立即数符号扩展到 64 位，LW、LUI、FCVT.W.S 的结果也符号扩展，SLLI 的移位量为 6 位，分支按 64 位比较。新增 LD、SD。浮点寄存器仍为 32 位单精度。W 后缀指令等不在子集内，执行时报指令错误。

Kernel 描述对象的代码入口改为 64 位：

```text
版本 4（16 字节）                     版本 5（24 字节）
0x00 u32 code_entry                  0x00 u64 code_entry
0x04 u32 code_size                   0x08 u32 code_size
0x08 u32 kernarg_size                0x0c u32 kernarg_size
0x0c u32 kernel_abi_version = 4      0x10 u32 kernel_abi_version = 5
                                     0x14 u32 reserved = 0
```

CP 把入口高 32 位写入 `KERNEL_ADDR_HI`。Dispatch 的 `KERNEL_ADDR_HI`、`KERNEL_ARGS_HI` 可以非零，代码和参数范围按 GPUVA 上限检查。运行时的参数块改为每项 8 字节，Kernel 用 `ld` 读取指针和标量；`gpgpu_rt_dispatch()` 接收 `uint64_t` 数组，`gpgpu_rt_arg_address()` 删除。

## 4. GPUVA 与 CPU 地址一致

版本 4 由库在可分配区间里自己挑选 GPUVA。版本 5 按 AMD libhsakmt 的做法，先申请 CPU 地址，再让 GPU 使用同一个地址：

```text
1. 库用 mmap(PROT_NONE) 预留对齐的 CPU 地址 va
2. BO_CREATE 分配 Host 或 VRAM 存储
3. 经 render node 用 MAP_FIXED 把对象映射到 va：Host 对象可读写，VRAM 对象只能 PROT_NONE
4. MAP(address = va)：GPUVM 开启时 gpu_address = va
```

驱动给 VRAM 对象也返回 mmap 偏移，但只接受 `PROT_NONE` 映射，VRAM 内容仍经 COPY 读写。释放时先 UNMAP，成功后才解除 CPU 映射；MAP 结果未知时，这段地址换回 `PROT_NONE` 占位，不归还给进程，避免 GPU 页表仍指向它时被分给别的映射。INFO 在 GPUVM 开启时报告 `address_bits = 48` 与区间 `[0x1000, 0x1_0000_0000_0000)`，关闭时报告 32 位与零个区间。驱动不检查 MAP 的地址是否等于 CPU 地址，这条约定由库保证。

## 5. 通知页由用户态创建

版本 4 的通知页由驱动在第一次 MAP 时分配，固定映射在 GPUVA `0xffff_f000`。版本 5 删除 `GPGPU_VM_NOTIFY_VA`，通知页由库在 `gpgpuKmtOpen()` 中创建：

```text
BO_CREATE(Host, 4 KiB, flags = GPGPU_BO_EVENT_PAGE) → 驱动登记为会话的通知页
  → 按第 4 节映射到 CPU 地址 va → MAP(va, 读写)
EVENT_CREATE → 槽 k，mailbox = 通知页 GPU 地址 + 8k
```

`struct gpgpu_bo_create` 中原本必须为 0 的 `reserved` 改名为 `flags`，布局不变；`GPGPU_EVENT_PAGE_SIZE` 为 4096。每个会话只能登记一个通知页，须为一页 Host 对象，MAP 时须可写；登记期间驱动为它持有一个使用计数，对它 UNMAP 返回 `-EBUSY`；会话关闭时驱动撤销登记，再随普通绑定回收。通知页未登记或未绑定时 EVENT_CREATE 返回 `-ENOSPC`。IH 记录格式、Signal 布局和完成顺序不变。

## 6. 未改变的部分

- MQD、管理命令与结果区、AQL 包、Signal、IH 记录的布局与字段偏移。
- 除第 2 节外的 BAR0 寄存器偏移，BAR2 与 BAR4；CP 私有地址空间（`gpgpu_cp_map.h`）。
- 除 `struct gpgpu_bo_create` 的 `flags` 外，DRM ioctl 编号与参数结构体布局。
- VRAM 默认 64 MiB、Outbound 固定为 `0x8000_0000` 起的 2 GiB、31 位 DMA mask。
