/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * ARM64 virt 上真实 GPGPU 的 SMMUv3 DMA 翻译与错误恢复。
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/generic-pcihost.h"
#include "libqos/qos-smmuv3.h"
#include "hw/arm/smmuv3-common.h"
#include "hw/gpgpu/gpgpu_regs.h"
#include "hw/pci/pci_regs.h"

#define DMA_LEN 64
#define VRAM_OFFSET 0x2000
#define DMA_GPA (QSMMU_SPACE_OFFS_NS + QSMMU_L3_PTE_VAL + (QSMMU_IOVA & 0xfff))
#define IRQ_OFFSET(vector) (0x100 + (vector) * 4)

static void save_device(QPCIDevice *dev, int devfn, void *opaque)
{
    *(QPCIDevice **)opaque = dev;
}

static void submit_dma(QPCIDevice *dev, uint64_t iova, bool d2h, bool fault)
{
    QTestState *qts = dev->bus->qts;
    QPCIBar bar = dev->msix_table_bar;
    uint64_t src = d2h ? VRAM_OFFSET : iova;
    uint64_t dst = d2h ? iova : VRAM_OFFSET;

    qpci_io_writel(dev, bar, GPGPU_REG_IRQ_ACK, UINT32_MAX);
    for (int vector = 1; vector <= 2; vector++) {
        qtest_writel(qts, DMA_GPA + IRQ_OFFSET(vector), 0);
    }
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_SRC_LO, src);
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_SRC_HI, src >> 32);
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_DST_LO, dst);
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_DST_HI, dst >> 32);
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_SIZE, DMA_LEN);
    qpci_io_writel(dev, bar, GPGPU_REG_DMA_CTRL,
                   GPGPU_DMA_START | GPGPU_DMA_IRQ_ENABLE |
                   (d2h ? GPGPU_DMA_DIR_FROM_VRAM : 0));

    /* 错误后也推进时钟，防止误报延迟的完成通知。 */
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qpci_io_readl(dev, bar, GPGPU_REG_DMA_STATUS), ==,
                    fault ? GPGPU_DMA_ERROR : GPGPU_DMA_COMPLETE);
    g_assert_cmphex(qpci_io_readl(dev, bar, GPGPU_REG_ERROR_STATUS), ==,
                    fault ? GPGPU_ERR_DMA_FAULT : 0);
    g_assert_cmphex(qpci_io_readl(dev, bar, GPGPU_REG_IRQ_STATUS), ==,
                    fault ? GPGPU_IRQ_ERROR : GPGPU_IRQ_DMA_DONE);
    g_assert_cmpuint(qtest_readl(qts, DMA_GPA + IRQ_OFFSET(1)), ==, !fault);
    g_assert_cmpuint(qtest_readl(qts, DMA_GPA + IRQ_OFFSET(2)), ==, fault);
}

static void run_dma(const void *opaque)
{
    bool fault = GPOINTER_TO_INT(opaque);
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3,iommu=smmuv3,"
        "default-bus-bypass-iommu=off "
        "-cpu max -m 512M -nodefaults -device gpgpu");
    QGenericPCIBus gbus;
    QPCIDevice *dev = NULL;
    QPCIBar bar;
    uint32_t events = 0;
    uint8_t input[DMA_LEN], output[DMA_LEN];

    qpci_init_generic(&gbus, qts, NULL, false);
    qpci_device_foreach(&gbus.bus, 0x1234, 0x1337, save_device, &dev);
    g_assert_nonnull(dev);
    qpci_device_enable(dev);
    qpci_msix_enable(dev);
    bar = dev->msix_table_bar;

    /* 复用固定 S1 映射，IOVA 不等于物理地址。 */
    g_assert_cmphex(QSMMU_IOVA, !=, DMA_GPA);
    g_assert_cmpint(qsmmu_build_translation(qts, QSMMU_TM_S1_ONLY,
                    QSMMU_SPACE_NONSECURE, dev->devfn), ==, 0);
    qsmmu_program_regs(qts, VIRT_SMMU_BASE, QSMMU_SPACE_NONSECURE);

    /* MSI-X 的完成与错误通知槽使用同一合法映射。 */
    for (int vector = 1; vector <= 2; vector++) {
        uint64_t address = QSMMU_IOVA + IRQ_OFFSET(vector);
        uint64_t entry = dev->msix_table_off + vector * PCI_MSIX_ENTRY_SIZE;

        qpci_io_writel(dev, bar, entry + PCI_MSIX_ENTRY_LOWER_ADDR, address);
        qpci_io_writel(dev, bar, entry + PCI_MSIX_ENTRY_UPPER_ADDR,
                       address >> 32);
        qpci_io_writel(dev, bar, entry + PCI_MSIX_ENTRY_DATA, 1);
        qpci_io_writel(dev, bar, entry + PCI_MSIX_ENTRY_VECTOR_CTRL, 0);
    }
    qpci_io_writel(dev, bar, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
    qpci_io_writel(dev, bar, GPGPU_REG_IRQ_ENABLE,
                   GPGPU_IRQ_DMA_DONE | GPGPU_IRQ_ERROR);
    for (size_t i = 0; i < sizeof(input); i++) {
        input[i] = i * 3 + 1;
    }
    qtest_memwrite(qts, DMA_GPA, input, sizeof(input));
    submit_dma(dev, QSMMU_IOVA, false, false);
    qtest_memset(qts, DMA_GPA, 0, sizeof(output));
    g_assert_cmpuint(qtest_readl(qts, VIRT_SMMU_BASE + A_EVENTQ_PROD), ==, 0);

    if (fault) {
        /* 相邻页未映射；恢复不复位或修改页表。 */
        submit_dma(dev, QSMMU_IOVA + 0x1000, true, true);
        events = qtest_readl(qts, VIRT_SMMU_BASE + A_EVENTQ_PROD);
        g_assert_cmpuint(events, >, 0);
        g_assert_cmpuint(events, <, 1024);
        /* 写入可被拆分，只允许目标范围的翻译 fault。 */
        for (uint32_t i = 0; i < events; i++) {
            uint64_t event = QSMMU_SPACE_OFFS_NS + QSMMU_EVENTQ_BASE_ADDR +
                             i * 32;
            uint64_t address = qtest_readq(qts, event + 16);

            g_assert_cmphex(qtest_readl(qts, event) & 0xff, ==, 0x10);
            g_assert_cmphex(qtest_readl(qts, event + 4), ==, dev->devfn);
            g_assert_cmphex(address, >=, QSMMU_IOVA + 0x1000);
            g_assert_cmphex(address, <, QSMMU_IOVA + 0x1000 + DMA_LEN);
        }
        qpci_io_writel(dev, bar, GPGPU_REG_ERROR_STATUS, GPGPU_ERR_DMA_FAULT);
    }
    submit_dma(dev, QSMMU_IOVA, true, false);
    qtest_memread(qts, DMA_GPA, output, sizeof(output));
    g_assert_cmpmem(output, sizeof(output), input, sizeof(input));
    g_assert_cmpuint(qtest_readl(qts, VIRT_SMMU_BASE + A_EVENTQ_PROD), ==,
                     events);
    g_test_message("SMMUv3 expected translation faults: %u", events);

    qpci_msix_disable(dev);
    g_free(dev);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/gpgpu/iommu/dma-roundtrip",
                        GINT_TO_POINTER(false), run_dma);
    qtest_add_data_func("/gpgpu/iommu/fault-recovery",
                        GINT_TO_POINTER(true), run_dma);
    return g_test_run();
}
