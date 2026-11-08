/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dcache：CP 用户数据窗口与 RV64 load/store 共用的数据缓存
 *
 * 用户访问先经 GPUVM 完成翻译和本次权限检查，只有数据读写进入本模块；
 * 取指、页表遍历、CP 管理域、IH 与 DMA 仍直接访问后备存储：
 *
 *   gpgpu_user_rw()
 *     ├─ READ/WRITE ─▶ gpgpu_dcache_rw()
 *     │                  ├─ 可缓存行：组相联查找，未命中时回写、回填
 *     │                  └─ 不可缓存行 ─▶ gpgpu_phys_rw()
 *     └─ EXEC ─▶ gpgpu_phys_rw()
 *
 *   BAR2、DMA、维护命令、复位
 *     └─▶ gpgpu_dcache_maintain() ─回写─▶ gpgpu_phys_rw()
 *
 * 缓存行以 GPU 物理地址为标签，与 GPUVA、VMID 无关；回填和回写只调用
 * gpgpu_phys_rw()，不会再次进入本模块。几何参数与寄存器见 gpgpu_regs.h。
 */
#ifndef HW_GPGPU_MEM_DCACHE_H
#define HW_GPGPU_MEM_DCACHE_H

#include "hw/gpgpu/gpgpu.h"
#include "exec/hwaddr.h"
#include "exec/memattrs.h"

/**
 * struct GPGPUDcacheRequest - Host 或 CP 一方的维护命令
 * @addr: ADDR_LO/HI，命令起始 GPU 物理地址。
 * @length: LENGTH，命令字节数。
 * @status: 本方最近一次命令的 GpgpuDcacheStatus，复位为 GPGPU_DCACHE_OK。
 */
typedef struct GPGPUDcacheRequest {
	uint64_t addr;
	uint32_t length;
	uint32_t status;
} GPGPUDcacheRequest;

/**
 * struct GPGPUDcacheRange - 一个不可缓存的 GPU 物理区间
 * @base: 起点，按行对齐。
 * @size: 字节数，按行对齐；0 表示空表项。
 */
typedef struct GPGPUDcacheRange {
	uint64_t base;
	uint64_t size;
} GPGPUDcacheRange;

/**
 * struct GPGPUDcacheState - dcache 模块状态
 * @data: 数据阵列，下标依次为组号、路号和行内偏移。
 * @meta: 每行的 Tag、VALID 与 DIRTY，位段见 dcache.c。
 * @order: 每组的路号从最近使用（MRU）到最久未用（LRU）排列，
 *         复位为 {0, 1, 2, 3}。
 * @hits: 可缓存数据请求按行查找的命中次数，32 位回绕。
 * @misses: 未命中次数，同上。
 * @evictions: 用新行替换有效旧行的次数，同上。
 * @writebacks: 成功写回脏行的次数，同上。
 * @host: Host 经 BAR0 发出的命令。
 * @cp: CP 经本地寄存器窗口发出的命令。
 * @uncached: 不可缓存区间表，size 为 0 的表项空闲。
 */
typedef struct GPGPUDcacheState {
	uint8_t data[GPGPU_DCACHE_SETS][GPGPU_DCACHE_WAYS][GPGPU_DCACHE_LINE_SIZE];
	uint32_t meta[GPGPU_DCACHE_SETS][GPGPU_DCACHE_WAYS];
	uint8_t order[GPGPU_DCACHE_SETS][GPGPU_DCACHE_WAYS];
	uint32_t hits;
	uint32_t misses;
	uint32_t evictions;
	uint32_t writebacks;
	GPGPUDcacheRequest host;
	GPGPUDcacheRequest cp;
	GPGPUDcacheRange uncached[GPGPU_DCACHE_UC_RANGES];
} GPGPUDcacheState;

/**
 * gpgpu_dcache_rw() - 按 GPU 物理地址读写数据
 * @s: 设备实例，调用者持有主锁。
 * @addr: 已完成翻译与权限检查的 GPU 物理地址。
 * @buf: 数据缓冲区。
 * @len: 字节数，可以跨行。
 * @is_write: 为 true 时写入。
 *
 * 逐行处理：不可缓存区间内的行直接读写后备存储，其余行组相联查找，
 * 未命中时先回写被替换的脏行，再回填整行。失败时保留已完成的行。
 *
 * Return: MEMTX_OK，或范围不可路由、回填与回写失败时的错误。
 */
MemTxResult gpgpu_dcache_rw(GPGPUState *s, uint64_t addr, void *buf, size_t len, bool is_write);

/**
 * gpgpu_dcache_maintain() - 回写或失效与一段 GPU 物理地址重叠的行
 * @s: 设备实例，调用者持有主锁。
 * @addr: 起始 GPU 物理地址；@op 含 GPGPU_DCACHE_ALL 时忽略。
 * @len: 字节数，非零，范围低于 4 GiB；@op 含 GPGPU_DCACHE_ALL 时忽略。
 * @op: GPGPU_DCACHE_CLEAN、GPGPU_DCACHE_INVALIDATE 或两者之和，可加
 *      GPGPU_DCACHE_ALL。
 *
 * 直接访问后备存储的一方在读之前回写，在写之前回写并失效。维护
 * 不改变替换顺序，也不计命中与未命中。遇到第一个失败的行即停止，
 * 之前的行保持已维护的状态；回写失败时锁存 GPGPU_ERR_DCACHE_FAULT。
 *
 * Return: GPGPU_DCACHE_OK、GPGPU_DCACHE_DIRTY 或 GPGPU_DCACHE_STORAGE。
 */
uint32_t gpgpu_dcache_maintain(GPGPUState *s, uint64_t addr, uint64_t len, uint32_t op);

/* dcache 寄存器组（BAR0 0xc00）的 Host 视图；只有 Host 能登记区间。 */
uint32_t gpgpu_dcache_read(GPGPUState *s, hwaddr reg);
void gpgpu_dcache_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* CP 本地寄存器窗口中的同一组寄存器；ADDR、LENGTH 与 STATUS 独立于 Host。 */
uint32_t gpgpu_dcache_cp_read(GPGPUState *s, hwaddr reg);
void gpgpu_dcache_cp_write(GPGPUState *s, hwaddr reg, uint32_t value);

/* 丢弃全部行，清空区间表、命令与统计；调用者已先回写脏行。 */
void gpgpu_dcache_reset(GPGPUState *s);

#endif /* HW_GPGPU_MEM_DCACHE_H */
