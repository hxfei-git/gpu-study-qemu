// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IH：Ring 配置、记录提交与故障
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/mem/phys.h"
#include "hw/gpgpu/mem/vram.h"
#include "ih.h"
#include "irq.h"

/* IH_STATUS 中故障码和提交结果字段的宽度掩码。 */
#define IH_CODE_MASK 0xffu
/* Ring 基址的对齐要求。 */
#define IH_BASE_ALIGN 4096u

/* 锁存首次通道故障（只保留第一个故障码），并锁存全局 IH_FAULT。 */
static void ih_fault(GPGPUState *s, uint32_t error)
{
	if (!(s->ih.status & GPGPU_IH_FAULT))
		s->ih.status |= GPGPU_IH_FAULT | error << GPGPU_IH_ERROR_SHIFT;
	gpgpu_set_error(s, GPGPU_ERR_IH_FAULT);
}

void gpgpu_ih_protocol_fault(GPGPUState *s)
{
	ih_fault(s, GPGPU_IH_ERR_PROTOCOL);
}

/* Ring 基址 4 KiB 对齐，容量为 2 的幂且至少一条记录，整段可路由。 */
static bool ih_config_valid(GPGPUState *s)
{
	GPGPUIHState *ih = &s->ih;

	return !(ih->base & (IH_BASE_ALIGN - 1)) && ih->size >= sizeof(GpgpuIhRecord) &&
		is_power_of_2(ih->size) && gpgpu_phys_contains(s, ih->base, ih->size);
}

/* 记录必须位于固件私有 VRAM 内且 8 字节对齐。 */
static bool ih_record_location_valid(GPGPUState *s, uint32_t addr)
{
	return !(addr & 7) && addr >= GPGPU_FIRMWARE_BASE &&
		gpgpu_range_ok(addr - GPGPU_FIRMWARE_BASE, sizeof(GpgpuIhRecord),
			       GPGPU_FIRMWARE_SIZE) &&
		gpgpu_vram_contains(s, addr, sizeof(GpgpuIhRecord));
}

/* 按类型核对来源字段与 data，保留字段必须为零。 */
static bool ih_record_valid(const GpgpuIhRecord *r)
{
	uint32_t type = le32_to_cpu(r->type);
	uint32_t slot = le32_to_cpu(r->hqd_slot);
	uint64_t mqd = le64_to_cpu(r->mqd_addr);
	uint32_t data = le32_to_cpu(r->data);

	if (r->reserved[0] || r->reserved[1] || r->reserved[2])
		return false;

	switch (type) {
	case GPGPU_IH_SIGNAL_NOTIFY:
		return slot >= GPGPU_HQD_USER_FIRST && slot < GPGPU_HQD_SLOTS && !(mqd & 7) && data;
	case GPGPU_IH_QUEUE_ERROR:
		return slot < GPGPU_HQD_SLOTS && !(mqd & 7) && data &&
			data <= GPGPU_QUEUE_ERROR_SAVE;
	case GPGPU_IH_DEVICE_ERROR:
		/* 固件只能报告固件异常，不能借记录写入任意全局错误位。 */
		return slot == UINT32_MAX && !mqd && data == GPGPU_ERR_FIRMWARE;
	default:
		return false;
	}
}

/*
 * ih_submit() - 把固件私有 VRAM 中的一条记录发布到 Host Ring
 *
 * CP 写 IH_SUBMIT 时以记录地址为参数同步调用本函数。CP 不能直接写
 * Host Ring，由设备检查记录后代为写入，保证驱动只会看到格式合法的
 * 记录，且记录内容先于 W 对 Host 可见：
 *
 * 1. 检查通道：未锁存故障、Host 已使能、Ring 基址与容量合法；
 * 2. 检查记录位于固件私有 VRAM 且 8 字节对齐，读出后按类型核对字段；
 * 3. 检查 Ring 有空槽：W - R 达到容量说明 Host 还没消费，返回 FULL；
 * 4. 置 BUSY，把记录写入 Host Ring 的 W 号槽。写 Host 内存经
 *    vfio-user DMA，可能暂时释放主锁，BUSY 期间 Host 不能改配置或复位
 *    IH；BUSY 由调用者在本函数返回后清除；
 * 5. 记录写完才把 W 加 1，再锁存 IH 中断通知 Host，驱动读到新 W 时
 *    记录已经完整。设备错误记录同时锁存全局 FIRMWARE 错误。
 *
 * Return: GPGPU_IH_OK 或 GpgpuIhError。调用者把结果写入 IH_STATUS 的
 * 结果字段，失败时另锁存通道故障。
 */
static uint32_t ih_submit(GPGPUState *s, uint32_t addr)
{
	GPGPUIHState *ih = &s->ih;
	GpgpuIhRecord record;
	uint32_t capacity = ih->size / sizeof(record);
	uint64_t slot_addr;

	/* 1. 通道已故障、未使能或配置非法。 */
	if (ih->status & GPGPU_IH_FAULT)
		return GPGPU_IH_ERR_FAULTED;
	if (!(ih->ctrl & GPGPU_IH_ENABLE))
		return GPGPU_IH_ERR_DISABLED;
	if (!ih_config_valid(s))
		return GPGPU_IH_ERR_CONFIG;

	/* 2. 记录位置合法后才读取，再检查内容。 */
	if (!ih_record_location_valid(s, addr))
		return GPGPU_IH_ERR_RECORD;
	gpgpu_vram_rw(s, addr, &record, sizeof(record), false);
	if (!ih_record_valid(&record))
		return GPGPU_IH_ERR_RECORD;

	/* 3. W、R 都是 32 位记录编号，相减回绕后仍是在途记录数。 */
	if (ih->wptr - ih->rptr >= capacity)
		return GPGPU_IH_ERR_FULL;

	/* 4. W 号记录落在第 (W mod 容量) 个槽。 */
	slot_addr = ih->base + (uint64_t)(ih->wptr % capacity) * sizeof(record);
	ih->status |= GPGPU_IH_BUSY;
	if (gpgpu_phys_rw(s, slot_addr, &record, sizeof(record), true) != MEMTX_OK)
		return GPGPU_IH_ERR_DMA;

	/* 5. 先发布 W，再置中断，顺序不能交换。 */
	ih->wptr++;
	if (le32_to_cpu(record.type) == GPGPU_IH_DEVICE_ERROR)
		gpgpu_set_error(s, GPGPU_ERR_FIRMWARE);
	gpgpu_raise_irq(s, GPGPU_IRQ_IH);
	return GPGPU_IH_OK;
}

/*
 * CP 只能写 IH_SUBMIT，值为记录地址。提交结束后清除 BUSY，并把本次
 * 结果写入 IH_STATUS 的结果字段，固件读回它确认提交是否成功。
 */
void gpgpu_ih_cp_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUIHState *ih = &s->ih;
	uint32_t result;

	if (reg != GPGPU_REG_IH_SUBMIT) {
		gpgpu_ih_protocol_fault(s);
		return;
	}

	result = ih_submit(s, value);
	ih->status &= ~(GPGPU_IH_BUSY | IH_CODE_MASK << GPGPU_IH_RESULT_SHIFT);
	ih->status |= result << GPGPU_IH_RESULT_SHIFT;
	if (result != GPGPU_IH_OK)
		ih_fault(s, result);
}

uint32_t gpgpu_ih_read(GPGPUState *s, hwaddr reg)
{
	GPGPUIHState *ih = &s->ih;

	switch (reg) {
	case GPGPU_REG_IH_BASE_LO:
	case GPGPU_REG_IH_BASE_HI:
		return gpgpu_reg64_read(ih->base, reg - GPGPU_REG_IH_BASE_LO);
	case GPGPU_REG_IH_SIZE:
		return ih->size;
	case GPGPU_REG_IH_WPTR:
		return ih->wptr;
	case GPGPU_REG_IH_RPTR:
		return ih->rptr;
	case GPGPU_REG_IH_CTRL:
		return ih->ctrl;
	case GPGPU_REG_IH_STATUS:
		return ih->status;
	default:
		return 0;
	}
}

/*
 * IH_CTRL 写入：
 * - RESET：只在停用、不忙且不同时置 ENABLE 时接受，清零 W/R 与状态，
 *   包括锁存的通道故障，驱动借此在重新使能前恢复通道；
 * - ENABLE：配置非法时锁存 CONFIG 故障而不使能。
 *
 * Return: false 表示写入不合法，由调用者锁存 PROTOCOL 故障。
 */
static bool ih_ctrl_write(GPGPUState *s, uint32_t value)
{
	GPGPUIHState *ih = &s->ih;

	if (value & ~(GPGPU_IH_ENABLE | GPGPU_IH_RESET))
		return false;

	if (value & GPGPU_IH_RESET) {
		if ((ih->ctrl & GPGPU_IH_ENABLE) || (ih->status & GPGPU_IH_BUSY) ||
		    (value & GPGPU_IH_ENABLE))
			return false;
		ih->status = ih->wptr = ih->rptr = 0;
		return true;
	}

	if ((value & GPGPU_IH_ENABLE) && !ih_config_valid(s)) {
		ih_fault(s, GPGPU_IH_ERR_CONFIG);
		return true;
	}
	ih->ctrl = value;
	return true;
}

/*
 * Host 写入规则：
 * - Ring 基址与容量只在停用且不忙时可改，避免设备写到一半换地址；
 * - RPTR 只能在 [R, W] 范围内前进，用无符号减法处理 32 位回绕，
 *   驱动不能归还设备还没写的槽，也不能让 R 后退；
 * - IH_SUBMIT 只供 CP 使用，Host 写入它或未定义偏移都锁存 PROTOCOL 故障。
 */
void gpgpu_ih_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUIHState *ih = &s->ih;
	bool configurable = !(ih->ctrl & GPGPU_IH_ENABLE) && !(ih->status & GPGPU_IH_BUSY);
	bool ok = false;

	switch (reg) {
	case GPGPU_REG_IH_BASE_LO:
	case GPGPU_REG_IH_BASE_HI:
		ok = configurable;
		if (ok)
			gpgpu_reg64_write(&ih->base, reg - GPGPU_REG_IH_BASE_LO, value);
		break;
	case GPGPU_REG_IH_SIZE:
		ok = configurable;
		if (ok)
			ih->size = value;
		break;
	case GPGPU_REG_IH_RPTR:
		ok = value - ih->rptr <= ih->wptr - ih->rptr;
		if (ok)
			ih->rptr = value;
		break;
	case GPGPU_REG_IH_CTRL:
		ok = ih_ctrl_write(s, value);
		break;
	default:
		break;
	}

	if (!ok)
		gpgpu_ih_protocol_fault(s);
}

bool gpgpu_ih_pending(GPGPUState *s)
{
	return s->ih.wptr != s->ih.rptr;
}

bool gpgpu_ih_faulted(GPGPUState *s)
{
	return s->ih.status & GPGPU_IH_FAULT;
}

bool gpgpu_ih_busy(GPGPUState *s)
{
	return s->ih.status & GPGPU_IH_BUSY;
}

void gpgpu_ih_reset(GPGPUState *s)
{
	s->ih = (GPGPUIHState) { 0 };
}
