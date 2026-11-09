/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GPGPU QTest：共享协议与 virt 的高位 VRAM BAR 映射
 *
 * 寄存器和容量取自共享协议。两个入口使用 ARM virt
 * 默认高位 PCI MMIO 窗口映射 BAR2；其他 BAR 交给 libqos。
 */
#ifndef TESTS_QTEST_GPGPU_TEST_H
#define TESTS_QTEST_GPGPU_TEST_H

#include "libqtest.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"
#include "gpgpu/gpgpu_protocol.h"

/* 固定 VRAM 容量，单位字节。 */
#define GPGPU_TEST_VRAM_SIZE GPGPU_VRAM_SIZE
/* virt 高位 PCI MMIO 起点：0x80_0000_0000，按 BAR2 容量对齐。 */
#define GPGPU_TEST_VRAM_BAR_BASE UINT64_C(0x8000000000)

/* 2 GiB BAR 改走高位窗口，同时写入 BAR 的高、低半部。 */
static inline QPCIBar gpgpu_test_map_vram(QPCIDevice *dev)
{
    qpci_config_writel(dev, PCI_BASE_ADDRESS_2,
                      (uint32_t)GPGPU_TEST_VRAM_BAR_BASE);
    qpci_config_writel(dev, PCI_BASE_ADDRESS_3,
                      GPGPU_TEST_VRAM_BAR_BASE >> 32);
    return (QPCIBar) {
        .addr = GPGPU_TEST_VRAM_BAR_BASE,
        .is_io = false,
    };
}

/*
 * libqos 通用回调把 MMIO 地址截成 32 位。
 * GPGPU 的 BAR 均为内存 BAR，改用 QTest 的 64 位物理接口，
 * 不改变通用 libqos。
 */
static inline uint16_t gpgpu_test_readw(QPCIDevice *dev, QPCIBar bar,
                                       uint64_t offset)
{
    return qtest_readw(dev->bus->qts, bar.addr + offset);
}

static inline uint32_t gpgpu_test_readl(QPCIDevice *dev, QPCIBar bar,
                                       uint64_t offset)
{
    return qtest_readl(dev->bus->qts, bar.addr + offset);
}

static inline uint64_t gpgpu_test_readq(QPCIDevice *dev, QPCIBar bar,
                                       uint64_t offset)
{
    return qtest_readq(dev->bus->qts, bar.addr + offset);
}

static inline void gpgpu_test_writel(QPCIDevice *dev, QPCIBar bar,
                                     uint64_t offset, uint32_t value)
{
    qtest_writel(dev->bus->qts, bar.addr + offset, value);
}

static inline void gpgpu_test_writeq(QPCIDevice *dev, QPCIBar bar,
                                     uint64_t offset, uint64_t value)
{
    qtest_writeq(dev->bus->qts, bar.addr + offset, value);
}

static inline void gpgpu_test_memread(QPCIDevice *dev, QPCIBar bar,
                                      uint64_t offset, void *buf, size_t size)
{
    qtest_memread(dev->bus->qts, bar.addr + offset, buf, size);
}

static inline void gpgpu_test_memwrite(QPCIDevice *dev, QPCIBar bar,
                                       uint64_t offset, const void *buf,
                                       size_t size)
{
    qtest_memwrite(dev->bus->qts, bar.addr + offset, buf, size);
}

#endif /* TESTS_QTEST_GPGPU_TEST_H */
