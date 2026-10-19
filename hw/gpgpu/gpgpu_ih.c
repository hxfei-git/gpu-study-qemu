/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "hw/remote/vfio-user-obj.h"
#include "gpgpu.h"

/*
 * ih_fault() - 锁存 IH 通道首次故障
 *
 * 已有错误不被覆盖，同时设置全局错误并通知 Host。
 */
static void ih_fault(GPGPUState *s, unsigned int error)
{
	if (!(s->ih.status & GPGPU_IH_FAULT))
		s->ih.status |= GPGPU_IH_FAULT | error << GPGPU_IH_ERROR_SHIFT;
	gpgpu_set_error(s, GPGPU_ERR_IH_FAULT);
}

/*
 * gpgpu_ih_refresh_irq() - 为未消费的 IH 记录保持通知
 *
 * 比较记录编号 W/R，只要 Ring 非空就重新提出 IH 中断。
 */
void gpgpu_ih_refresh_irq(GPGPUState *s)
{
	if (s->ih.wptr != s->ih.rptr)
		gpgpu_raise_irq(s, GPGPU_IRQ_IH);
}

/*
 * ih_config_valid() - 校验 IH Ring 的布局
 *
 * 要求页对齐基址、2 的幂字节容量，并检查末地址不溢出。
 */
static bool ih_config_valid(GPGPUIHState *ih)
{
	return !(ih->base & 4095) && ih->size >= sizeof(GpgpuIhRecord) && is_power_of_2(ih->size) &&
		ih->base <= UINT64_MAX - (ih->size - 1);
}

/*
 * ih_record_valid() - 校验固件提交的通知记录
 *
 * 按类型核对槽号、MQD、事件或错误值，同时拒绝非零保留字段。
 */
static bool ih_record_valid(const uint8_t *record)
{
	uint32_t type = ldl_le_p(record);
	uint32_t slot = ldl_le_p(record + 4);
	uint64_t mqd = ldq_le_p(record + 8);
	uint32_t data = ldl_le_p(record + 16);

	if (ldl_le_p(record + 20) || ldl_le_p(record + 24) || ldl_le_p(record + 28))
		return false;
	switch (type) {
	case GPGPU_IH_SIGNAL_NOTIFY:
		return slot >= 2 && slot < 4 && !(mqd & 7) && data;
	case GPGPU_IH_QUEUE_ERROR:
		return slot < 4 && !(mqd & 7) && data && data <= GPGPU_QUEUE_ERROR_SAVE;
	case GPGPU_IH_DEVICE_ERROR:
		return slot == UINT32_MAX && mqd == 0 && data;
	default:
		return false;
	}
}

/*
 * ih_submit() - 同步将固件记录提交到 Host IH Ring
 *
 * 检查私有 VRAM 记录和可用槽，先写记录、再发布 W、最后发中断。
 */
static void ih_submit(GPGPUState *s, uint32_t addr)
{
	GPGPUIHState *ih = &s->ih;
	uint8_t record[sizeof(GpgpuIhRecord)];
	unsigned int result = 0;
	uint32_t capacity;

	if (ih->status & GPGPU_IH_FAULT) {
		result = GPGPU_IH_ERR_FAULTED;
		goto done;
	}
	if (!(ih->ctrl & GPGPU_IH_ENABLE)) {
		result = GPGPU_IH_ERR_DISABLED;
		goto done;
	}
	if (!ih_config_valid(ih)) {
		result = GPGPU_IH_ERR_CONFIG;
		goto done;
	}
	if ((addr & 7) || addr < GPGPU_FIRMWARE_BASE ||
	    addr > GPGPU_FIRMWARE_BASE + GPGPU_FIRMWARE_SIZE - sizeof(record) ||
	    !gpgpu_vram_contains(s, addr, sizeof(record))) {
		result = GPGPU_IH_ERR_RECORD;
		goto done;
	}
	memcpy(record, s->vram_ptr + addr, sizeof(record));
	if (!ih_record_valid(record)) {
		result = GPGPU_IH_ERR_RECORD;
		goto done;
	}
	capacity = ih->size / sizeof(record);
	if ((uint32_t)(ih->wptr - ih->rptr) >= capacity) {
		result = GPGPU_IH_ERR_FULL;
		goto done;
	}
	ih->status |= GPGPU_IH_BUSY;
	if (vfu_object_dma_rw(PCI_DEVICE(s), ih->base + (ih->wptr % capacity) * sizeof(record),
			      record, sizeof(record), true) != MEMTX_OK) {
		result = GPGPU_IH_ERR_DMA;
		goto done;
	}
	/* DMA 写完记录后才发布 W，再发送通知。 */
	smp_wmb();
	ih->wptr++;
	if (ldl_le_p(record) == GPGPU_IH_DEVICE_ERROR)
		gpgpu_set_error(s, ldl_le_p(record + 16));
	gpgpu_ih_refresh_irq(s);
done:
	ih->status &= ~(GPGPU_IH_BUSY | (0xffu << GPGPU_IH_RESULT_SHIFT));
	ih->status |= result << GPGPU_IH_RESULT_SHIFT;
	if (result)
		ih_fault(s, result);
}

/*
 * gpgpu_ih_read() - 读回 IH 配置、进度与状态
 *
 * 寄存器为 32 位小端值，未定义偏移返回零。
 */
uint32_t gpgpu_ih_read(GPGPUState *s, hwaddr addr)
{
	switch (addr) {
	case GPGPU_REG_IH_BASE_LO:
		return s->ih.base;
	case GPGPU_REG_IH_BASE_HI:
		return s->ih.base >> 32;
	case GPGPU_REG_IH_SIZE:
		return s->ih.size;
	case GPGPU_REG_IH_WPTR:
		return s->ih.wptr;
	case GPGPU_REG_IH_RPTR:
		return s->ih.rptr;
	case GPGPU_REG_IH_CTRL:
		return s->ih.ctrl;
	case GPGPU_REG_IH_STATUS:
		return s->ih.status;
	default:
		return 0;
	}
}

/*
 * gpgpu_ih_write() - 区分 Host 配置与 CP 通知提交
 *
 * Host 只能在合法状态修改配置或推进 R；CP 只可通过 SUBMIT 提交记录。
 */
void gpgpu_ih_write(GPGPUState *s, hwaddr addr, uint32_t val, bool from_cp)
{
	GPGPUIHState *ih = &s->ih;

	if (from_cp) {
		if (addr == GPGPU_REG_IH_SUBMIT) {
			ih_submit(s, val);
			return;
		}
		goto invalid;
	}
	switch (addr) {
	case GPGPU_REG_IH_BASE_LO:
	case GPGPU_REG_IH_BASE_HI:
	case GPGPU_REG_IH_SIZE:
		if ((ih->ctrl & GPGPU_IH_ENABLE) || (ih->status & GPGPU_IH_BUSY))
			goto invalid;
		if (addr == GPGPU_REG_IH_SIZE) {
			ih->size = val;
		} else {
			ih->base =
				deposit64(ih->base, addr == GPGPU_REG_IH_BASE_LO ? 0 : 32, 32, val);
		}
		return;
	case GPGPU_REG_IH_RPTR:
		if ((uint32_t)(val - ih->rptr) > (uint32_t)(ih->wptr - ih->rptr))
			goto invalid;
		ih->rptr = val;
		return;
	case GPGPU_REG_IH_CTRL:
		if (val & ~(GPGPU_IH_ENABLE | GPGPU_IH_RESET))
			goto invalid;
		if (val & GPGPU_IH_RESET) {
			if ((ih->ctrl & GPGPU_IH_ENABLE) || (ih->status & GPGPU_IH_BUSY) ||
			    (val & GPGPU_IH_ENABLE)) {
				goto invalid;
			}
			ih->status = ih->wptr = ih->rptr = 0;
			return;
		}
		if ((val & GPGPU_IH_ENABLE) && !ih_config_valid(ih)) {
			ih_fault(s, GPGPU_IH_ERR_CONFIG);
			return;
		}
		ih->ctrl = val;
		return;
	default:
		break;
	}
invalid:
	ih_fault(s, GPGPU_IH_ERR_PROTOCOL);
}
