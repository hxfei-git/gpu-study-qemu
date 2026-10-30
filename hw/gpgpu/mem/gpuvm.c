// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPUVM：用户地址翻译、TLB、上下文寄存器与故障记录
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/cp/hqd.h"
#include "gpuvm.h"
#include "phys.h"

/* GPUVA 中页内偏移的位数。 */
#define VM_PAGE_SHIFT 12
/* 页表级数：L0 由 VA[31:30] 索引 4 项，L1、L2 由 9 位索引 512 项。 */
#define VM_LEVELS 3
/* 每个页表项的字节数。 */
#define VM_PTE_SIZE 8
/* 每个用户上下文、每个故障组在 BAR0 中占用的字节数。 */
#define VM_CONTEXT_STRIDE (GPGPU_REG_VM_CONTEXT(1) - GPGPU_REG_VM_CONTEXT(0))
#define VM_FAULT_STRIDE (GPGPU_REG_VM_FAULT(1) - GPGPU_REG_VM_FAULT(0))

/* 每级索引在 GPUVA 中的起始位与掩码，依次为 L0、L1、L2。 */
static const unsigned int vm_level_shift[VM_LEVELS] = { 30, 21, 12 };
static const unsigned int vm_level_mask[VM_LEVELS] = { 3, 511, 511 };

void gpgpu_vm_record_fault(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr,
			   uint32_t access, uint32_t phase, uint32_t reason)
{
	if (slot >= GPGPU_HQD_SLOTS || s->vm.fault[slot].reason)
		return;
	s->vm.fault[slot] = (GPGPUVMFault) {
		.address = addr,
		.reason = reason,
		.vmid = vmid,
		.access = access,
		.phase = phase,
	};
}

/*
 * 标量（不超过 8 字节）要求长度为 2 的幂且自然对齐；
 * 访问类型必须是 READ、WRITE、EXEC 之一。
 */
static bool user_request_valid(uint64_t addr, size_t len, uint32_t access)
{
	if (!len || (len <= 8 && (!is_power_of_2(len) || (addr & (len - 1)))))
		return false;
	return access == GPGPU_VM_READ || access == GPGPU_VM_WRITE || access == GPGPU_VM_EXEC;
}

/* 用户可见的物理范围：可路由，且不触及 VRAM 首页与固件保留区。 */
static bool user_phys_valid(GPGPUState *s, uint64_t addr, uint64_t len)
{
	const uint64_t fw_end = (uint64_t)GPGPU_FIRMWARE_BASE + GPGPU_FIRMWARE_SIZE;

	if (!gpgpu_phys_contains(s, addr, len))
		return false;
	if (addr >= gpgpu_vram_size(s))
		return true;
	if (addr < GPGPU_VM_PAGE_SIZE)
		return false;
	return addr >= fw_end || gpgpu_range_ok(addr, len, GPGPU_FIRMWARE_BASE);
}

/* 页表页与叶子目标页都必须是页对齐、用户可见的物理页。 */
static bool vm_page_valid(GPGPUState *s, uint64_t addr)
{
	return !(addr & (GPGPU_VM_PAGE_SIZE - 1)) && user_phys_valid(s, addr, GPGPU_VM_PAGE_SIZE);
}

/*
 * vm_walk() - 同步遍历 vmid 的三级页表，取得 va 所在页的叶子项
 *
 * TLB 未命中时由 vm_lookup() 调用。32 位 GPUVA 按下面的方式拆成三级索引
 * 和页内偏移，每级页表都占一个 4 KiB 物理页，表项为 8 字节小端值：
 *
 *   va 位    31..30     29..21     20..12     11..0
 *   用途     L0 索引    L1 索引    L2 索引    页内偏移（本函数不用）
 *   项数     4          512        512
 *
 * 表项格式：位 31..12 是下一级页表页（L0、L1）或目标数据页（L2）的
 * GPU 物理页基址；位 0 为 VALID；只有 L2 叶子项可以带 X(4)、R(5)、W(6)
 * 权限位，其余位必须为 0。
 *
 * 例：va = 0x8000_1234 依次取 L0[2]、L1[0]、L2[1]。L2[1] 给出数据页基址，
 * 调用者再加上页内偏移 0x234 得到最终物理地址。
 *
 * 从上下文寄存器 ROOT 指向的 L0 页开始，每级执行同样的步骤：
 *
 * 1. 用 va 的对应位段算出本级索引；
 * 2. 检查当前表页页对齐、可路由，且不落在 VRAM 首页或固件保留区，
 *    再按 GPU 物理地址读出 table + 索引 × 8 处的表项。表页本身不经过
 *    用户翻译，否则会递归查表。读不到记 TABLE 故障；
 * 3. VALID 为 0 说明这段地址没有映射，记 UNMAPPED 故障；
 * 4. 表项带有本级不允许的位（中间项带权限位，或任一级带保留位）
 *    说明页表内容损坏，记 TABLE 故障；
 * 5. 取出位 31..12 作为下一级表页。到 L2 时它是数据页，同样检查
 *    后把整个叶子项交给调用者。
 *
 * 函数只读页表，不检查本次访问的 R/W/X 权限，也不写 TLB，这两件事由
 * vm_translated_rw() 和 vm_lookup() 完成。
 *
 * Return: 0 表示成功并写入 *pte，否则为 GPGPU_VM_FAULT_TABLE 或
 * GPGPU_VM_FAULT_UNMAPPED。
 */
static uint32_t vm_walk(GPGPUState *s, uint32_t vmid, uint32_t va, uint64_t *pte)
{
	/* 从 L0 开始，table 始终保存当前这一级表页的 GPU 物理基址。 */
	uint64_t table = s->vm.contexts[vmid].root;

	for (unsigned int level = 0; level < VM_LEVELS; level++) {
		bool leaf = level == VM_LEVELS - 1;
		/* 1. L0 取 va[31:30]，L1 取 va[29:21]，L2 取 va[20:12]。 */
		uint32_t index = (va >> vm_level_shift[level]) & vm_level_mask[level];
		/* 本级允许的位：中间项为地址和 VALID，叶子项再加 R/W/X。 */
		uint64_t allowed = GPGPU_VM_ADDR_MASK | (leaf ? GPGPU_VM_FLAGS : GPGPU_VM_VALID);
		uint8_t raw[VM_PTE_SIZE];
		uint64_t entry;

		/* 2. 表页必须是用户可见的物理页，读出本级第 index 项。 */
		if (!vm_page_valid(s, table) ||
		    gpgpu_phys_rw(s, table + index * VM_PTE_SIZE, raw, sizeof(raw), false) != MEMTX_OK)
			return GPGPU_VM_FAULT_TABLE;
		entry = ldq_le_p(raw);

		/*
		 * 3. 本级没有建立映射：驱动尚未为这段 GPUVA
		 *    分配下一级页表页或数据页。
		 */
		if (!(entry & GPGPU_VM_VALID))
			return GPGPU_VM_FAULT_UNMAPPED;

		/*
		 * 4. 表项带有本级不允许的位：中间项带了 R/W/X，
		 *    或任一级带了地址以外的高位、保留位。驱动不会
		 *    写出这样的项，视为页表损坏。
		 */
		if (entry & ~allowed)
			return GPGPU_VM_FAULT_TABLE;

		/*
		 * 5. 中间级：取下一级表页，下一轮循环再检查它。
		 *    叶子级：取目标数据页，在这里检查后返回整个叶子项。
		 */
		table = entry & GPGPU_VM_ADDR_MASK;
		if (leaf) {
			if (!vm_page_valid(s, table))
				return GPGPU_VM_FAULT_TABLE;
			*pte = entry;
		}
	}
	return 0;
}

/*
 * vm_lookup() - 查 TLB，未命中时遍历页表并填回
 *
 * TLB 是 64 项直接映射，下标为 (页号 ^ vmid) % 64；项中保存 vmid、页号
 * 和叶子 PTE。pte 为 0 表示空项，合法叶子项至少带 VALID，不会为 0。
 * 只缓存成功的翻译，故障不进 TLB，下次访问会重新遍历页表。
 * Host 修改页表后必须写 INVALIDATE 清掉该 vmid 的全部项。
 *
 * Return: 0 并写入 *pte，或 vm_walk() 返回的故障原因。
 */
static uint32_t vm_lookup(GPGPUState *s, uint32_t vmid, uint32_t va, uint64_t *pte)
{
	uint32_t page = va >> VM_PAGE_SHIFT;
	GPGPUVMTlbEntry *entry = &s->vm.tlb[(page ^ vmid) % GPGPU_VM_TLB_ENTRIES];
	uint32_t reason;

	if (entry->pte && entry->vmid == vmid && entry->page == page) {
		*pte = entry->pte;
		return 0;
	}
	reason = vm_walk(s, vmid, va, pte);
	if (!reason)
		*entry = (GPGPUVMTlbEntry) { .pte = *pte, .page = page, .vmid = vmid };
	return reason;
}

/*
 * vm_translated_rw() - gpuvm=on 时按 VMID 翻译并访问用户地址
 *
 * CP 或计算执行器给出的是 32 位 GPUVA，必须先经 vmid 的三级页表
 * 换成 GPU 物理地址才能访问。一次请求可能跨越多个 4 KiB 页，
 * 所以按页拆开逐段处理：
 *
 * 1. 检查 vmid 的上下文已启用，请求不越过 GPUVA 上限，
 *    长度与对齐合法；
 * 2. 增加上下文的在途访问计数。访问 Host 内存时 vfio-user DMA
 *    可能暂时释放主锁，此时 Host 写 INVALIDATE 或上下文寄存器
 *    会因计数非零被拒绝，页表根和 TLB 不会在访问中途被换掉；
 * 3. 逐页查 TLB 或遍历页表，检查叶子项的权限位，再读写物理页；
 * 4. 撤销计数。失败时按 HQD 槽位记录首个故障，已完成的页不回退。
 *
 * 参数按值保存，跨进程 DMA 期间的寄存器写入不影响本次访问。
 */
static bool vm_translated_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr,
			     uint8_t *buf, size_t len, uint32_t access, uint32_t phase)
{
	GPGPUVMContext *ctx;
	uint32_t reason;

	/*
	 * 1. 上下文未启用记 CONTEXT 故障；
	 *    地址越界、长度或对齐不合法记 RANGE 故障。
	 */
	if (!gpgpu_vm_context_enabled(s, vmid)) {
		reason = GPGPU_VM_FAULT_CONTEXT;
		goto fail;
	}
	if (!gpgpu_range_ok(addr, len, GPGPU_VM_ADDRESS_LIMIT) ||
	    !user_request_valid(addr, len, access)) {
		reason = GPGPU_VM_FAULT_RANGE;
		goto fail;
	}

	/*
	 * 2. 计数非零期间 vm_context_busy() 为真，Host 发起的失效
	 *    和上下文修改都会被拒绝，直到第 4 步撤销计数。
	 */
	ctx = &s->vm.contexts[vmid];
	ctx->accesses++;

	/*
	 * 3. 每轮最多处理到当前页末尾：查得叶子 PTE，检查它是否含
	 *    本次访问的 R/W/X 位，再按“物理页基址 + 页内偏移”访问。
	 *    任一步失败就停在该页。
	 */
	reason = 0;
	while (len && !reason) {
		uint32_t offset = addr & (GPGPU_VM_PAGE_SIZE - 1);
		size_t chunk = MIN(len, GPGPU_VM_PAGE_SIZE - offset);
		uint64_t pte;

		reason = vm_lookup(s, vmid, addr, &pte);
		if (!reason && !(pte & access))
			reason = GPGPU_VM_FAULT_PERMISSION;
		if (!reason && gpgpu_phys_rw(s, (pte & GPGPU_VM_ADDR_MASK) + offset, buf, chunk,
					     access == GPGPU_VM_WRITE) != MEMTX_OK)
			reason = GPGPU_VM_FAULT_STORAGE;
		if (!reason) {
			addr += chunk;
			buf += chunk;
			len -= chunk;
		}
	}

	/*
	 * 4. 成功或失败都撤销计数。失败时 addr 停在出错页内，
	 *    作为故障地址记录。
	 */
	ctx->accesses--;
	if (!reason)
		return true;

fail:
	gpgpu_vm_record_fault(s, vmid, slot, addr, access, phase, reason);
	return false;
}

/* gpuvm=off 时的用户访问：地址即 GPU 物理地址，只检查保护区。 */
static bool vm_direct_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr, void *buf,
			 size_t len, uint32_t access, uint32_t phase)
{
	uint32_t reason = 0;

	if (!user_request_valid(addr, len, access) || !user_phys_valid(s, addr, len))
		reason = GPGPU_VM_FAULT_RANGE;
	else if (gpgpu_phys_rw(s, addr, buf, len, access == GPGPU_VM_WRITE) != MEMTX_OK)
		reason = GPGPU_VM_FAULT_STORAGE;
	if (!reason)
		return true;
	gpgpu_vm_record_fault(s, vmid, slot, addr, access, phase, reason);
	return false;
}

bool gpgpu_user_rw(GPGPUState *s, uint32_t vmid, uint32_t slot, uint64_t addr, void *buf,
		   size_t len, uint32_t access, uint32_t phase)
{
	if (s->gpuvm)
		return vm_translated_rw(s, vmid, slot, addr, buf, len, access, phase);
	return vm_direct_rw(s, vmid, slot, addr, buf, len, access, phase);
}

bool gpgpu_vm_context_enabled(GPGPUState *s, unsigned int vmid)
{
	return vmid >= 1 && vmid <= GPGPU_VM_USERS && s->vm.contexts[vmid].enabled;
}

/* 上下文有激活的用户队列或在途访问时，Host 不能修改它或使其失效。 */
static bool vm_context_busy(GPGPUState *s, unsigned int vmid)
{
	return s->vm.contexts[vmid].accesses || gpgpu_hqd_vmid_active(s, vmid);
}

/* reg 所属的用户上下文：返回 VMID，*field 为组内偏移；不属于时返回 0。 */
static unsigned int vm_context_of(hwaddr reg, hwaddr *field)
{
	for (unsigned int vmid = 1; vmid <= GPGPU_VM_USERS; vmid++) {
		hwaddr base = GPGPU_REG_VM_CONTEXT(vmid);

		if (reg >= base && reg < base + VM_CONTEXT_STRIDE) {
			*field = reg - base;
			return vmid;
		}
	}
	return 0;
}

/* reg 所属的故障组：返回槽号，*field 为组内偏移；不在故障区返回 -1。 */
static int vm_fault_slot_of(hwaddr reg, hwaddr *field)
{
	if (reg < GPGPU_REG_VM_FAULT(0) || reg >= GPGPU_REG_VM_FAULT(GPGPU_HQD_SLOTS))
		return -1;
	*field = (reg - GPGPU_REG_VM_FAULT(0)) % VM_FAULT_STRIDE;
	return (reg - GPGPU_REG_VM_FAULT(0)) / VM_FAULT_STRIDE;
}

static uint32_t vm_fault_read(const GPGPUVMFault *f, hwaddr field)
{
	switch (field) {
	case GPGPU_VM_FAULT_REASON:
		return f->reason;
	case GPGPU_VM_FAULT_VMID:
		return f->vmid;
	case GPGPU_VM_FAULT_ADDR_LO:
	case GPGPU_VM_FAULT_ADDR_HI:
		return gpgpu_reg64_read(f->address, field - GPGPU_VM_FAULT_ADDR_LO);
	case GPGPU_VM_FAULT_ACCESS:
		return f->access;
	case GPGPU_VM_FAULT_PHASE:
		return f->phase;
	default:
		return 0;
	}
}

uint32_t gpgpu_vm_read(GPGPUState *s, hwaddr reg)
{
	unsigned int vmid;
	hwaddr field;
	int slot;

	switch (reg) {
	case GPGPU_REG_VM_VERSION:
		return GPGPU_PROTOCOL_VERSION;
	case GPGPU_REG_VM_MODE:
		return s->gpuvm;
	case GPGPU_REG_VM_FIRMWARE:
		return s->vm.firmware;
	case GPGPU_REG_VM_STATUS:
		return s->vm.status;
	}

	vmid = vm_context_of(reg, &field);
	if (vmid) {
		const GPGPUVMContext *ctx = &s->vm.contexts[vmid];

		if (field == GPGPU_VM_ROOT_LO || field == GPGPU_VM_ROOT_HI)
			return gpgpu_reg64_read(ctx->root, field - GPGPU_VM_ROOT_LO);
		return field == GPGPU_VM_ENABLE ? ctx->enabled : 0;
	}

	slot = vm_fault_slot_of(reg, &field);
	return slot >= 0 ? vm_fault_read(&s->vm.fault[slot], field) : 0;
}

/*
 * 同步失效 vmid 的全部 TLB 项，完成后 VM_STATUS 为 ACK。VMID 非法、
 * 上下文还有激活的队列或在途访问时不清除任何项，VM_STATUS 为 ERROR。
 */
static void vm_invalidate(GPGPUState *s, uint32_t vmid)
{
	if (vmid < 1 || vmid > GPGPU_VM_USERS || vm_context_busy(s, vmid)) {
		s->vm.status = GPGPU_VM_ERROR;
		return;
	}
	for (unsigned int i = 0; i < GPGPU_VM_TLB_ENTRIES; i++)
		if (s->vm.tlb[i].vmid == vmid)
			memset(&s->vm.tlb[i], 0, sizeof(s->vm.tlb[i]));
	s->vm.status = GPGPU_VM_ACK;
}

/*
 * 上下文寄存器写入规则：VM 忙时全部拒绝；ENABLE 需要合法根页；
 * 启用后根只读；ROOT_LO 须页对齐，ROOT_HI 只接受 0。
 */
static bool vm_context_write(GPGPUState *s, unsigned int vmid, hwaddr field, uint32_t value)
{
	GPGPUVMContext *ctx = &s->vm.contexts[vmid];

	if (vm_context_busy(s, vmid))
		return false;
	if (field == GPGPU_VM_ENABLE) {
		if (value > 1 || (value && !vm_page_valid(s, ctx->root)))
			return false;
		ctx->enabled = value;
		return true;
	}
	if (ctx->enabled)
		return false;
	if (field == GPGPU_VM_ROOT_LO && !(value & (GPGPU_VM_PAGE_SIZE - 1))) {
		gpgpu_reg64_write(&ctx->root, 0, value);
		return true;
	}
	if (field == GPGPU_VM_ROOT_HI && !value) {
		gpgpu_reg64_write(&ctx->root, 4, 0);
		return true;
	}
	return false;
}

/*
 * Host 写入分三类：INVALIDATE、上下文寄存器、故障清除。其余偏移以及
 * gpuvm=off 时的前两类都锁存 INVALID_CMD。
 */
void gpgpu_vm_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	unsigned int vmid;
	hwaddr field;
	int slot;

	if (reg == GPGPU_REG_VM_INVALIDATE) {
		if (!s->gpuvm)
			goto invalid;
		vm_invalidate(s, value);
		return;
	}

	vmid = vm_context_of(reg, &field);
	if (vmid) {
		if (!s->gpuvm || !vm_context_write(s, vmid, field, value))
			goto invalid;
		return;
	}

	/* 故障组只接受 CLEAR=1，且所属 HQD 必须未激活。 */
	slot = vm_fault_slot_of(reg, &field);
	if (slot >= 0 && field == GPGPU_VM_FAULT_CLEAR && value == 1 &&
	    !gpgpu_hqd_active(s, slot)) {
		memset(&s->vm.fault[slot], 0, sizeof(s->vm.fault[slot]));
		return;
	}

invalid:
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

bool gpgpu_vm_firmware_handshake(GPGPUState *s, uint32_t version)
{
	if (version != GPGPU_PROTOCOL_VERSION)
		return false;
	s->vm.firmware = version;
	return true;
}

void gpgpu_vm_firmware_reset(GPGPUState *s)
{
	s->vm.firmware = 0;
}

void gpgpu_vm_reset(GPGPUState *s)
{
	memset(&s->vm, 0, sizeof(s->vm));
}
