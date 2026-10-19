/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * A57 Host 经 PCI 代理装载和启动原生 RV64 CP 固件。
 */

#include "qemu/osdep.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "libqos/qos-smmuv3.h"
#include "hw/arm/smmuv3-common.h"
#include "hw/gpgpu/gpgpu_regs.h"
#include "gpgpu-remote.h"
#include "gpgpu-vfio-user-test.h"

#define FIRMWARE_OFFSET 0x4000
#define RESULT_OFFSET 0x6000
#define TRAP_OFFSET 0x5000
#define PARAM_OFFSET 0x6100
#define DMA_VRAM_OFFSET 0x8000
#define HOST_DATA_OFFSET 0x800
#define HOST_DMA_SRC_OFFSET 0xa00
#define HOST_DMA_DST_OFFSET 0xb00
#define TEST_DMA_SIZE 64

typedef struct CPTest {
    GPGPURemote remote;
    QTestState *host;
    QGenericPCIBus bus;
    QPCIDevice *dev;
    QPCIBar regs;
    QPCIBar vram;
    bool gpuvm;
} CPTest;

static void save_device(QPCIDevice *dev, int devfn, void *opaque)
{
    *(QPCIDevice **)opaque = dev;
}

static void cp_start_mode(CPTest *s, bool smmu)
{
    g_autofree char *device = NULL;
    g_autofree char *args = NULL;

    gpgpu_remote_start_mode(&s->remote, s->gpuvm);
    device = gpgpu_remote_device_option(&s->remote);
    args = g_strdup_printf("-machine virt,gic-version=3%s -cpu cortex-a57 "
                           "-m 512M -nodefaults -device %s",
                           smmu ? ",iommu=smmuv3,"
                                  "default-bus-bypass-iommu=off" : "", device);
    s->host = qtest_init(args);
    qpci_init_generic(&s->bus, s->host, NULL, false);
    qpci_device_foreach(&s->bus.bus, 0x1234, 0x1337, save_device, &s->dev);
    g_assert_nonnull(s->dev);
    qpci_device_enable(s->dev);
    s->regs = qpci_iomap(s->dev, 0, NULL);
    s->vram = qpci_iomap(s->dev, 2, NULL);
}

static void cp_start(CPTest *s)
{
    cp_start_mode(s, false);
}

static void cp_stop(CPTest *s)
{
    qpci_iounmap(s->dev, s->regs);
    qpci_iounmap(s->dev, s->vram);
    g_free(s->dev);
    qtest_quit(s->host);
    gpgpu_remote_stop(&s->remote);
}

static void load_firmware(CPTest *s, uint32_t value)
{
    /* t0 = 结果地址，t1 = 常量；写回一次后执行原生 WFI。 */
    uint32_t firmware[] = {
        0x000062b7,                   /* lui t0, 0x6 */
        (value << 20) | 0x00000313,    /* addi t1, zero, value */
        0x0062a023,                   /* sw t1, 0(t0) */
        0x10500073,                   /* wfi */
        0xffdff06f,                   /* jal zero, -4 */
    };

    g_assert_cmphex(qpci_io_readl(s->dev, s->regs, GPGPU_REG_CP_CTRL), ==,
                    GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD);
    for (size_t i = 0; i < ARRAY_SIZE(firmware); i++) {
        qpci_io_writel(s->dev, s->vram, FIRMWARE_OFFSET + i * 4, firmware[i]);
    }
    qpci_io_writel(s->dev, s->vram, RESULT_OFFSET, 0);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_RESET_VECTOR,
                   FIRMWARE_OFFSET);
    /* 读回入口与代码，确认同步写入完成。 */
    g_assert_cmphex(qpci_io_readl(s->dev, s->regs,
                                GPGPU_REG_CP_RESET_VECTOR), ==,
                    FIRMWARE_OFFSET);
    g_assert_cmphex(qpci_io_readl(s->dev, s->vram, FIRMWARE_OFFSET), ==,
                    firmware[0]);
}

static void test_device_access(void)
{
    CPTest s = {0};

    cp_start(&s);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_DEV_ID), ==,
                    GPGPU_DEV_ID_VALUE);
    qpci_io_writel(s.dev, s.vram, RESULT_OFFSET, 0x12345678);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET), ==,
                    0x12345678);
    cp_stop(&s);
}

static void test_reset_hold(void)
{
    CPTest s = {0};
    QDict *reply, *cpu;
    QList *cpus;

    cp_start(&s);
    reply = qtest_qmp(s.remote.qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_false(qdict_haskey(reply, "error"));
    cpus = qdict_get_qlist(reply, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, 1);
    cpu = qobject_to(QDict, qlist_entry_obj(qlist_first(cpus)));
    g_assert_cmpstr(qdict_get_str(cpu, "target"), ==, "riscv64");
    qobject_unref(reply);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_CP_RESET_VECTOR),
                    ==, 0);
    load_firmware(&s, 0x123);
    /* B 使用原生 TCG，等待真实时间确认仍未取指。 */
    g_usleep(100000);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET), ==, 0);
    cp_stop(&s);
}

static void run_firmware(CPTest *s, uint32_t value)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    uint32_t result;

    load_firmware(s, value);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_CTRL, 0);
    g_assert_cmphex(qpci_io_readl(s->dev, s->regs, GPGPU_REG_CP_CTRL), ==,
                    GPGPU_CP_PRESENT);
    do {
        result = qpci_io_readl(s->dev, s->vram, RESULT_OFFSET);
        if (result == value) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmphex(result, ==, value);
}

static void test_firmware_replace(void)
{
    CPTest s = {0};

    cp_start(&s);
    run_firmware(&s, 0x123);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
    /* 同一入口替换指令，结果必须来自新固件。 */
    run_firmware(&s, 0x456);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
    cp_stop(&s);
}

/* RV64 指令仍按 32 位指令字编码，无需外部交叉编译器。 */
typedef struct WindowFirmware {
    uint32_t insns[256];
    size_t count;
} WindowFirmware;

static void fw_emit(WindowFirmware *fw, uint32_t insn)
{
    g_assert_cmpuint(fw->count, <, ARRAY_SIZE(fw->insns));
    fw->insns[fw->count++] = insn;
}

static uint32_t rv_i(uint32_t opcode, unsigned funct3, unsigned rd,
                     unsigned rs1, int imm)
{
    return ((uint32_t)imm & 0xfff) << 20 | rs1 << 15 | funct3 << 12 |
           rd << 7 | opcode;
}

static uint32_t rv_store(unsigned funct3, unsigned rs2, unsigned rs1, int imm)
{
    unsigned value = imm & 0xfff;

    return (value >> 5) << 25 | rs2 << 20 | rs1 << 15 | funct3 << 12 |
           (value & 31) << 7 | 0x23;
}

static uint32_t rv_branch(unsigned rs1, unsigned rs2, int offset)
{
    unsigned value = offset & 0x1fff;

    return (value >> 12) << 31 | ((value >> 5) & 63) << 25 |
           rs2 << 20 | rs1 << 15 | ((value >> 1) & 15) << 8 |
           ((value >> 11) & 1) << 7 | 0x63;
}

static void fw_li(WindowFirmware *fw, unsigned rd, uint32_t value)
{
    fw_emit(fw, ((value + 0x800) & 0xfffff000) | rd << 7 | 0x37);
    fw_emit(fw, rv_i(0x13, 0, rd, rd, value & 0xfff));
    /* 清除 LUI/ADDI 的符号扩展，保留低 32 位。 */
    fw_emit(fw, rv_i(0x13, 1, rd, rd, 32)); /* slli */
    fw_emit(fw, rv_i(0x13, 5, rd, rd, 32)); /* srli */
}

static void fw_finish(WindowFirmware *fw)
{
    fw_emit(fw, rv_i(0x13, 0, 5, 0, 1));
    fw_emit(fw, rv_store(2, 5, 10, 0));
    fw_emit(fw, 0x10500073); /* wfi */
    fw_emit(fw, 0xffdff06f); /* jal zero, -4 */
}

static void fw_window_setup(WindowFirmware *fw)
{
    fw_li(fw, 10, RESULT_OFFSET);
    fw_li(fw, 11, PARAM_OFFSET);
    fw_li(fw, 6, GPGPU_CP_IOVA_BASE_LO);
    fw_emit(fw, rv_i(0x03, 2, 5, 11, 0));
    fw_emit(fw, rv_store(2, 5, 6, 0));
    fw_emit(fw, rv_i(0x03, 2, 5, 11, 4));
    fw_emit(fw, rv_store(2, 5, 6, 4));
    fw_emit(fw, rv_i(0x03, 6, 7, 11, 8)); /* lwu：本地窗口地址 */
}

static void fw_load(CPTest *s, const WindowFirmware *fw, uint32_t offset)
{
    for (size_t i = 0; i < fw->count; i++) {
        qpci_io_writel(s->dev, s->vram, offset + i * 4, fw->insns[i]);
    }
}

static void fw_start(CPTest *s, const WindowFirmware *fw)
{
    g_assert_cmphex(qpci_io_readl(s->dev, s->regs, GPGPU_REG_CP_CTRL), ==,
                   GPGPU_CP_PRESENT | GPGPU_CP_RESET_HOLD);
    fw_load(s, fw, FIRMWARE_OFFSET);
    for (unsigned i = 0; i < 64; i += 4) {
        qpci_io_writel(s->dev, s->vram, RESULT_OFFSET + i, 0);
    }
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_RESET_VECTOR,
                   FIRMWARE_OFFSET);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_CTRL, 0);
}

static void fw_wait(CPTest *s, unsigned offset)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    uint32_t value;

    do {
        value = qpci_io_readl(s->dev, s->vram, RESULT_OFFSET + offset);
        if (value) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmphex(value, !=, 0);
}

static void window_memory(CPTest *s, bool smmu, uint64_t *iova, uint64_t *gpa)
{
    if (smmu) {
        *iova = QSMMU_IOVA & ~0xfffULL;
        *gpa = QSMMU_SPACE_OFFS_NS + QSMMU_L3_PTE_VAL;
        g_assert_cmphex(*iova, >, UINT32_MAX);
        g_assert_cmphex(*iova, !=, *gpa);
        g_assert_cmpint(qsmmu_build_translation(s->host, QSMMU_TM_S1_ONLY,
                        QSMMU_SPACE_NONSECURE, s->dev->devfn), ==, 0);
        qsmmu_program_regs(s->host, VIRT_SMMU_BASE, QSMMU_SPACE_NONSECURE);
    } else {
        *iova = *gpa = 0x41000000;
    }
    /* 故意置位低 16 位，固件读回应只保留对齐后的基址。 */
    qpci_io_writel(s->dev, s->vram, PARAM_OFFSET, *iova | 0xffff);
    qpci_io_writel(s->dev, s->vram, PARAM_OFFSET + 4, *iova >> 32);
    qpci_io_writel(s->dev, s->vram, PARAM_OFFSET + 8,
                   GPGPU_CP_HOST_WINDOW_BASE + (*iova & 0xffff) +
                   HOST_DATA_OFFSET);
}

static void check_base_reset(CPTest *s, const WindowFirmware *access_fw)
{
    WindowFirmware read_fw = {0};

    fw_li(&read_fw, 10, RESULT_OFFSET);
    fw_li(&read_fw, 6, GPGPU_CP_IOVA_BASE_LO);
    fw_emit(&read_fw, rv_i(0x03, 2, 5, 6, 0));
    fw_emit(&read_fw, rv_store(2, 5, 10, 4));
    fw_emit(&read_fw, rv_i(0x03, 2, 5, 6, 4));
    fw_emit(&read_fw, rv_store(2, 5, 10, 8));
    fw_finish(&read_fw);

    for (unsigned global = 0; global < 2; global++) {
        if (global) {
            /* 重新配置非零基址，再检查全局复位。 */
            qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_CTRL,
                           GPGPU_CP_RESET_HOLD);
            fw_start(s, access_fw);
            fw_wait(s, 0);
        }
        qpci_io_writel(s->dev, s->regs,
                       global ? GPGPU_REG_GLOBAL_CTRL : GPGPU_REG_CP_CTRL,
                       global ? GPGPU_CTRL_RESET : GPGPU_CP_RESET_HOLD);
        fw_start(s, &read_fw);
        fw_wait(s, 0);
        g_assert_cmphex(qpci_io_readl(s->dev, s->vram, RESULT_OFFSET + 4),
                       ==, 0);
        g_assert_cmphex(qpci_io_readl(s->dev, s->vram, RESULT_OFFSET + 8),
                       ==, 0);
    }
}

static void test_window_access(const void *opaque)
{
    bool smmu = GPOINTER_TO_INT(opaque);
    CPTest s = {0};
    WindowFirmware fw = {0};
    uint64_t iova, gpa;
    uint8_t input[32] = {
        0x80, 0x99, 0x34, 0x92, 0x78, 0x56, 0x34, 0x12,
        [16] = 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0xf1,
    };
    uint8_t output[32], expected[32];

    cp_start_mode(&s, smmu);
    window_memory(&s, smmu, &iova, &gpa);
    qtest_memwrite(s.host, gpa + HOST_DATA_OFFSET, input, sizeof(input));
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_GLOBAL_CTRL), ==, 0);
    fw_window_setup(&fw);
    fw_emit(&fw, rv_i(0x03, 2, 5, 6, 0));
    fw_emit(&fw, rv_store(2, 5, 10, 4));
    fw_emit(&fw, rv_i(0x03, 2, 5, 6, 4));
    fw_emit(&fw, rv_store(2, 5, 10, 8));
    /* lbu/sb、lhu/sh、lw/sw 分别验证 1、2、4 字节访存。 */
    fw_emit(&fw, rv_i(0x03, 4, 5, 7, 0));
    fw_emit(&fw, rv_store(0, 5, 7, 8));
    fw_emit(&fw, rv_store(2, 5, 10, 12));
    fw_emit(&fw, rv_i(0x03, 5, 5, 7, 2));
    fw_emit(&fw, rv_store(1, 5, 7, 10));
    fw_emit(&fw, rv_store(2, 5, 10, 16));
    fw_emit(&fw, rv_i(0x03, 2, 5, 7, 4));
    fw_emit(&fw, rv_store(2, 5, 7, 12));
    fw_emit(&fw, rv_store(2, 5, 10, 20));
    /* ld/sd 验证高半部与最高位均非零的完整 64 位值。 */
    fw_emit(&fw, rv_i(0x03, 3, 5, 7, 16));
    fw_emit(&fw, rv_store(3, 5, 7, 24));
    fw_emit(&fw, rv_store(3, 5, 10, 24));
    fw_finish(&fw);
    fw_start(&s, &fw);
    fw_wait(&s, 0);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 4), ==,
                   (uint32_t)iova & 0xffff0000);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 8), ==,
                   iova >> 32);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 12), ==, 0x80);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 16), ==,
                   0x9234);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 20), ==,
                   0x12345678);
    g_assert_cmphex(qpci_io_readq(s.dev, s.vram, RESULT_OFFSET + 24), ==,
                   UINT64_C(0xf123456789abcdef));
    memcpy(expected, input, sizeof(input));
    expected[8] = input[0];
    memcpy(expected + 10, input + 2, 6);
    memcpy(expected + 24, input + 16, 8);
    qtest_memread(s.host, gpa + HOST_DATA_OFFSET, output, sizeof(output));
    g_assert_cmpmem(output, sizeof(output), expected, sizeof(expected));
    if (smmu) {
        g_assert_cmpuint(qtest_readl(s.host, VIRT_SMMU_BASE + A_EVENTQ_PROD),
                         ==, 0);
    }
    check_base_reset(&s, &fw);
    cp_stop(&s);
}

static void test_window_fault_recovery(void)
{
    CPTest s = {0};
    uint64_t iova, gpa;
    uint32_t bad_address;
    uint32_t events;
    /* 未映射的 8 字节请求产生两条 SMMU 记录。 */
    const unsigned fault_offsets[] = {0, 0, 0, 4, 0, 4};
    WindowFirmware trap = {0};

    cp_start_mode(&s, true);
    window_memory(&s, true, &iova, &gpa);
    bad_address = GPGPU_CP_HOST_WINDOW_BASE + (iova & 0xffff) + 0x1000;
    /* t3/t4 专供异常处理；a0 始终指向 VRAM 结果区。 */
    fw_emit(&trap, rv_i(0x73, 2, 28, 0, 0x342)); /* csrr mcause */
    fw_emit(&trap, rv_store(3, 28, 10, 8));
    fw_emit(&trap, rv_i(0x73, 2, 28, 0, 0x343)); /* csrr mtval */
    fw_emit(&trap, rv_store(3, 28, 10, 16));
    fw_emit(&trap, rv_i(0x73, 2, 29, 0, 0x341)); /* csrr mepc */
    fw_emit(&trap, rv_store(3, 29, 10, 24));
    fw_emit(&trap, rv_i(0x13, 0, 29, 29, 4));
    fw_emit(&trap, rv_i(0x73, 1, 0, 29, 0x341)); /* csrw mepc */
    fw_emit(&trap, 0x30200073);                  /* mret */
    fw_load(&s, &trap, TRAP_OFFSET);

    /* 覆盖 lw/sw 和 ld/sd 的异常与恢复。 */
    for (unsigned access = 0; access < 4; access++) {
        WindowFirmware fw = {0};
        bool store = access & 1;
        unsigned funct3 = access < 2 ? 2 : 3;
        uint32_t fault_pc;

        if (access) {
            qpci_io_writel(s.dev, s.regs, GPGPU_REG_CP_CTRL,
                           GPGPU_CP_RESET_HOLD);
        }
        fw_window_setup(&fw);
        fw_li(&fw, 5, TRAP_OFFSET);
        fw_emit(&fw, rv_i(0x73, 1, 0, 5, 0x305)); /* csrw mtvec */
        fw_li(&fw, 7, bad_address);
        fw_li(&fw, 5, 0x5a5);
        fault_pc = FIRMWARE_OFFSET + fw.count * 4;
        fw_emit(&fw, store ? rv_store(funct3, 5, 7, 0) :
                            rv_i(0x03, funct3, 6, 7, 0));
        fw_emit(&fw, rv_i(0x03, 6, 7, 11, 8)); /* lwu */
        fw_emit(&fw, rv_store(funct3, 5, 7, 0));
        fw_emit(&fw, rv_i(0x03, funct3, 6, 7, 0));
        fw_emit(&fw, rv_store(3, 6, 10, 32));
        fw_finish(&fw);
        fw_start(&s, &fw);
        fw_wait(&s, 0);
        g_assert_cmphex(qpci_io_readq(s.dev, s.vram, RESULT_OFFSET + 8), ==,
                       store ? 7 : 5);
        g_assert_cmphex(qpci_io_readq(s.dev, s.vram, RESULT_OFFSET + 16), ==,
                       bad_address);
        g_assert_cmphex(qpci_io_readq(s.dev, s.vram, RESULT_OFFSET + 24), ==,
                       fault_pc);
        g_assert_cmphex(qpci_io_readq(s.dev, s.vram, RESULT_OFFSET + 32), ==,
                       0x5a5);
        g_assert_cmphex(qtest_readq(s.host, gpa + HOST_DATA_OFFSET), ==, 0x5a5);
    }
    events = qtest_readl(s.host, VIRT_SMMU_BASE + A_EVENTQ_PROD);
    g_assert_cmpuint(events, ==, ARRAY_SIZE(fault_offsets));
    for (unsigned i = 0; i < events; i++) {
        uint64_t event = QSMMU_SPACE_OFFS_NS + QSMMU_EVENTQ_BASE_ADDR + i * 32;

        g_assert_cmphex(qtest_readl(s.host, event) & 0xff, ==, 0x10);
        g_assert_cmphex(qtest_readl(s.host, event + 4), ==, s.dev->devfn);
        g_assert_cmphex(qtest_readq(s.host, event + 16), ==,
                       iova + 0x1000 + fault_offsets[i]);
    }
    cp_stop(&s);
}

static void host_dma(CPTest *s, uint64_t iova, bool to_host)
{
    uint64_t src = to_host ? DMA_VRAM_OFFSET : iova;
    uint64_t dst = to_host ? iova : DMA_VRAM_OFFSET;
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    uint32_t status;

    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_SRC_LO, src);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_SRC_HI, src >> 32);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_DST_LO, dst);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_DST_HI, dst >> 32);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_SIZE, TEST_DMA_SIZE);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_DMA_CTRL,
                   GPGPU_DMA_START | (to_host ? GPGPU_DMA_DIR_FROM_VRAM : 0));
    do {
        status = qpci_io_readl(s->dev, s->regs, GPGPU_REG_DMA_STATUS);
        if (status != GPGPU_DMA_BUSY) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmphex(status, ==, GPGPU_DMA_COMPLETE);
    g_assert_cmphex(qpci_io_readl(s->dev, s->regs, GPGPU_REG_ERROR_STATUS),
                   ==, 0);
}

static void test_window_host_dma_coexist(void)
{
    CPTest s = {0};
    WindowFirmware fw = {0};
    uint64_t iova, gpa;
    size_t loop;
    uint32_t before, after;
    uint8_t input[TEST_DMA_SIZE], output[TEST_DMA_SIZE];

    cp_start_mode(&s, true);
    window_memory(&s, true, &iova, &gpa);
    qtest_writel(s.host, gpa + HOST_DATA_OFFSET, 0);
    for (unsigned i = 0; i < sizeof(input); i++) {
        input[i] = i * 3 + 1;
    }
    qtest_memwrite(s.host, gpa + HOST_DMA_SRC_OFFSET, input, sizeof(input));
    qtest_memset(s.host, gpa + HOST_DMA_DST_OFFSET, 0, sizeof(output));
    fw_window_setup(&fw);
    loop = fw.count;
    fw_emit(&fw, rv_i(0x03, 2, 5, 7, 0));
    fw_emit(&fw, rv_i(0x13, 0, 5, 5, 1));
    fw_emit(&fw, rv_store(2, 5, 7, 0));
    fw_emit(&fw, rv_store(2, 5, 10, 4));
    fw_emit(&fw, rv_i(0x03, 2, 6, 10, 32));
    fw_emit(&fw, rv_branch(6, 0, ((int)loop - (int)fw.count) * 4));
    fw_finish(&fw);
    fw_start(&s, &fw);
    fw_wait(&s, 4);
    before = qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 4);

    /* CP 继续运行；Host 用独立缓冲区配置原 DMA。 */
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
    host_dma(&s, iova + HOST_DMA_SRC_OFFSET, false);
    host_dma(&s, iova + HOST_DMA_DST_OFFSET, true);
    qtest_memread(s.host, gpa + HOST_DMA_DST_OFFSET, output, sizeof(output));
    g_assert_cmpmem(output, sizeof(output), input, sizeof(input));
    qpci_io_writel(s.dev, s.vram, RESULT_OFFSET + 32, 1);
    fw_wait(&s, 0);
    after = qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 4);
    g_assert_cmpuint(after, >, before);
    g_assert_cmpuint(qtest_readl(s.host, gpa + HOST_DATA_OFFSET), ==, after);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_CP_CTRL), ==,
                   GPGPU_CP_PRESENT);
    g_assert_cmpuint(qtest_readl(s.host, VIRT_SMMU_BASE + A_EVENTQ_PROD),
                     ==, 0);
    cp_stop(&s);
}

#include "gpgpu-device-queue-test.inc"
#include "gpgpu-firmware-test.inc"
#include "gpgpu-gpuvm-test.inc"

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gpgpu/remote/device-access", test_device_access);
    qtest_add_func("/gpgpu/cp/reset-hold", test_reset_hold);
    qtest_add_func("/gpgpu/cp/firmware-replace", test_firmware_replace);
    qtest_add_data_func("/gpgpu/cp/window/none", GINT_TO_POINTER(false),
                        test_window_access);
    qtest_add_data_func("/gpgpu/cp/window/smmuv3", GINT_TO_POINTER(true),
                        test_window_access);
    qtest_add_func("/gpgpu/cp/window/fault-recovery",
                   test_window_fault_recovery);
    qtest_add_func("/gpgpu/cp/window/host-dma-coexist",
                   test_window_host_dma_coexist);
    qtest_add_func("/gpgpu/cp/window-interleave", test_window_interleave);
    register_device_queue_tests();
    register_firmware_queue_tests();
    register_gpuvm_tests();
    return g_test_run();
}
