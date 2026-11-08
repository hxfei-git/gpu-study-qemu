// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 访存入口：检查请求，按访问类型分派物理事务并处理旁路访问的缓存副本
 */
#include "qemu/osdep.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "access.h"
#include "backend.h"

/* DEVICE 访问之前对重叠行执行的维护：先回写脏行，再失效。 */
#define MEM_FLUSH (GPGPU_DCACHE_CLEAN | GPGPU_DCACHE_INVALIDATE)

/*
 * 各来源的约束，下标为 enum GPGPUMemClient：ops 是允许的操作位图，位号为
 * enum GPGPUMemOp；cacheable 表示能否使用 WB，只有经 GPUVM 的用户访问可以。
 */
static const struct {
	uint8_t ops;
	bool cacheable;
} mem_clients[] = {
	[GPGPU_MEM_CP_USER] = { BIT(GPGPU_MEM_READ) | BIT(GPGPU_MEM_WRITE), true },
	[GPGPU_MEM_EXECUTOR] = {
		BIT(GPGPU_MEM_READ) | BIT(GPGPU_MEM_WRITE) | BIT(GPGPU_MEM_FETCH), true },
	[GPGPU_MEM_CP_MGMT] = { BIT(GPGPU_MEM_READ) | BIT(GPGPU_MEM_WRITE), false },
	[GPGPU_MEM_PTW] = { BIT(GPGPU_MEM_READ), false },
	[GPGPU_MEM_IH] = { BIT(GPGPU_MEM_READ) | BIT(GPGPU_MEM_WRITE), false },
	[GPGPU_MEM_BAR] = { BIT(GPGPU_MEM_READ) | BIT(GPGPU_MEM_WRITE), false },
	[GPGPU_MEM_DMA] = { BIT(GPGPU_MEM_COPY), false },
};

/*
 * 来源允许这项操作；类型已填写，WB 只给可以缓存的来源；长度非零且
 * 范围可路由，COPY 的目的端同样可路由。
 */
static bool mem_request_valid(GPGPUState *s, const GPGPUMemRequest *req)
{
	if (req->client >= ARRAY_SIZE(mem_clients) || !(mem_clients[req->client].ops & BIT(req->op)))
		return false;
	if (req->type != GPGPU_MEM_DEVICE &&
	    (req->type != GPGPU_MEM_WB || !mem_clients[req->client].cacheable))
		return false;
	if (!gpgpu_phys_contains(s, req->addr, req->len))
		return false;
	return req->op != GPGPU_MEM_COPY || gpgpu_phys_contains(s, req->dst, req->len);
}

/* DEVICE 访问之前回写并失效重叠的行：读到最新数据，写入后不留旧副本。 */
static bool mem_device_prepare(GPGPUState *s, uint64_t addr, uint64_t len)
{
	return gpgpu_dcache_maintain(s, addr, len, MEM_FLUSH, NULL);
}

/*
 * mem_copy() - 在 VRAM 与 Host 内存之间搬运一段数据
 *
 * DMA 引擎写 DMA_CTRL 时经 gpgpu_mem_access() 调用。两端都直接读写后备
 * 存储，又都可能在 dcache 中有副本，所以先处理两端，再整段搬运：
 *
 *   VRAM → Host：源 [VRAM 偏移] 回写并失效 ─┐
 *                目的 [Outbound] 回写并失效 ─┴─▶ 映射 VRAM，写 Host
 *
 * 例：src = 0x0004_0000、dst = 0xc100_0000、len = 0x1000，VRAM 端是源，
 * 方向为 VRAM 到 Host。
 *
 * 1. 一端完整位于 VRAM、另一端位于 Outbound 窗口，否则失败；
 * 2. 两端都按 DEVICE 处理：读端拿到缓存中的新数据，写端搬运之后不会再被
 *    旧脏行覆盖。任一端维护失败时不搬运；
 * 3. 把 VRAM 端映射成宿主机指针，与 Host 端整段互拷，避免逐行经 socket
 *    往返。Host 访问失败时已写入的部分保留。
 *
 * 函数不检查 DMA 描述符的其他约束，也不发布完成状态，由 dma.c 负责。
 *
 * Return: 搬运完成时为 true。
 */
static bool mem_copy(GPGPUState *s, const GPGPUMemRequest *req)
{
	bool to_host = gpgpu_vram_contains(s, req->addr, req->len);
	uint64_t vram_addr = to_host ? req->addr : req->dst;
	uint64_t host_addr = to_host ? req->dst : req->addr;
	MemTxResult result;
	void *vram;

	/* 1. 只支持 VRAM 与 Outbound 之间的搬运。 */
	if (!gpgpu_vram_contains(s, vram_addr, req->len) || host_addr < GPGPU_OUTBOUND_BASE)
		return false;

	/* 2. 两端都处理完缓存副本才开始搬运。 */
	if (!mem_device_prepare(s, req->addr, req->len) ||
	    !mem_device_prepare(s, req->dst, req->len))
		return false;

	/* 3. VRAM 端 D2H 只读、H2D 只写，Host 端方向相反。 */
	vram = gpgpu_vram_map(s, vram_addr, req->len, !to_host);
	result = gpgpu_phys_rw(s, host_addr, vram, req->len, to_host);
	gpgpu_vram_unmap(s, vram, req->len, !to_host);
	return result == MEMTX_OK;
}

/* 读写数据：WB 经 dcache；DEVICE 先处理重叠行，再直接读写后备存储。 */
static bool mem_data_rw(GPGPUState *s, const GPGPUMemRequest *req)
{
	bool is_write = req->op == GPGPU_MEM_WRITE;

	if (req->type == GPGPU_MEM_WB)
		return gpgpu_dcache_rw(s, req->addr, req->buf, req->len, is_write) == MEMTX_OK;
	return mem_device_prepare(s, req->addr, req->len) &&
		gpgpu_phys_rw(s, req->addr, req->buf, req->len, is_write) == MEMTX_OK;
}

bool gpgpu_mem_access(GPGPUState *s, const GPGPUMemRequest *req)
{
	bool ok;

	if (!mem_request_valid(s, req))
		return false;

	/* 整次事务计入在途访问，维护与后备访问之间不会插入复位或重配。 */
	gpgpu_phys_begin(s);
	switch (req->op) {
	case GPGPU_MEM_COPY:
		ok = mem_copy(s, req);
		break;
	case GPGPU_MEM_FETCH:
		/* 取指不查 dcache，也不分配数据行；代码发布由软件先回写。 */
		ok = gpgpu_phys_rw(s, req->addr, req->buf, req->len, false) == MEMTX_OK;
		break;
	default:
		ok = mem_data_rw(s, req);
		break;
	}
	gpgpu_phys_end(s);
	return ok;
}

bool gpgpu_mem_sync(GPGPUState *s, uint64_t addr, uint64_t len, uint32_t op, uint32_t *status)
{
	uint32_t result = GPGPU_DCACHE_BUSY;
	bool ok = false;

	/* 已有在途事务时不改动缓存，结果为 BUSY。 */
	if (!gpgpu_phys_busy(s)) {
		gpgpu_phys_begin(s);
		ok = gpgpu_dcache_maintain(s, addr, len, op, &result);
		gpgpu_phys_end(s);
	}
	if (status)
		*status = result;
	return ok;
}

void gpgpu_mem_reset(GPGPUState *s)
{
	gpgpu_outbound_reset(s);
	gpgpu_dcache_reset(s);
}
