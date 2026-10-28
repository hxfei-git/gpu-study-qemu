# SPDX-License-Identifier: GPL-2.0-or-later
# 启动状态通过只读 CSR 获取；a0 保存参数的 GPUVA 或 GPU 物理地址。
.equ THREAD_ID_X, 0xCC0
.equ THREAD_ID_Y, 0xCC1
.equ THREAD_ID_Z, 0xCC2
.equ BLOCK_ID_X, 0xCC3
.equ BLOCK_ID_Y, 0xCC4
.equ BLOCK_ID_Z, 0xCC5
.equ BLOCK_DIM_X, 0xCC6
.equ BLOCK_DIM_Y, 0xCC7
.equ BLOCK_DIM_Z, 0xCC8
.equ GRID_DIM_X, 0xCC9
.equ GRID_DIM_Y, 0xCCA
.equ GRID_DIM_Z, 0xCCB
