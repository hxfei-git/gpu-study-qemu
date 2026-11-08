// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dcache：组相联查找、写回与写分配、维护命令与不可缓存区间
 */
#include "qemu/osdep.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "dcache.h"
#include "phys.h"
#include "vram.h"

/* 行内偏移位数，即 log2(行大小)。 */
#define DCACHE_OFFSET_BITS __builtin_ctz(GPGPU_DCACHE_LINE_SIZE)
/* 组号位数，即 log2(组数)。 */
#define DCACHE_INDEX_BITS __builtin_ctz(GPGPU_DCACHE_SETS)
/* Tag 位数：物理地址位数减去组号与行内偏移，32 位地址时为 19。 */
#define DCACHE_TAG_BITS (GPGPU_PHYS_ADDRESS_BITS - DCACHE_INDEX_BITS - DCACHE_OFFSET_BITS)
/* meta 低位存 Tag，其上依次是 VALID 与 DIRTY。 */
#define DCACHE_TAG_MASK ((1u << DCACHE_TAG_BITS) - 1)
/* meta：行有效，数据可以读取。 */
#define DCACHE_VALID (1u << DCACHE_TAG_BITS)
/* meta：行已被写入，尚未回写到后备存储。 */
#define DCACHE_DIRTY (DCACHE_VALID << 1)
/* 缓存行总数；维护范围不少于它时改为扫描整份缓存。 */
#define DCACHE_LINES (GPGPU_DCACHE_SETS * GPGPU_DCACHE_WAYS)
/* 维护命令中回写与失效两个操作位。 */
#define DCACHE_OPS (GPGPU_DCACHE_CLEAN | GPGPU_DCACHE_INVALIDATE)

QEMU_BUILD_BUG_ON(GPGPU_DCACHE_LINE_SIZE & (GPGPU_DCACHE_LINE_SIZE - 1));
QEMU_BUILD_BUG_ON(GPGPU_DCACHE_SETS & (GPGPU_DCACHE_SETS - 1));
QEMU_BUILD_BUG_ON(DCACHE_LINES * GPGPU_DCACHE_LINE_SIZE != GPGPU_DCACHE_SIZE);
QEMU_BUILD_BUG_ON(DCACHE_TAG_BITS + 2 > 32);

/* 地址所在行的组号。 */
static unsigned int dcache_set_of(uint64_t addr)
{
	return (addr >> DCACHE_OFFSET_BITS) & (GPGPU_DCACHE_SETS - 1);
}

/* 地址所在行的 Tag，即去掉组号与行内偏移后剩下的高位。 */
static uint32_t dcache_tag_of(uint64_t addr)
{
	return addr >> (DCACHE_OFFSET_BITS + DCACHE_INDEX_BITS);
}

/* 由 meta 的 Tag 与组号重建行起点；替换和维护按它回写。 */
static uint64_t dcache_line_addr(uint32_t meta, unsigned int set)
{
	return (uint64_t)(meta & DCACHE_TAG_MASK) << (DCACHE_OFFSET_BITS + DCACHE_INDEX_BITS) |
		(uint64_t)set << DCACHE_OFFSET_BITS;
}

/* 组内查找有效且 Tag 相同的路；未找到返回 -1。 */
static int dcache_lookup(const GPGPUDcacheState *c, unsigned int set, uint32_t tag)
{
	for (unsigned int way = 0; way < GPGPU_DCACHE_WAYS; way++) {
		uint32_t meta = c->meta[set][way];

		if ((meta & DCACHE_VALID) && (meta & DCACHE_TAG_MASK) == tag)
			return way;
	}
	return -1;
}

/* 把 way 移到 order[0] 成为 MRU，排在它前面的路依次后移一位。 */
static void dcache_touch(uint8_t *order, unsigned int way)
{
	unsigned int pos = 0;

	while (pos < GPGPU_DCACHE_WAYS - 1 && order[pos] != way)
		pos++;
	memmove(&order[1], &order[0], pos);
	order[0] = way;
}

/* 行起点是否落在不可缓存区间内；区间按行对齐，整行属性相同。 */
static bool dcache_line_uncached(const GPGPUDcacheState *c, uint64_t line)
{
	for (unsigned int i = 0; i < GPGPU_DCACHE_UC_RANGES; i++) {
		const GPGPUDcacheRange *r = &c->uncached[i];

		if (r->size && line - r->base < r->size)
			return true;
	}
	return false;
}

/* 回写一条有效的脏行；成功后清除 DIRTY 并计数，失败时该行保持不变。 */
static bool dcache_writeback(GPGPUState *s, unsigned int set, unsigned int way)
{
	GPGPUDcacheState *c = &s->dcache;
	uint32_t *meta = &c->meta[set][way];

	if (gpgpu_phys_rw(s, dcache_line_addr(*meta, set), c->data[set][way],
			  GPGPU_DCACHE_LINE_SIZE, true) != MEMTX_OK)
		return false;
	*meta &= ~DCACHE_DIRTY;
	c->writebacks++;
	return true;
}

/*
 * dcache_fill() - 为 line 所在行分配一路并回填
 *
 * 未命中时由 dcache_line_rw() 调用。先选出要用的路，再分两步处理旧行和
 * 新行，任何一步失败都不会留下“新 Tag + 旧数据”的半成品行：
 *
 * 1. 优先选编号最小的无效路；四路都有效时选 order 末尾的 LRU 路；
 * 2. 旧行有效且脏时，按旧 Tag 与组号回写整行。失败时旧行保持有效
 *    且脏，返回失败；
 * 3. 把新行读入临时缓冲。失败时旧行的 Tag 与数据不变，第 2 步已经成功
 *    的回写也不回退；
 * 4. 读到完整的新行后才提交数据与 Tag，新行是干净的。替换了有效旧行时
 *    计一次 EVICTIONS。
 *
 * 函数不更新替换顺序，由调用者读写完成后把这一路置为 MRU。
 *
 * Return: 回填后的路号；失败为 -1。
 */
static int dcache_fill(GPGPUState *s, unsigned int set, uint64_t line)
{
	GPGPUDcacheState *c = &s->dcache;
	unsigned int way = c->order[set][GPGPU_DCACHE_WAYS - 1];
	uint8_t buf[GPGPU_DCACHE_LINE_SIZE];
	bool replace;

	/* 1. 无效路优先，否则替换 LRU 路。 */
	for (unsigned int i = 0; i < GPGPU_DCACHE_WAYS; i++) {
		if (!(c->meta[set][i] & DCACHE_VALID)) {
			way = i;
			break;
		}
	}
	replace = c->meta[set][way] & DCACHE_VALID;

	/* 2. 先腾出旧行；回写失败时它保持有效且脏。 */
	if ((c->meta[set][way] & DCACHE_DIRTY) && !dcache_writeback(s, set, way))
		return -1;

	/* 3. 新行先读到临时缓冲，读不到时不改动旧行。 */
	if (gpgpu_phys_rw(s, line, buf, sizeof(buf), false) != MEMTX_OK)
		return -1;

	/* 4. 整行到齐后再提交。 */
	memcpy(c->data[set][way], buf, sizeof(buf));
	c->meta[set][way] = dcache_tag_of(line) | DCACHE_VALID;
	if (replace)
		c->evictions++;
	return way;
}

/*
 * 在一条可缓存的行内读写 [addr, addr + len)，调用者保证不跨行。命中或回填
 * 成功后读写数据，写入置 DIRTY，最后把这一路置为 MRU。
 */
static MemTxResult dcache_line_rw(GPGPUState *s, uint64_t addr, uint8_t *buf, size_t len,
				  bool is_write)
{
	GPGPUDcacheState *c = &s->dcache;
	unsigned int set = dcache_set_of(addr);
	unsigned int offset = addr & (GPGPU_DCACHE_LINE_SIZE - 1);
	int way = dcache_lookup(c, set, dcache_tag_of(addr));

	if (way >= 0) {
		c->hits++;
	} else {
		c->misses++;
		way = dcache_fill(s, set, addr - offset);
		if (way < 0)
			return MEMTX_ERROR;
	}

	if (is_write) {
		memcpy(&c->data[set][way][offset], buf, len);
		c->meta[set][way] |= DCACHE_DIRTY;
	} else {
		memcpy(buf, &c->data[set][way][offset], len);
	}
	dcache_touch(c->order[set], way);
	return MEMTX_OK;
}

MemTxResult gpgpu_dcache_rw(GPGPUState *s, uint64_t addr, void *buf, size_t len, bool is_write)
{
	uint8_t *p = buf;

	/* 范围不可路由时直接失败，不计命中与未命中。 */
	if (!gpgpu_phys_contains(s, addr, len))
		return MEMTX_ERROR;

	while (len) {
		uint64_t line = addr & ~(uint64_t)(GPGPU_DCACHE_LINE_SIZE - 1);
		size_t chunk = MIN(len, line + GPGPU_DCACHE_LINE_SIZE - addr);
		MemTxResult result;

		if (dcache_line_uncached(&s->dcache, line))
			result = gpgpu_phys_rw(s, addr, p, chunk, is_write);
		else
			result = dcache_line_rw(s, addr, p, chunk, is_write);
		if (result != MEMTX_OK)
			return result;
		addr += chunk;
		p += chunk;
		len -= chunk;
	}
	return MEMTX_OK;
}

/*
 * 对一路执行回写或失效。无效行直接跳过；脏行要失效时必须先回写，
 * 只失效不回写时返回 DIRTY，该行保持不变。
 */
static uint32_t dcache_maintain_way(GPGPUState *s, unsigned int set, unsigned int way, uint32_t op)
{
	uint32_t *meta = &s->dcache.meta[set][way];

	if (!(*meta & DCACHE_VALID))
		return GPGPU_DCACHE_OK;
	if (*meta & DCACHE_DIRTY) {
		if (!(op & GPGPU_DCACHE_CLEAN))
			return GPGPU_DCACHE_DIRTY;
		if (!dcache_writeback(s, set, way))
			return GPGPU_DCACHE_STORAGE;
	}
	if (op & GPGPU_DCACHE_INVALIDATE)
		*meta = 0;
	return GPGPU_DCACHE_OK;
}

/*
 * gpgpu_dcache_maintain() - 按范围或全部行执行回写与失效
 *
 * BAR 的单次写入只涉及一两行，DMA 和复位可能覆盖整份缓存，所以按范围
 * 大小选择查找方式。范围用行号表示：行号 = 物理地址 >> 6。
 *
 * 例：[0x0123_4568, +8) 只覆盖行号 0x4_8d15，即行 0x0123_4540，只需查组
 * 0x15 中 Tag 为 0x91a 的那一路。
 *
 * 1. 带 ALL，或范围覆盖的行数不少于缓存行总数时，逐路扫描整份缓存
 *    一遍，只处理行号落在范围内的有效行；
 * 2. 否则逐个行号计算组号与 Tag，只查可能缓存这一行的那一组；
 * 3. 任一行失败即停止，返回原因；回写失败另锁存 DCACHE_FAULT。
 *
 * Return: GPGPU_DCACHE_OK、GPGPU_DCACHE_DIRTY 或 GPGPU_DCACHE_STORAGE。
 */
uint32_t gpgpu_dcache_maintain(GPGPUState *s, uint64_t addr, uint64_t len, uint32_t op)
{
	GPGPUDcacheState *c = &s->dcache;
	bool all = op & GPGPU_DCACHE_ALL;
	uint64_t first = all ? 0 : addr >> DCACHE_OFFSET_BITS;
	uint64_t last = all ? 0 : (addr + len - 1) >> DCACHE_OFFSET_BITS;
	uint32_t status = GPGPU_DCACHE_OK;

	/* 1. 整份扫描：每路按自己的 Tag 与组号判断是否在范围内。 */
	if (all || last - first + 1 >= DCACHE_LINES) {
		for (unsigned int set = 0; set < GPGPU_DCACHE_SETS && !status; set++) {
			for (unsigned int way = 0; way < GPGPU_DCACHE_WAYS && !status; way++) {
				uint64_t index = dcache_line_addr(c->meta[set][way], set) >>
						 DCACHE_OFFSET_BITS;

				if (all || (index >= first && index <= last))
					status = dcache_maintain_way(s, set, way, op);
			}
		}
	} else {
		/* 2. 小范围：逐行定位到唯一可能的组与路。 */
		for (uint64_t index = first; index <= last && !status; index++) {
			uint64_t line = index << DCACHE_OFFSET_BITS;
			unsigned int set = dcache_set_of(line);
			int way = dcache_lookup(c, set, dcache_tag_of(line));

			if (way >= 0)
				status = dcache_maintain_way(s, set, way, op);
		}
	}

	/* 3. 脏数据仍留在缓存中，只有回写失败才算存储故障。 */
	if (status == GPGPU_DCACHE_STORAGE)
		gpgpu_set_error(s, GPGPU_ERR_DCACHE_FAULT);
	return status;
}

/* 区间完整位于 VRAM 或 Outbound 窗口的固定布局内；不要求窗口已经打开。 */
static bool dcache_range_routable(GPGPUState *s, uint64_t base, uint64_t size)
{
	if (gpgpu_vram_contains(s, base, size))
		return true;
	return base >= GPGPU_OUTBOUND_BASE &&
		gpgpu_range_ok(base - GPGPU_OUTBOUND_BASE, size, GPGPU_OUTBOUND_SIZE);
}

/*
 * dcache_uc_add() - 登记一个不可缓存区间
 *
 * 驱动在发布 Ring、进度、Signal 等控制对象前登记。登记后这段物理地址
 * 的数据访问都直接读写后备存储，任何用户地址别名都不会再产生缓存
 * 副本：
 *
 * 1. 起点与长度按行对齐、长度非零、位于 VRAM 或 Outbound 布局内，且与
 *    已登记的区间不重叠，否则返回 INVALID；
 * 2. 找一个空表项，表满返回 FULL；
 * 3. 回写并失效区间内已经缓存的行；回写失败不登记，返回 STORAGE；
 * 4. 写入表项。
 *
 * Return: GpgpuDcacheStatus。
 */
static uint32_t dcache_uc_add(GPGPUState *s, uint64_t base, uint64_t size)
{
	GPGPUDcacheState *c = &s->dcache;
	GPGPUDcacheRange *slot = NULL;
	uint32_t status;

	/* 1. 对齐、范围与重叠。 */
	if (!size || ((base | size) & (GPGPU_DCACHE_LINE_SIZE - 1)) ||
	    !dcache_range_routable(s, base, size))
		return GPGPU_DCACHE_INVALID;
	for (unsigned int i = 0; i < GPGPU_DCACHE_UC_RANGES; i++) {
		GPGPUDcacheRange *r = &c->uncached[i];

		if (!r->size) {
			slot = slot ? slot : r;
			continue;
		}
		if (base < r->base + r->size && r->base < base + size)
			return GPGPU_DCACHE_INVALID;
	}

	/* 2. 表满时不做任何维护。 */
	if (!slot)
		return GPGPU_DCACHE_FULL;

	/* 3. 清掉区间内已有的缓存行。 */
	status = gpgpu_dcache_maintain(s, base, size, DCACHE_OPS);
	if (status)
		return status;

	/* 4. 登记。 */
	*slot = (GPGPUDcacheRange) {
		.base = base,
		.size = size,
	};
	return GPGPU_DCACHE_OK;
}

/* 撤销与 [base, base + size) 完全相同的区间；其中没有缓存行，无需维护。 */
static uint32_t dcache_uc_remove(GPGPUState *s, uint64_t base, uint64_t size)
{
	for (unsigned int i = 0; i < GPGPU_DCACHE_UC_RANGES; i++) {
		GPGPUDcacheRange *r = &s->dcache.uncached[i];

		if (r->size && r->base == base && r->size == size) {
			memset(r, 0, sizeof(*r));
			return GPGPU_DCACHE_OK;
		}
	}
	return GPGPU_DCACHE_INVALID;
}

/*
 * dcache_command() - 执行一方写入 CMD 的命令，结果写回该方的 STATUS
 *
 * host 为 false 时命令来自 CP，只接受维护操作：
 *
 * 1. 仍有在途物理访问时整体拒绝，返回 BUSY；
 * 2. UC_ADD、UC_REMOVE 只接受 Host，且不能与其他位同时出现；
 * 3. 维护操作至少带 CLEAN 或 INVALIDATE，只能另加 ALL；不带 ALL 时范围
 *    非空且低于 4 GiB。
 */
static void dcache_command(GPGPUState *s, GPGPUDcacheRequest *req, uint32_t cmd, bool host)
{
	/* 1. 防御检查：在途访问期间不改动缓存。 */
	if (gpgpu_phys_busy(s)) {
		req->status = GPGPU_DCACHE_BUSY;
		return;
	}

	/* 2. 区间登记与撤销。 */
	if (host && cmd == GPGPU_DCACHE_UC_ADD) {
		req->status = dcache_uc_add(s, req->addr, req->length);
		return;
	}
	if (host && cmd == GPGPU_DCACHE_UC_REMOVE) {
		req->status = dcache_uc_remove(s, req->addr, req->length);
		return;
	}

	/* 3. 维护操作。 */
	if (!(cmd & DCACHE_OPS) || (cmd & ~(DCACHE_OPS | GPGPU_DCACHE_ALL)) ||
	    (!(cmd & GPGPU_DCACHE_ALL) &&
	     (!req->length || !gpgpu_range_ok(req->addr, req->length, GPGPU_PHYS_ADDRESS_LIMIT)))) {
		req->status = GPGPU_DCACHE_INVALID;
		return;
	}
	req->status = gpgpu_dcache_maintain(s, req->addr, req->length, cmd);
}

/* 两方共用的读取：统计全设备共享，ADDR、LENGTH、STATUS 取 req 的值。 */
static uint32_t dcache_reg_read(const GPGPUDcacheState *c, const GPGPUDcacheRequest *req,
				hwaddr reg)
{
	switch (reg) {
	case GPGPU_REG_DCACHE_HITS:
		return c->hits;
	case GPGPU_REG_DCACHE_MISSES:
		return c->misses;
	case GPGPU_REG_DCACHE_EVICTIONS:
		return c->evictions;
	case GPGPU_REG_DCACHE_WRITEBACKS:
		return c->writebacks;
	case GPGPU_REG_DCACHE_ADDR_LO:
	case GPGPU_REG_DCACHE_ADDR_HI:
		return gpgpu_reg64_read(req->addr, reg - GPGPU_REG_DCACHE_ADDR_LO);
	case GPGPU_REG_DCACHE_LENGTH:
		return req->length;
	case GPGPU_REG_DCACHE_STATUS:
		return req->status;
	default:
		return 0;
	}
}

/* 两方共用的写入：写只读寄存器或未定义偏移时锁存 INVALID_CMD。 */
static void dcache_reg_write(GPGPUState *s, GPGPUDcacheRequest *req, hwaddr reg, uint32_t value,
			     bool host)
{
	switch (reg) {
	case GPGPU_REG_DCACHE_ADDR_LO:
	case GPGPU_REG_DCACHE_ADDR_HI:
		gpgpu_reg64_write(&req->addr, reg - GPGPU_REG_DCACHE_ADDR_LO, value);
		break;
	case GPGPU_REG_DCACHE_LENGTH:
		req->length = value;
		break;
	case GPGPU_REG_DCACHE_CMD:
		dcache_command(s, req, value, host);
		break;
	default:
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		break;
	}
}

uint32_t gpgpu_dcache_read(GPGPUState *s, hwaddr reg)
{
	return dcache_reg_read(&s->dcache, &s->dcache.host, reg);
}

void gpgpu_dcache_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	dcache_reg_write(s, &s->dcache.host, reg, value, true);
}

uint32_t gpgpu_dcache_cp_read(GPGPUState *s, hwaddr reg)
{
	return dcache_reg_read(&s->dcache, &s->dcache.cp, reg);
}

void gpgpu_dcache_cp_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	dcache_reg_write(s, &s->dcache.cp, reg, value, false);
}

void gpgpu_dcache_reset(GPGPUState *s)
{
	GPGPUDcacheState *c = &s->dcache;

	memset(c, 0, sizeof(*c));
	for (unsigned int set = 0; set < GPGPU_DCACHE_SETS; set++)
		for (unsigned int way = 0; way < GPGPU_DCACHE_WAYS; way++)
			c->order[set][way] = way;
}
