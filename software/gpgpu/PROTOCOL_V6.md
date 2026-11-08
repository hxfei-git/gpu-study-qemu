# GPGPU 协议版本 6 的变更

| 缩写 | 英文全称 | 中文含义 |
| --- | --- | --- |
| GPGPU | General-Purpose Computing on Graphics Processing Units | GPU 通用计算 |
| GPU / GPUVM | Graphics Processing Unit / GPU Virtual Memory | 图形处理器／GPU 虚拟内存 |
| CPU | Central Processing Unit | 中央处理器 |
| ABI | Application Binary Interface | 应用二进制接口 |
| AQL | Architected Queuing Language | HSA 队列包格式 |
| BAR | Base Address Register | PCI 基址寄存器 |
| BO | Buffer Object | 持有固定存储的缓冲对象 |
| CP | Command Processor | 命令处理器 |
| dcache | Data Cache | 数据缓存 |
| DMA | Direct Memory Access | 直接内存访问 |
| DRM | Direct Rendering Manager | Linux 图形设备框架 |
| GEM | Graphics Execution Manager | DRM 的缓冲对象管理框架 |
| IH | Interrupt Handler | 本实验中的通知记录 Ring |
| KMT | Kernel Mode Thunk | 本仓库 `lib/kmt/` 薄层的简称 |
| MQD | Memory Queue Descriptor | 内存队列描述符 |
| RV64 | 64-bit RISC-V | 64 位 RISC-V |
| VRAM | Video RAM | 设备本地显存 |
| WB | Write-Back | 写回；本文指页属性允许进入 dcache 的访问类型 |

本文列出协议从版本 5 到版本 6 的全部差异，供核对 [dcache 规格](<../../docs/dcache 第一版规格.md>)与[硬件手册](<../../docs/GPGPU_硬件手册.html>)时使用。缓存的行为、交接顺序和设计理由只在 dcache 规格中维护，本文只列接口变化。版本 4 到版本 5 的差异见 [PROTOCOL_V5.md](./PROTOCOL_V5.md)。

协议常量的唯一来源是 `include/gpgpu/`，下文引用的名称都定义在那里。版本号仍只有 `gpgpu_version.h` 中的一个常量，其余版本值都由它派生：

```text
GPGPU_PROTOCOL_VERSION = 6
  ├─ BAR0 DEV_VERSION      = 0x0006_0000（高 16 位为协议版本）
  ├─ BAR0 VM_VERSION       = 6，固件读到 6 才写回 VM_FIRMWARE = 6
  ├─ Kernel 描述对象 kernel_abi_version = 6
  ├─ DRM ioctl 参数 version = 6，INFO 返回 version = 6
  └─ DRM driver major      = 6
```

设备模型、RV64 CP 固件、Linux 驱动、用户库和 Kernel 必须一起更新。混用版本 5 的组件时，驱动 probe 或固件握手会失败；版本 5 的描述对象因 `kernel_abi_version` 不符被 CP 拒绝。示例程序输出 `GPGPU DRM ABI=6 GPUVM=on|off`。

## 1. 新增 dcache 寄存器组

CP 用户数据窗口与 RV64 计算执行器的 load/store 在页属性为 WB 时经过一份设备级数据缓存：写回加写分配，VRAM 页和 Host 页都可以缓存。属性来自页表叶子项，GPUVM 关闭时数据访问都不进入缓存，见第 2 节。BAR0 新增 `0x0c00` 组：

```text
0x0c00 DCACHE_HITS         只读，命中次数
0x0c04 DCACHE_MISSES       只读，未命中次数
0x0c08 DCACHE_EVICTIONS    只读，替换有效旧行的次数
0x0c0c DCACHE_WRITEBACKS   只读，成功写回脏行的次数
0x0c10 DCACHE_ADDR_LO      命令起始 GPU 物理地址低 32 位
0x0c14 DCACHE_ADDR_HI      高 32 位
0x0c18 DCACHE_LENGTH       命令字节数
0x0c1c DCACHE_CMD          只写，写入即同步执行
0x0c20 DCACHE_STATUS       只读，本方最近一次命令的结果
```

`DCACHE_CMD` 的位为 `GPGPU_DCACHE_CLEAN`、`INVALIDATE`、`ALL`，结果为 `enum GpgpuDcacheStatus` 的 `OK`、`INVALID`、`DIRTY`、`STORAGE`、`BUSY`。几何参数 `GPGPU_DCACHE_SIZE`、`LINE_SIZE`、`WAYS`、`SETS` 也定义在 `gpgpu_regs.h`。

CP 的本地寄存器窗口（`gpgpu_cp_map.h` 中的 `0xfffd_0000`）新增同一组偏移。Host 和 CP 各有一份 ADDR、LENGTH 与 STATUS，统计共用，两方可用的命令相同。

`ERROR_STATUS` 新增位 5 `GPGPU_ERR_DCACHE_FAULT`：回写脏行失败时锁存，该行保持有效且脏。驱动在错误中断中记录日志并写 1 清除这一位。`GLOBAL_CTRL.RESET` 在复位各模块之前回写全部脏行，回写失败时复位失败；设备冷复位回写失败时丢弃剩余的行，继续复位。

## 2. 页表缓存属性与对象标志

`gpgpu_vm_abi.h` 新增叶子项位 2 `GPGPU_VM_UNCACHED`。置位的页按 DEVICE 访问，不进入 dcache；清零的页按 WB 缓存。它只能出现在叶子项，目录项带它报页表错误；`MAP.permissions` 仍只接受 R/W/X。GPUVM 关闭时没有页表，数据访问都按 DEVICE 处理。

`struct gpgpu_bo_create` 的 `flags` 新增 `GPGPU_BO_UNCACHED`（位 1）。带这个标志的对象整体不可缓存，属性在创建后不变：

```text
BO_CREATE(flags = GPGPU_BO_UNCACHED)
  → 驱动记下对象的缓存属性
  → MAP（GPUVM 开启时）：对象的每个叶子项都置 GPGPU_VM_UNCACHED
```

带 `GPGPU_BO_EVENT_PAGE` 的通知页总是不可缓存，不必再加 `GPGPU_BO_UNCACHED`。`QUEUE_CREATE` 新增一项检查：Ring 与进度对象都必须不可缓存，否则返回 `-EINVAL`。Signal 由用户库放进不可缓存对象，驱动不检查。

驱动内部的 KIQ、HIQ Ring 与进度页、各队列 MQD、管理结果区和 IH Ring 由 CP 管理域和 IH 访问，这两条路径总按 DEVICE 处理，不需要设置属性。

## 3. CACHE_SYNC 与用户库接口

新增 ioctl `GPGPU_DRM_CACHE_SYNC`（私有编号 `0x0d`），参数为 32 字节的 `struct gpgpu_cache_sync`：

```text
0x00 u32 version     GPGPU_DRM_VERSION
0x04 u32 handle      本文件的 GEM handle
0x08 u64 offset      对象内字节偏移
0x10 u64 size        字节数，非零，不超过对象请求的长度
0x18 u32 op          GPGPU_CACHE_CLEAN、GPGPU_CACHE_INVALIDATE 或两者之和
0x1c u32 reserved    必须为 0
```

驱动把对象内的范围换算成 GPU 物理地址发给设备，用户态不能维护任意物理范围。不可缓存对象同样按范围处理，范围内没有缓存行时直接成功；只失效时遇到脏行返回 `-EBUSY`，回写失败返回 `-EIO`。

用户库的接口随之变化：

- `gpgpuKmtAllocMemory()` 在 `kind` 之后增加 `uint32_t flags`，取 0 或 `GPGPU_BO_UNCACHED`；
- 新增 `gpgpuKmtSyncMemory(memory, offset, size, op)`，封装 `CACHE_SYNC`。CPU 直接改写可缓存对象后、发布任务前，用 `GPGPU_CACHE_INVALIDATE` 失效设备中的旧副本。`gpgpuKmtCopyMemory()` 由设备维护 DMA 两端，不需要另外调用。

运行时把 Ring、进度页和 Signal 建为不可缓存对象，参数块和暂存区可以缓存。`gpgpu_rt_dispatch()` 写完参数块后先失效它，再创建事件和发布包。`gpgpu_rt_*` 的函数签名不变。

## 4. 固件收尾与队列错误

执行器给出终态后，CP 固件先对整个 dcache 发 `CLEAN | ALL` 并确认 `STATUS = OK`，再发布 Signal、通知槽、IH 记录、Header 与消费进度。执行失败时也先回写，再记录队列错误。

`GPGPU_QUEUE_ERROR_EXECUTOR` 的含义扩大为“执行器拒绝派发、执行失败，或收尾回写失败”。错误码数值和记录格式不变。

## 5. 设备内部的旁路维护

以下行为不改变接口，但改变了 Host 能观察到的时序：

- BAR2 读写之前回写并失效重叠的行。维护失败时读返回 0，写被丢弃。
- DMA 开始搬运前两端都回写并失效重叠的行，失败时 `DMA_STATUS` 置 ERROR，不搬运。
- 关闭 Outbound 前回写并失效窗口内的全部行，失败时 `OUTBOUND_STATUS` 为 ERROR，窗口保持打开。
- 驱动归还 Host DMA 内存或 VRAM 块之前，回写并失效整段范围。维护失败，或设备已移除但还没确认停止时，存储留到设备释放时才归还。

## 6. 未改变的部分

- MQD、管理命令与结果区、AQL 包、Signal、IH 记录、Kernel 描述对象的布局与字段偏移。
- `0x0c00` 组以外的 BAR0 寄存器偏移，BAR2 与 BAR4；CP 私有地址空间中各窗口的位置。
- 除新增 `CACHE_SYNC` 外，DRM ioctl 编号与参数结构体布局。
- GPUVA 位宽、四级页表、TLB 组织与失效寄存器；页表项除叶子项位 2 外编码不变。
- VRAM 默认 64 MiB、Outbound 固定为 `0x8000_0000` 起的 2 GiB、31 位 DMA mask。
