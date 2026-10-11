/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * A57 Host 经 PCI 代理装载和启动原生 RV32 CP 固件。
 */

#include "qemu/osdep.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "hw/gpgpu/gpgpu_regs.h"
#include "gpgpu-remote.h"

#define FIRMWARE_OFFSET 0x4000
#define RESULT_OFFSET 0x6000

typedef struct CPTest {
    GPGPURemote remote;
    QTestState *host;
    QGenericPCIBus bus;
    QPCIDevice *dev;
    QPCIBar regs;
    QPCIBar vram;
} CPTest;

static void save_device(QPCIDevice *dev, int devfn, void *opaque)
{
    *(QPCIDevice **)opaque = dev;
}

static void cp_start(CPTest *s)
{
    g_autofree char *device = NULL;
    g_autofree char *args = NULL;

    gpgpu_remote_start(&s->remote);
    device = gpgpu_remote_device_option(&s->remote);
    args = g_strdup_printf("-machine virt,gic-version=3 -cpu cortex-a57 "
                           "-m 512M -nodefaults -device %s", device);
    s->host = qtest_init(args);
    qpci_init_generic(&s->bus, s->host, NULL, false);
    qpci_device_foreach(&s->bus.bus, 0x1234, 0x1337, save_device, &s->dev);
    g_assert_nonnull(s->dev);
    qpci_device_enable(s->dev);
    s->regs = qpci_iomap(s->dev, 0, NULL);
    s->vram = qpci_iomap(s->dev, 2, NULL);
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
    g_assert_cmpstr(qdict_get_str(cpu, "target"), ==, "riscv32");
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gpgpu/remote/device-access", test_device_access);
    qtest_add_func("/gpgpu/cp/reset-hold", test_reset_hold);
    qtest_add_func("/gpgpu/cp/firmware-replace", test_firmware_replace);
    return g_test_run();
}
