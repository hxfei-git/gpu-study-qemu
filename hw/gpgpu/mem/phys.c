// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPU 物理地址路由与 Outbound 窗口
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/remote/vfio-user-obj.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "phys.h"
#include "vram.h"

bool gpgpu_phys_contains(GPGPUState *s, uint64_t addr, uint64_t len)
{
	GPGPUPhysState *p = &s->phys;

	if (!len || !gpgpu_range_ok(addr, len, GPGPU_VM_ADDRESS_LIMIT))
		return false;
	if (addr < s->vram_size)
		return gpgpu_vram_contains(s, addr, len);
	return p->enabled && addr >= p->gpu_base &&
		gpgpu_range_ok(addr - p->gpu_base, len, p->size);
}

/*
 * gpgpu_phys_rw() - 按 GPU 物理地址读写 VRAM 或 Host 内存
 *
 * CP 管理域访问、页表遍历、IH 写记录和 DMA 的 Host 端都经过这里。
 * GPU 物理空间低端是 VRAM，0x8000_0000 起是 Outbound 窗口，窗口内的地址
 * 对应 Host 内存；两者之间的空洞不可路由：
 *
 * 1. 检查 [addr, addr + len) 完整落在 VRAM 或已使能的 Outbound 窗口内，
 *    不允许触及空洞或跨越边界；
 * 2. 增加在途访问计数。访问 Host 内存时 vfio-user DMA 可能暂时释放
 *    主锁，此时 Host 发起的设备复位和 Outbound 重配会因计数非零被拒绝，
 *    窗口配置不会在访问中途被改掉；
 * 3. VRAM 直接读写设备内部的后备 RAM；Outbound 把地址减去窗口起点、
 *    加上 Host 基址换算成 Host IOVA，再经 vfio-user DMA 读写。
 *    例：0xc100_2000 减去 0x8000_0000 得到 IOVA 0x4100_2000；
 * 4. 成功或失败都撤销计数，失败前已写入的部分保留。
 *
 * 使能时 host_base 为 0 且窗口为 2 GiB，换算后的 IOVA 不会溢出。
 */
MemTxResult gpgpu_phys_rw(GPGPUState *s, uint64_t addr, void *buf, size_t len, bool is_write)
{
	GPGPUPhysState *p = &s->phys;
	MemTxResult result;

	/* 1. 范围不可路由时直接失败，不计数。 */
	if (!gpgpu_phys_contains(s, addr, len))
		return MEMTX_ERROR;

	/* 2. 计数非零期间 gpgpu_phys_busy() 为真，直到第 4 步撤销。 */
	p->accesses++;

	/* 3. 按地址落在 VRAM 还是 Outbound 窗口分两条路径。 */
	if (addr < s->vram_size) {
		result = gpgpu_vram_rw(s, addr, buf, len, is_write);
	} else {
		uint64_t iova = p->host_base + (addr - p->gpu_base);

		result = vfu_object_dma_rw(PCI_DEVICE(s), iova, buf, len, is_write);
	}

	/* 4. 只撤销计数，不回退已经写入的数据。 */
	p->accesses--;
	return result;
}

bool gpgpu_phys_busy(GPGPUState *s)
{
	return s->phys.accesses;
}

uint32_t gpgpu_outbound_read(GPGPUState *s, hwaddr reg)
{
	GPGPUPhysState *p = &s->phys;

	switch (reg) {
	case GPGPU_REG_OUTBOUND_GPU_BASE:
		return p->gpu_base;
	case GPGPU_REG_OUTBOUND_SIZE_LO:
		return p->size;
	case GPGPU_REG_OUTBOUND_SIZE_HI:
		return p->size >> 32;
	case GPGPU_REG_OUTBOUND_HOST_LO:
		return p->host_base;
	case GPGPU_REG_OUTBOUND_HOST_HI:
		return p->host_base >> 32;
	case GPGPU_REG_OUTBOUND_CONTROL:
		return p->enabled ? GPGPU_OUTBOUND_ENABLE : 0;
	case GPGPU_REG_OUTBOUND_STATUS:
		return p->status | (p->accesses ? GPGPU_OUTBOUND_BUSY : 0);
	default:
		return 0;
	}
}

/*
 * 布局固定且只读，Host 只能写 CONTROL：所有设备引擎停止、且无在途访问时
 * 才接受，值只能是 0 或 GPGPU_OUTBOUND_ENABLE。其余写入置 ERROR 并锁存
 * INVALID_CMD。
 */
void gpgpu_outbound_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUPhysState *p = &s->phys;

	if (reg != GPGPU_REG_OUTBOUND_CONTROL || (value & ~GPGPU_OUTBOUND_ENABLE) ||
	    p->accesses || !gpgpu_engines_idle(s))
		goto invalid;
	p->enabled = value;
	p->status = GPGPU_OUTBOUND_ACK;
	return;

invalid:
	p->status = GPGPU_OUTBOUND_ERROR;
	gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
}

void gpgpu_outbound_reset(GPGPUState *s)
{
	assert(!s->phys.accesses);
	s->phys = (GPGPUPhysState) {
		.gpu_base = GPGPU_OUTBOUND_BASE,
		.size = GPGPU_OUTBOUND_SIZE,
	};
}
