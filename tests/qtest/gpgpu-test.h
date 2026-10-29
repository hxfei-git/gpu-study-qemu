/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU QTest 共用的协议常量。
 *
 * 寄存器和内存布局直接取自设备与软件栈共用的协议头，测试不另抄一份数值。
 */
#ifndef TESTS_QTEST_GPGPU_TEST_H
#define TESTS_QTEST_GPGPU_TEST_H

#include "hw/gpgpu/gpgpu_regs.h"
#include "software/gpgpu/include/gpgpu_queue.h"

/* 教学设备的 PCI 厂商与设备编号。 */
#define GPGPU_TEST_VENDOR 0x1234
#define GPGPU_TEST_DEVICE 0x1337
/* 设备默认 VRAM 容量，也是 Outbound 窗口的 GPU 物理起点。 */
#define GPGPU_TEST_VRAM_SIZE 0x04000000ULL

#endif /* TESTS_QTEST_GPGPU_TEST_H */
