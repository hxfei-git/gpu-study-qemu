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
    }
}

/* Execute the integer kernel instructions used by experiment 8. */
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
