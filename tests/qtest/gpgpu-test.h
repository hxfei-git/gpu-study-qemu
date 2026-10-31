/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU QTest 共用的协议常量。
 *
 * 寄存器和内存布局直接取自设备与软件栈共用的协议头，测试不另抄一份数值。
 */
#ifndef TESTS_QTEST_GPGPU_TEST_H
#define TESTS_QTEST_GPGPU_TEST_H

#include "gpgpu/gpgpu_protocol.h"

/* 设备默认 VRAM 容量；VRAM 末端到 Outbound 起点之间是不可路由的空洞。 */
#define GPGPU_TEST_VRAM_SIZE 0x04000000ULL

#endif /* TESTS_QTEST_GPGPU_TEST_H */
