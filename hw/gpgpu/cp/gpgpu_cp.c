// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 原生 CP 的架构无关控制寄存器
 */

#include "qemu/osdep.h"
#include "gpgpu_cp.h"
#include "gpgpu_cp_window.h"
#include "hw/gpgpu/memory/gpgpu_vram.h"
#include "hw/gpgpu/memory/gpgpu_vm.h"
#include "hw/gpgpu/gpgpu_internal.h"

/*
 * gpgpu_attach_cp() - 连接板级原生 CP 的控制入口
 *
 * 登记回调和私有数据，并立即使 CP 保持复位。
 */
void gpgpu_attach_cp(GPGPUState *s, GPGPUCPSetState set_state, void *opaque)
{
	assert(set_state && !s->cp.set_state);
	s->cp.set_state = set_state;
	s->cp.opaque = opaque;
	gpgpu_cp_reset(s);
}

/*
 * gpgpu_cp_reset() - 使已连接的 CP 保持复位
 *
 * 清零启动入口并通知板级回调，不清除 HQD 或队列错误。
 */
void gpgpu_cp_reset(GPGPUState *s)
{
	if (!s->cp.set_state)
		return;

	s->cp.reset_hold = true;
	s->cp.reset_vector = 0;
	s->cp.set_state(s->cp.opaque, true, 0);
}

/*
 * gpgpu_cp_read() - 读回 CP 控制状态或复位入口
 *
 * 未连接原生 CP 时返回零，PRESENT 由回调是否存在决定。
 */
uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr addr)
{
	if (!s->cp.set_state)
		return 0;

	switch (addr) {
	case GPGPU_REG_CP_CTRL:
		return GPGPU_CP_PRESENT | (s->cp.reset_hold ? GPGPU_CP_RESET_HOLD : 0);
	case GPGPU_REG_CP_RESET_VECTOR:
		return s->cp.reset_vector;
	default:
		return 0;
	}
}

/*
 * gpgpu_cp_write() - 处理 CP 复位与启动控制
 *
 * 仅在保持复位时允许修改入口；状态转换才通知板级代码，
 * 重复写不重启。
 */
void gpgpu_cp_write(GPGPUState *s, hwaddr addr, uint32_t val)
{
	if (!s->cp.set_state)
		return;

	switch (addr) {
	case GPGPU_REG_CP_CTRL: {
		bool reset_hold = val & GPGPU_CP_RESET_HOLD;

		/* 重复写入不改变 PC，不再次启动 CPU。 */
		if (reset_hold != s->cp.reset_hold) {
			s->cp.set_state(s->cp.opaque, reset_hold, s->cp.reset_vector);
			s->cp.reset_hold = reset_hold;
			s->cp.started |= !reset_hold;
		}
		break;
	}
	case GPGPU_REG_CP_RESET_VECTOR:
		if (!s->cp.reset_hold || (val & 1) || !gpgpu_vram_contains(s, val, 2)) {
			gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
			return;
		}
		s->cp.set_state(s->cp.opaque, true, val);
		s->cp.reset_vector = val;
		break;
	default:
		break;
	}
}

void gpgpu_cp_device_reset(GPGPUState *s)
{
	gpgpu_cp_reset(s);
	s->cp.started = false;
}

void gpgpu_cp_reset_access(GPGPUState *s)
{
	/* 保持原生 CPU 复位中的清理时机，不能提前到跨线程回调之前。 */
	gpgpu_cp_window_reset(s);
	gpgpu_vm_reset_firmware(s);
}

bool gpgpu_cp_host_configurable(GPGPUState *s)
{
	return s->cp.reset_hold && !s->cp.started;
}
