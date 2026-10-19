/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU 教学用 GPGPU 设备
 * Copyright (c) 2024-2025
 */

#include "qemu/osdep.h"
#include "gpgpu.h"

/*
 * gpgpu_reset_state() - 复位设备寄存器与执行状态
 *
 * 先停止 CP 和计算任务，再清队列、中断及 DMA 状态；此入口保留 VRAM。
 */
void gpgpu_reset_state(GPGPUState *s)
{
	/* 停止 CP 后再改变设备状态；此处保留 VRAM。 */
	gpgpu_cp_reset(s);
	gpgpu_dispatch_reset(s);
	s->cp_started = false;
	memset(s->vm, 0, sizeof(s->vm));
	memset(s->tlb, 0, sizeof(s->tlb));
	memset(s->vm_fault, 0, sizeof(s->vm_fault));
	s->vm_status = s->vm_firmware = 0;
	s->queue_mode = false;
	s->hqd_select = 0;
	memset(s->hqd, 0, sizeof(s->hqd));
	memset(s->doorbell_pending, 0, sizeof(s->doorbell_pending));
	memset(&s->ih, 0, sizeof(s->ih));
	s->global_ctrl = 0;
	s->global_status = GPGPU_STATUS_READY;
	s->error_status = 0;
	gpgpu_irq_clear(s);
	memset(&s->kernel, 0, sizeof(s->kernel));
	memset(&s->simt, 0, sizeof(s->simt));
	gpgpu_dma_reset(s);
}

/*
 * gpgpu_ctrl_read() - 分发 Host BAR0 读访问
 *
 * 要求对齐的 32 位访问，再按寄存器组交给 HQD、IH、DMA 或派发模块。
 */
static uint64_t gpgpu_ctrl_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 4 || (addr & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
			gpgpu_ih_write(s, UINT64_MAX, 0, false);
		return 0;
	}
	if (addr >= 0x0900 && addr < 0x0b00)
		return gpgpu_vm_read(s, addr);
	if (addr == GPGPU_REG_HQD_SELECT)
		return s->hqd_select;
	if ((addr & ~0xffULL) == GPGPU_REG_HQD_WINDOW)
		return gpgpu_hqd_read(s, s->hqd_select, addr & 0xff, size);
	if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
		return gpgpu_ih_read(s, addr);

	/* 每个 0x100 字节的寄存器组对应一个设备模块。 */
	switch (addr & ~0xffULL) {
	case GPGPU_REG_IRQ_ENABLE:
		return gpgpu_irq_read(s, addr);
	case GPGPU_REG_KERNEL_ADDR_LO:
		return gpgpu_kernel_read(s, addr);
	case GPGPU_REG_DMA_SRC_LO:
		return gpgpu_dma_read(s, addr);
	case GPGPU_REG_CP_CTRL:
		return gpgpu_cp_read(s, addr);
	}

	switch (addr) {
	case GPGPU_REG_DEV_ID:
		return GPGPU_DEV_ID_VALUE;
	case GPGPU_REG_DEV_VERSION:
		return GPGPU_DEV_VERSION_VALUE;
	case GPGPU_REG_DEV_CAPS:
		return (s->num_cus & 0xff) | ((s->warps_per_cu & 0xff) << 8) |
			((s->warp_size & 0xff) << 16);
	case GPGPU_REG_VRAM_SIZE_LO:
		return (uint32_t)s->vram_size;
	case GPGPU_REG_VRAM_SIZE_HI:
		return (uint32_t)(s->vram_size >> 32);
	case GPGPU_REG_GLOBAL_CTRL:
		return s->global_ctrl;
	case GPGPU_REG_GLOBAL_STATUS:
		return s->global_status;
	case GPGPU_REG_ERROR_STATUS:
		return s->error_status;
	case GPGPU_REG_THREAD_ID_X:
		return s->simt.thread_id[0];
	case GPGPU_REG_THREAD_ID_Y:
		return s->simt.thread_id[1];
	case GPGPU_REG_THREAD_ID_Z:
		return s->simt.thread_id[2];
	case GPGPU_REG_BLOCK_ID_X:
		return s->simt.block_id[0];
	case GPGPU_REG_BLOCK_ID_Y:
		return s->simt.block_id[1];
	case GPGPU_REG_BLOCK_ID_Z:
		return s->simt.block_id[2];
	case GPGPU_REG_WARP_ID:
		return s->simt.warp_id;
	case GPGPU_REG_LANE_ID:
		return s->simt.lane_id;
	case GPGPU_REG_THREAD_MASK:
		return s->simt.thread_mask;
	default:
		return 0;
	}
}

/*
 * gpgpu_ctrl_write() - 分发 Host BAR0 写访问
 *
 * 校验宽度和权限，处理全局控制并将各寄存器组转交所属模块。
 */
static void gpgpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 4 || (addr & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
			gpgpu_ih_write(s, UINT64_MAX, 0, false);
		return;
	}
	if (addr >= 0x0900 && addr < 0x0b00) {
		gpgpu_vm_write(s, addr, val);
		return;
	}
	if (addr == GPGPU_REG_HQD_SELECT) {
		if (val < GPGPU_QUEUE_SLOTS)
			s->hqd_select = val;
		else
			gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		return;
	}
	if ((addr & ~0xffULL) == GPGPU_REG_HQD_WINDOW) {
		gpgpu_hqd_write(s, s->hqd_select, addr & 0xff, val, size, false);
		return;
	}
	if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO) {
		gpgpu_ih_write(s, addr, val, false);
		return;
	}

	switch (addr & ~0xffULL) {
	case GPGPU_REG_IRQ_ENABLE:
		gpgpu_irq_write(s, addr, val);
		return;
	case GPGPU_REG_KERNEL_ADDR_LO:
		gpgpu_kernel_write(s, addr, val);
		return;
	case GPGPU_REG_DMA_SRC_LO:
		gpgpu_dma_write(s, addr, val);
		return;
	case GPGPU_REG_CP_CTRL:
		gpgpu_cp_write(s, addr, val);
		return;
	}

	switch (addr) {
	case GPGPU_REG_GLOBAL_CTRL:
		if (val & GPGPU_CTRL_RESET)
			gpgpu_reset_state(s);
		else
			s->global_ctrl = val & GPGPU_CTRL_ENABLE;
		break;
	case GPGPU_REG_ERROR_STATUS:
		s->error_status &= ~(uint32_t)val;
		if (!s->error_status)
			s->global_status &= ~GPGPU_STATUS_ERROR;
		break;
	case GPGPU_REG_THREAD_ID_X:
		s->simt.thread_id[0] = (uint32_t)val;
		break;
	case GPGPU_REG_THREAD_ID_Y:
		s->simt.thread_id[1] = (uint32_t)val;
		break;
	case GPGPU_REG_THREAD_ID_Z:
		s->simt.thread_id[2] = (uint32_t)val;
		break;
	case GPGPU_REG_BLOCK_ID_X:
		s->simt.block_id[0] = (uint32_t)val;
		break;
	case GPGPU_REG_BLOCK_ID_Y:
		s->simt.block_id[1] = (uint32_t)val;
		break;
	case GPGPU_REG_BLOCK_ID_Z:
		s->simt.block_id[2] = (uint32_t)val;
		break;
	case GPGPU_REG_WARP_ID:
		s->simt.warp_id = (uint32_t)val;
		break;
	case GPGPU_REG_LANE_ID:
		s->simt.lane_id = (uint32_t)val;
		break;
	case GPGPU_REG_THREAD_MASK:
		s->simt.thread_mask = (uint32_t)val;
		break;
	default:
		break;
	}
}

const MemoryRegionOps gpgpu_ctrl_ops = {
    .read = gpgpu_ctrl_read,
    .write = gpgpu_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/*
 * cp_regs_read() - 分发 CP 本地控制窗口的读访问
 *
 * 只暴露派发、IH 和全局错误状态，非法偏移锁存命令错误。
 */
static uint64_t cp_regs_read(void *opaque, hwaddr addr, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 4 || (addr & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
			gpgpu_ih_write(s, UINT64_MAX, 0, false);
		return 0;
	}
	if (addr >= 0x0900 && addr < 0x0a00)
		return gpgpu_vm_read(s, addr);
	if ((addr & ~0xffULL) == GPGPU_REG_KERNEL_ADDR_LO)
		return gpgpu_kernel_read(s, addr);
	if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
		return gpgpu_ih_read(s, addr);
	if (addr == GPGPU_REG_ERROR_STATUS)
		return s->error_status;
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
	return 0;
}

/*
 * cp_regs_write() - 分发 CP 本地控制窗口的写访问
 *
 * 只允许执行器配置/提交/确认及 IH 提交，不暴露 Host 管理寄存器。
 */
static void cp_regs_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
	GPGPUState *s = opaque;

	if (size != 4 || (addr & 3)) {
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
		if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
			gpgpu_ih_write(s, UINT64_MAX, 0, false);
		return;
	}
	if (addr == GPGPU_REG_VM_FIRMWARE && s->gpuvm && val == GPGPU_VM_VERSION) {
		s->vm_firmware = val;
		return;
	}
	if ((addr & ~0xffULL) == GPGPU_REG_KERNEL_ADDR_LO)
		gpgpu_kernel_cp_write(s, addr, val);
	else if ((addr & ~0xffULL) == GPGPU_REG_IH_BASE_LO)
		gpgpu_ih_write(s, addr, val, true);
	else
		gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

const MemoryRegionOps gpgpu_cp_regs_ops = {
	.read = cp_regs_read,
	.write = cp_regs_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
	.impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};
