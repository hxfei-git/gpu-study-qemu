/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * 原生 CP 的架构无关控制寄存器
 */

#include "qemu/osdep.h"
#include "gpgpu.h"

/*
 * gpgpu_attach_cp() - 连接板级原生 CP 的控制入口
 *
 * 登记回调和私有数据，并立即使 CP 保持复位。
 */
void gpgpu_attach_cp(GPGPUState *s, GPGPUCPSetState set_state, void *opaque)
{
	assert(set_state && !s->cp_set_state);
	s->cp_set_state = set_state;
	s->cp_opaque = opaque;
	gpgpu_cp_reset(s);
}

/*
 * gpgpu_cp_reset() - 使已连接的 CP 保持复位
 *
 * 清零启动入口并通知板级回调，不清除 HQD 或队列错误。
 */
void gpgpu_cp_reset(GPGPUState *s)
{
	if (!s->cp_set_state)
		return;

	s->cp_reset_hold = true;
	s->cp_reset_vector = 0;
	s->cp_set_state(s->cp_opaque, true, 0);
}

/*
 * gpgpu_cp_read() - 读回 CP 控制状态或复位入口
 *
 * 未连接原生 CP 时返回零，PRESENT 由回调是否存在决定。
 */
uint32_t gpgpu_cp_read(GPGPUState *s, hwaddr addr)
{
	if (!s->cp_set_state)
		return 0;

	switch (addr) {
	case GPGPU_REG_CP_CTRL:
		return GPGPU_CP_PRESENT | (s->cp_reset_hold ? GPGPU_CP_RESET_HOLD : 0);
	case GPGPU_REG_CP_RESET_VECTOR:
		return s->cp_reset_vector;
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
	if (!s->cp_set_state)
		return;

	switch (addr) {
	case GPGPU_REG_CP_CTRL: {
		bool reset_hold = val & GPGPU_CP_RESET_HOLD;

		/* 重复写入不改变 PC，不再次启动 CPU。 */
		if (reset_hold != s->cp_reset_hold) {
			s->cp_set_state(s->cp_opaque, reset_hold, s->cp_reset_vector);
			s->cp_reset_hold = reset_hold;
			s->cp_started |= !reset_hold;
		}
		break;
	}
	case GPGPU_REG_CP_RESET_VECTOR:
		if (!s->cp_reset_hold || (val & 1) || !gpgpu_vram_contains(s, val, 2)) {
			gpgpu_set_error(s, GPGPU_ERR_INVALID_CMD);
			return;
		}
		s->cp_set_state(s->cp_opaque, true, val);
		s->cp_reset_vector = val;
		break;
	default:
		break;
	}
}
