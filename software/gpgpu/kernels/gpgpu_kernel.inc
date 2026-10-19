# SPDX-License-Identifier: GPL-2.0-or-later
# 设备侧内置变量窗口；同步模式的 a0 为参数 VRAM 偏移，
# 队列模式的 a0 指向只读 Host 参数窗口。
# 供 LUI 装载的控制窗口地址高 20 位。
.equ GPGPU_CTRL_HI, 0x80000
# X 维线程在工作组内的索引在控制窗口内的字节偏移。
.equ THREAD_X, 0x00
# Y 维线程在工作组内的索引在控制窗口内的字节偏移。
.equ THREAD_Y, 0x04
# Z 维线程在工作组内的索引在控制窗口内的字节偏移。
.equ THREAD_Z, 0x08
# X 维工作组在网格内的索引在控制窗口内的字节偏移。
.equ BLOCK_X, 0x10
# Y 维工作组在网格内的索引在控制窗口内的字节偏移。
.equ BLOCK_Y, 0x14
# Z 维工作组在网格内的索引在控制窗口内的字节偏移。
.equ BLOCK_Z, 0x18
# X 维工作组线程数在控制窗口内的字节偏移。
.equ BLOCK_DIM_X, 0x20
# Y 维工作组线程数在控制窗口内的字节偏移。
.equ BLOCK_DIM_Y, 0x24
# Z 维工作组线程数在控制窗口内的字节偏移。
.equ BLOCK_DIM_Z, 0x28
# X 维网格工作组数在控制窗口内的字节偏移。
.equ GRID_DIM_X, 0x30
# Y 维网格工作组数在控制窗口内的字节偏移。
.equ GRID_DIM_Y, 0x34
# Z 维网格工作组数在控制窗口内的字节偏移。
.equ GRID_DIM_Z, 0x38
