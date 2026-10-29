// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CP 控制：复位保持、复位向量与板级回调
 */
#include "qemu/osdep.h"
#include "hw/gpgpu/gpgpu_state.h"
#include "hw/gpgpu/mem/gpuvm.h"
#include "hw/gpgpu/mem/vram.h"
#include "cp_ctrl.h"
#include "cp_window.h"

void gpgpu_cp_attach(GPGPUState *s, GPGPUCPSetState set_state, void *opaque)
{
	assert(set_state && !s->cp.set_state);
	s->cp.set_state = set_state;
	s->cp.opaque = opaque;
	gpgpu_cp_hold(s);
}

void gpgpu_cp_on_cpu_reset(GPGPUState *s)
{
	gpgpu_cp_window_reset(s);
	gpgpu_vm_firmware_reset(s);
}

void gpgpu_cp_hold(GPGPUState *s)
{
	if (!s->cp.set_state)
		return;
	s->cp.reset_hold = true;
	s->cp.reset_vector = 0;
	s->cp.set_state(s->cp.opaque, true, 0);
}

void gpgpu_cp_reset(GPGPUState *s)
{
	gpgpu_cp_hold(s);
	s->cp.started = false;
}

bool gpgpu_cp_cold(GPGPUState *s)
{
	return s->cp.reset_hold && !s->cp.started;
}

bool gpgpu_cp_held(GPGPUState *s)
{
	return s->cp.reset_hold;
}

uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr reg)
{
	if (!s->cp.set_state)
		return 0;
	switch (reg) {
	case GPGPU_REG_CP_CTRL:
		return GPGPU_CP_PRESENT | (s->cp.reset_hold ? GPGPU_CP_RESET_HOLD : 0);
	case GPGPU_REG_CP_RESET_VECTOR:
		return s->cp.reset_vector;
	default:
		return 0;
	}
}

/*
 * CP_CTRL：只在 RESET_HOLD 变化时调用回调，重复写入不会重新启动 CPU。
 * CP_RESET_VECTOR：只在保持复位时接受 2 字节对齐、位于 VRAM 内的入口。
 */
void gpgpu_cp_write(GPGPUState *s, hwaddr reg, uint32_t value)
{
	GPGPUCPState *cp = &s->cp;
	bool hold = value & GPGPU_CP_RESET_HOLD;

	if (!cp->set_state)
		return;
	switch (reg) {
	case GPGPU_REG_CP_CTRL:
		if (hold == cp->reset_hold)
			return;
		cp->set_state(cp->opaque, hold, cp->reset_vector);
		cp->reset_hold = hold;
		cp->started |= !hold;
		break;
	case GPGPU_REG_CP_RESET_VECTOR:
		if (!cp->reset_hold || (value & 1) || !gpgpu_vram_contains(s, value, 2)) {
			gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
			return;
		}
		cp->set_state(cp->opaque, true, value);
		cp->reset_vector = value;
		break;
	default:
		break;
	}
}
