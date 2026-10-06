/*
 * QEMU GPGPU - RISC-V SIMT Core Implementation
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "gpgpu.h"
#include "gpgpu_core.h"
#include "qemu/bswap.h"

/* RISC-V and SoftFloat use different rounding-mode encodings. */
static bool gpgpu_core_set_rounding_mode(GPGPULane *lane, uint32_t rm)
{
    FloatRoundMode mode;

    if (rm == 7) {
        rm = (lane->fcsr >> 5) & 7;
    }

    switch (rm) {
    case 0:
        mode = float_round_nearest_even;
        break;
    case 1:
        mode = float_round_to_zero;
        break;
    case 2:
        mode = float_round_down;
        break;
    case 3:
        mode = float_round_up;
        break;
    case 4:
        mode = float_round_ties_away;
        break;
    default:
        return false;
    }

    set_float_rounding_mode(mode, &lane->fp_status);
    set_float_exception_flags(0, &lane->fp_status);
    return true;
}

/* Accumulate IEEE 754 exceptions in the RISC-V fflags bit positions. */
static void gpgpu_core_update_fflags(GPGPULane *lane)
{
    int flags = get_float_exception_flags(&lane->fp_status);

    lane->fcsr |= ((flags & float_flag_inexact) ? 1U << 0 : 0) |
                  ((flags & float_flag_underflow) ? 1U << 1 : 0) |
                  ((flags & float_flag_overflow) ? 1U << 2 : 0) |
                  ((flags & float_flag_divbyzero) ? 1U << 3 : 0) |
                  ((flags & float_flag_invalid) ? 1U << 4 : 0);
}

/* Initialize the active lanes and per-warp identity. */
void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc,
                          uint32_t thread_id_base, const uint32_t block_id[3],
                          uint32_t num_threads,
                          uint32_t warp_id, uint32_t block_id_linear)
{
    memset(warp, 0, sizeof(*warp));

    warp->thread_id_base = thread_id_base;
    warp->warp_id = warp_id;

    for (int i = 0; i < 3; i++) {
        warp->block_id[i] = block_id[i];
    }

    warp->active_mask = num_threads == GPGPU_WARP_SIZE
                        ? UINT32_MAX : (1U << num_threads) - 1;

    for (uint32_t lane_id = 0; lane_id < num_threads; lane_id++) {
        GPGPULane *lane = &warp->lanes[lane_id];

        lane->active = true;
        lane->pc = pc;
        lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id, lane_id);
        set_float_rounding_mode(float_round_nearest_even, &lane->fp_status);
        set_float_detect_tininess(float_tininess_after_rounding,
                                 &lane->fp_status);
        set_default_nan_mode(true, &lane->fp_status);
        set_float_default_nan_pattern(0x40, &lane->fp_status);
    }
}

/* Execute the integer and floating-point kernels used by experiments 8/9. */
int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    uint32_t cycles = 0;

    while (warp->active_mask != 0) {
        if (cycles >= max_cycles) {
            return -1;
        }

        for (int lane_id = 0; lane_id < GPGPU_WARP_SIZE; lane_id++) {
            GPGPULane *lane = &warp->lanes[lane_id];

            if (lane->active != true) {
                continue;
            }

            if ((lane->pc & 3) || s->vram_size < 4 ||
                lane->pc > s->vram_size - 4) {
                return -1;
            }

            uint32_t inst = ldl_le_p(s->vram_ptr + lane->pc);

            uint32_t opcode = inst & 0x7F;
            uint32_t rd = (inst >> 7) & 0x1F;
            uint32_t funct3 = (inst >> 12) & 0x7;
            uint32_t rs1 = (inst >> 15) & 0x1F;
            uint32_t rs2 = (inst >> 20) & 0x1F;
            uint32_t funct7 = inst >> 25;
            uint32_t csr = inst >> 20;

            switch (opcode) {
            case 0x73:  /* SYSTEM */
                switch (funct3) {
                case 0x0:  /* EBREAK */
                    if (inst != 0x00100073) {
                        return -1;
                    }
                    lane->active = false;
                    warp->active_mask &= ~(1U << lane_id);
                    break;

                case 0x2:  /* CSRRS */
                    /* mhartid is read-only. */
                    if (csr != CSR_MHARTID || rs1 != 0) {
                        return -1;
                    }
                    if (rd != 0) {
                        lane->gpr[rd] = lane->mhartid;
                    }
                    lane->pc += 4;
                    break;

                default:
                    return -1;
                }
                break;

            case 0x13:  /* OP-IMM */
                switch (funct3) {
                case 0x0: {  /* ADDI */
                    int32_t imm = (int32_t)inst >> 20;

                    if (rd != 0) {
                        lane->gpr[rd] = lane->gpr[rs1] + (uint32_t)imm;
                    }
                    lane->pc += 4;
                    break;
                }

                case 0x1: {  /* SLLI */
                    uint32_t shamt = (inst >> 20) & 0x1F;

                    if (funct7 != 0) {
                        return -1;
                    }
                    if (rd != 0) {
                        lane->gpr[rd] = lane->gpr[rs1] << shamt;
                    }
                    lane->pc += 4;
                    break;
                }

                case 0x7: {  /* ANDI */
                    int32_t imm = (int32_t)inst >> 20;

                    if (rd != 0) {
                        lane->gpr[rd] = lane->gpr[rs1] & (uint32_t)imm;
                    }
                    lane->pc += 4;
                    break;
                }

                default:
                    return -1;
                }
                break;

            case 0x37:  /* LUI */
                if (rd != 0) {
                    lane->gpr[rd] = inst & 0xFFFFF000;
                }
                lane->pc += 4;
                break;

            case 0x33:  /* OP */
                if (funct3 != 0 || funct7 != 0) {
                    return -1;
                }
                if (rd != 0) {
                    lane->gpr[rd] = lane->gpr[rs1] + lane->gpr[rs2];
                }
                lane->pc += 4;
                break;

            case 0x23:  /* STORE */
                switch (funct3) {
                case 0x2: {  /* SW */
                    uint32_t raw_imm = (((inst >> 25) & 0x7F) << 5) |
                                       ((inst >> 7) & 0x1F);
                    int32_t offset = (int32_t)(raw_imm << 20) >> 20;
                    uint32_t addr = lane->gpr[rs1] + (uint32_t)offset;
                    uint32_t value = lane->gpr[rs2];

                    if ((addr & 3) || s->vram_size < 4 ||
                        addr > s->vram_size - 4) {
                        return -1;
                    }
                    stl_le_p(s->vram_ptr + addr, value);
                    lane->pc += 4;
                    break;
                }

                default:
                    return -1;
                }
                break;

            case 0x53:  /* OP-FP */
                switch (funct7) {
                case 0x00:  /* FADD.S */
                case 0x08:  /* FMUL.S */
                    break;
                case 0x60:  /* FCVT.W.S */
                case 0x68:  /* FCVT.S.W */
                    if (rs2 != 0) {
                        return -1;
                    }
                    break;
                default:
                    return -1;
                }

                if (!gpgpu_core_set_rounding_mode(lane, funct3)) {
                    return -1;
                }

                switch (funct7) {
                case 0x00:  /* FADD.S */
                    lane->fpr[rd] = float32_add(lane->fpr[rs1],
                                              lane->fpr[rs2],
                                              &lane->fp_status);
                    break;
                case 0x08:  /* FMUL.S */
                    lane->fpr[rd] = float32_mul(lane->fpr[rs1],
                                              lane->fpr[rs2],
                                              &lane->fp_status);
                    break;
                case 0x60: {  /* FCVT.W.S */
                    int32_t value = float32_to_int32(lane->fpr[rs1],
                                                    &lane->fp_status);

                    if (rd != 0) {
                        lane->gpr[rd] = (uint32_t)value;
                    }
                    break;
                }
                case 0x68:  /* FCVT.S.W */
                    lane->fpr[rd] = int32_to_float32((int32_t)lane->gpr[rs1],
                                                   &lane->fp_status);
                    break;
                }

                gpgpu_core_update_fflags(lane);
                lane->pc += 4;
                break;

            default:
                return -1;
            }
        }

        cycles++;
    }

    return 0;
}

/* Execute the configured grid one block and warp at a time. */
int gpgpu_core_exec_kernel(GPGPUState *s)
{
    const uint32_t max_blocks = 1U << MHARTID_BLOCK_BITS;
    const uint32_t max_threads = GPGPU_WARP_SIZE * (1U << MHARTID_WARP_BITS);
    uint32_t block_count = 1;
    uint32_t threads_per_block = 1;
    uint32_t warp_count;
    uint32_t pc;

    if (s->kernel.kernel_addr > UINT32_MAX ||
        (s->kernel.kernel_addr & 3) ||
        s->vram_size < sizeof(uint32_t) ||
        s->kernel.kernel_addr > s->vram_size - sizeof(uint32_t)) {
        return -1;
    }

    /* Keep block and warp IDs within their mhartid fields. */
    for (size_t i = 0; i < 3; i++) {
        uint32_t grid_dim = s->kernel.grid_dim[i];
        uint32_t block_dim = s->kernel.block_dim[i];

        if (!grid_dim || !block_dim ||
            grid_dim > max_blocks / block_count ||
            block_dim > max_threads / threads_per_block) {
            return -1;
        }
        block_count *= grid_dim;
        threads_per_block *= block_dim;
    }

    pc = (uint32_t)s->kernel.kernel_addr;
    warp_count = threads_per_block / GPGPU_WARP_SIZE +
                 (threads_per_block % GPGPU_WARP_SIZE != 0);

    for (uint32_t bz = 0; bz < s->kernel.grid_dim[2]; bz++) {
        for (uint32_t by = 0; by < s->kernel.grid_dim[1]; by++) {
            for (uint32_t bx = 0; bx < s->kernel.grid_dim[0]; bx++) {
                uint32_t block_id_linear =
                    bx + by * s->kernel.grid_dim[0] +
                    bz * s->kernel.grid_dim[0] * s->kernel.grid_dim[1];
                uint32_t block_id[3] = {bx, by, bz};

                for (uint32_t warp_id = 0; warp_id < warp_count; warp_id++) {
                    uint32_t thread_id_base = warp_id * GPGPU_WARP_SIZE;
                    uint32_t num_threads =
                        MIN(GPGPU_WARP_SIZE,
                            threads_per_block - thread_id_base);
                    GPGPUWarp warp;

                    gpgpu_core_init_warp(&warp, pc,
                                        thread_id_base, block_id,
                                        num_threads, warp_id, block_id_linear);

                    if (gpgpu_core_exec_warp(s, &warp, 100000) < 0) {
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}
