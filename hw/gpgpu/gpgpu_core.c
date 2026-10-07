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

/* Convert FP32 to saturating E2M1 using ordered positive FP32 encodings. */
static float4_e2m1 gpgpu_core_float32_to_e2m1(float32 value,
                                              float_status *status)
{
    static const uint32_t values[] = {
        0x00000000, 0x3F000000, 0x3F800000, 0x3FC00000,
        0x40000000, 0x40400000, 0x40800000, 0x40C00000,
        0x41000000, /* Virtual next value, 8.0, for overflow rounding. */
    };
    static const uint32_t midpoints[] = {
        0x3E800000, 0x3F400000, 0x3FA00000, 0x3FE00000,
        0x40200000, 0x40600000, 0x40A00000, 0x40E00000,
    };
    uint32_t magnitude = value & 0x7FFFFFFF;
    uint32_t sign = (value >> 28) & 8;
    uint32_t upper = 0;
    uint32_t result;
    uint16_t flags = float_flag_inexact;
    bool round_up;

    /* E2M1 has no NaN or infinity encodings; preserve the input sign. */
    if (magnitude >= 0x7F800000) {
        if (magnitude > 0x7F800000) {
            float_raise(float_flag_invalid, status);
        }
        return sign | 7;
    }
    if (magnitude >= values[8]) {
        float_raise(float_flag_overflow | float_flag_inexact, status);
        return sign | 7;
    }

    while (magnitude > values[upper]) {
        upper++;
    }
    if (magnitude == values[upper]) {
        return sign | upper;
    }

    switch (get_float_rounding_mode(status)) {
    case float_round_nearest_even:
        round_up = magnitude > midpoints[upper - 1] ||
                   (magnitude == midpoints[upper - 1] && !(upper & 1));
        break;
    case float_round_ties_away:
        round_up = magnitude >= midpoints[upper - 1];
        break;
    case float_round_to_zero:
        round_up = false;
        break;
    case float_round_down:
        round_up = sign != 0;
        break;
    case float_round_up:
        round_up = sign == 0;
        break;
    default:
        g_assert_not_reached();
    }

    result = upper - 1 + round_up;
    if (result > 7) {
        result = 7;
        flags |= float_flag_overflow;
    } else if (result < 2) {
        /* Detect tininess after rounding, as for the other FP formats. */
        flags |= float_flag_underflow;
    }
    float_raise(flags, status);
    return sign | result;
}

static bool gpgpu_core_word_valid(GPGPUState *s, uint32_t addr)
{
    return !(addr & 3) && s->vram_size >= 4 && addr <= s->vram_size - 4;
}

/* The CTRL window belongs to GPU threads and is separate from PCI BAR0. */
static bool gpgpu_core_load_word(GPGPUState *s, GPGPUWarp *warp,
                                 uint32_t lane_id, uint32_t addr,
                                 uint32_t *value)
{
    uint32_t linear_thread = warp->thread_id_base + lane_id;

    if (gpgpu_core_word_valid(s, addr)) {
        *value = ldl_le_p(s->vram_ptr + addr);
        return true;
    }

    switch (addr) {
    case GPGPU_CORE_CTRL_THREAD_ID_X:
        *value = linear_thread % s->kernel.block_dim[0];
        break;
    case GPGPU_CORE_CTRL_THREAD_ID_Y:
        *value = linear_thread / s->kernel.block_dim[0] %
                 s->kernel.block_dim[1];
        break;
    case GPGPU_CORE_CTRL_THREAD_ID_Z:
        *value = linear_thread / s->kernel.block_dim[0] /
                 s->kernel.block_dim[1];
        break;
    case GPGPU_CORE_CTRL_BLOCK_ID_X:
    case GPGPU_CORE_CTRL_BLOCK_ID_Y:
    case GPGPU_CORE_CTRL_BLOCK_ID_Z:
        *value = warp->block_id[(addr - GPGPU_CORE_CTRL_BLOCK_ID_X) / 4];
        break;
    case GPGPU_CORE_CTRL_BLOCK_DIM_X:
    case GPGPU_CORE_CTRL_BLOCK_DIM_Y:
    case GPGPU_CORE_CTRL_BLOCK_DIM_Z:
        *value = s->kernel.block_dim[(addr - GPGPU_CORE_CTRL_BLOCK_DIM_X) / 4];
        break;
    case GPGPU_CORE_CTRL_GRID_DIM_X:
    case GPGPU_CORE_CTRL_GRID_DIM_Y:
    case GPGPU_CORE_CTRL_GRID_DIM_Z:
        *value = s->kernel.grid_dim[(addr - GPGPU_CORE_CTRL_GRID_DIM_X) / 4];
        break;
    default:
        return false;
    }

    return true;
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

/* Execute the integer and floating-point kernels used by experiments 8-10. */
int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp,
                         uint32_t *insn_budget)
{
    while (warp->active_mask != 0) {
        for (int lane_id = 0; lane_id < GPGPU_WARP_SIZE; lane_id++) {
            GPGPULane *lane = &warp->lanes[lane_id];

            if (lane->active != true) {
                continue;
            }

            if (!*insn_budget || !gpgpu_core_word_valid(s, lane->pc)) {
                return -1;
            }
            (*insn_budget)--;

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

            case 0x33: {  /* ADD / SUB / MUL */
                uint32_t value;

                if (funct3 != 0) {
                    return -1;
                }
                switch (funct7) {
                case 0x00:
                    value = lane->gpr[rs1] + lane->gpr[rs2];
                    break;
                case 0x20:
                    value = lane->gpr[rs1] - lane->gpr[rs2];
                    break;
                case 0x01:
                    /* The low half is identical for signed/unsigned MUL. */
                    value = lane->gpr[rs1] * lane->gpr[rs2];
                    break;
                default:
                    return -1;
                }
                if (rd != 0) {
                    lane->gpr[rd] = value;
                }
                lane->pc += 4;
                break;
            }

            case 0x03:  /* LW */
            case 0x07: {  /* FLW */
                int32_t offset = (int32_t)inst >> 20;
                uint32_t addr = lane->gpr[rs1] + (uint32_t)offset;
                uint32_t value;

                if (funct3 != 2 ||
                    !gpgpu_core_load_word(s, warp, lane_id, addr, &value)) {
                    return -1;
                }
                if (opcode == 0x07) {
                    lane->fpr[rd] = value;
                } else if (rd != 0) {
                    lane->gpr[rd] = value;
                }
                lane->pc += 4;
                break;
            }

            case 0x63: {  /* Conditional branches; each lane has its own PC. */
                uint32_t raw_imm = ((inst >> 31) << 12) |
                                   (((inst >> 7) & 1) << 11) |
                                   (((inst >> 25) & 0x3F) << 5) |
                                   (((inst >> 8) & 0xF) << 1);
                int32_t offset = (int32_t)(raw_imm << 19) >> 19;
                uint32_t left = lane->gpr[rs1];
                uint32_t right = lane->gpr[rs2];
                bool taken;

                switch (funct3) {
                case 0:
                    taken = left == right;
                    break;
                case 1:
                    taken = left != right;
                    break;
                case 4:
                    taken = (int32_t)left < (int32_t)right;
                    break;
                case 5:
                    taken = (int32_t)left >= (int32_t)right;
                    break;
                case 6:
                    taken = left < right;
                    break;
                case 7:
                    taken = left >= right;
                    break;
                default:
                    return -1;
                }
                lane->pc += taken ? (uint32_t)offset : 4;
                break;
            }

            case 0x6F: {  /* JAL */
                uint32_t raw_imm = ((inst >> 31) << 20) |
                                   (((inst >> 12) & 0xFF) << 12) |
                                   (((inst >> 20) & 1) << 11) |
                                   (((inst >> 21) & 0x3FF) << 1);
                int32_t offset = (int32_t)(raw_imm << 11) >> 11;

                if (rd != 0) {
                    lane->gpr[rd] = lane->pc + 4;
                }
                lane->pc += (uint32_t)offset;
                break;
            }

            case 0x23:  /* STORE */
            case 0x27:  /* FSW */
                switch (funct3) {
                case 0x2: {  /* SW */
                    uint32_t raw_imm = (((inst >> 25) & 0x7F) << 5) |
                                       ((inst >> 7) & 0x1F);
                    int32_t offset = (int32_t)(raw_imm << 20) >> 20;
                    uint32_t addr = lane->gpr[rs1] + (uint32_t)offset;
                    uint32_t value = opcode == 0x27 ? lane->fpr[rs2] :
                                                     lane->gpr[rs2];

                    if (!gpgpu_core_word_valid(s, addr)) {
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
                if (funct7 == 0x78) {  /* FMV.W.X */
                    if (rs2 != 0 || funct3 != 0) {
                        return -1;
                    }
                    lane->fpr[rd] = lane->gpr[rs1];
                    lane->pc += 4;
                    break;
                }

                switch (funct7) {
                case 0x00:  /* FADD.S */
                case 0x08:  /* FMUL.S */
                    break;
                case 0x22:  /* BF16 conversion */
                case 0x26:  /* E2M1 conversion */
                    if (rs2 > 1) {
                        return -1;
                    }
                    break;
                case 0x24:  /* E4M3 / E5M2 conversion */
                    if (rs2 > 3) {
                        return -1;
                    }
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
                case 0x22:  /* BF16 conversion */
                    if (rs2 == 0) {
                        lane->fpr[rd] = bfloat16_to_float32(
                            (bfloat16)lane->fpr[rs1], &lane->fp_status);
                    } else {
                        lane->fpr[rd] = float32_to_bfloat16(
                            lane->fpr[rs1], &lane->fp_status);
                    }
                    break;
                case 0x24:  /* E4M3 / E5M2 conversion */
                    if (rs2 == 0 || rs2 == 2) {
                        bfloat16 value;

                        if (rs2 == 0) {
                            value = float8_e4m3_to_bfloat16(
                                (float8_e4m3)lane->fpr[rs1], &lane->fp_status);
                        } else {
                            value = float8_e5m2_to_bfloat16(
                                (float8_e5m2)lane->fpr[rs1], &lane->fp_status);
                        }
                        lane->fpr[rd] = bfloat16_to_float32(value,
                                                          &lane->fp_status);
                    } else if (rs2 == 1) {
                        float32 value = lane->fpr[rs1];
                        float8_e4m3 result = float32_to_float8_e4m3(
                            value, true, &lane->fp_status);

                        /* E4M3 NaN requires all exponent/fraction bits set. */
                        lane->fpr[rd] = float32_is_any_nan(value)
                                        ? 0x7F : result;
                    } else {
                        /* Saturate finite values while preserving Inf. */
                        lane->fpr[rd] = float32_to_float8_e5m2(
                            lane->fpr[rs1],
                            !float32_is_infinity(lane->fpr[rs1]),
                            &lane->fp_status);
                    }
                    break;
                case 0x26:  /* E2M1 conversion */
                    if (rs2 == 0) {
                        float8_e4m3 value = float4_e2m1_to_float8_e4m3(
                            lane->fpr[rs1] & 0xF, &lane->fp_status);
                        bfloat16 bf16 = float8_e4m3_to_bfloat16(
                            value, &lane->fp_status);

                        lane->fpr[rd] = bfloat16_to_float32(bf16,
                                                          &lane->fp_status);
                    } else {
                        lane->fpr[rd] = gpgpu_core_float32_to_e2m1(
                            lane->fpr[rs1], &lane->fp_status);
                    }
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

    }

    return 0;
}

/* Execute the configured grid one block and warp at a time. */
int gpgpu_core_exec_kernel(GPGPUState *s)
{
    uint32_t block_count = 1;
    uint32_t threads_per_block = 1;
    uint32_t warp_count;
    uint32_t pc;
    uint32_t insn_budget = GPGPU_MAX_KERNEL_INSNS;

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
            grid_dim > GPGPU_MAX_BLOCKS / block_count ||
            block_dim > GPGPU_MAX_BLOCK_THREADS / threads_per_block) {
            return -1;
        }
        block_count *= grid_dim;
        threads_per_block *= block_dim;
    }

    if (block_count > GPGPU_MAX_TOTAL_THREADS / threads_per_block) {
        return -1;
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
                    for (uint32_t i = 0; i < num_threads; i++) {
                        warp.lanes[i].gpr[10] = s->kernel.kernel_args;
                    }

                    if (gpgpu_core_exec_warp(s, &warp, &insn_budget) < 0) {
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}
