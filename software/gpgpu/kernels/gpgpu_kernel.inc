# SPDX-License-Identifier: GPL-2.0-or-later
# 设备侧内置变量窗口；a0 保存打包参数块在 VRAM 中的偏移。
.equ GPGPU_CTRL_HI, 0x80000
.equ THREAD_X, 0x00
.equ THREAD_Y, 0x04
.equ THREAD_Z, 0x08
.equ BLOCK_X, 0x10
.equ BLOCK_Y, 0x14
.equ BLOCK_Z, 0x18
.equ BLOCK_DIM_X, 0x20
.equ BLOCK_DIM_Y, 0x24
.equ BLOCK_DIM_Z, 0x28
.equ GRID_DIM_X, 0x30
.equ GRID_DIM_Y, 0x34
.equ GRID_DIM_Z, 0x38
