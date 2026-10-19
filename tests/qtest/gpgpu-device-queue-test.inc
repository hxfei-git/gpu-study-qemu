/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 复用当前 CP QTest 的双进程 fixture；覆盖硬件入口的边界。 */
#include "software/gpgpu/include/gpgpu_queue.h"

static void dq_fw_write(WindowFirmware *fw, uint32_t addr, uint32_t value)
{
    fw_li(fw, 6, addr);
    fw_li(fw, 5, value);
    fw_emit(fw, rv_store(2, 5, 6, 0));
}

static void dq_run(CPTest *s, WindowFirmware *fw)
{
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
    fw_start(s, fw);
    fw_wait(s, 0);
}

static void dq_dispatch(CPTest *s, const uint32_t *code, unsigned words,
                        uint64_t args, uint32_t expected_error);

static void test_hqd_access_rules(void)
{
    CPTest s = {0};
    WindowFirmware fw = {0};
    const unsigned ring = GPGPU_REG_HQD_WINDOW + GPGPU_HQD_RING_LO;
    QPCIBar doorbell;

    cp_start(&s);
    doorbell = qpci_iomap(s.dev, 4, NULL);
    qpci_io_writeq(s.dev, doorbell, 8, 0x12345678);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_ERROR_STATUS), ==, 0);
    qpci_io_writel(s.dev, doorbell, 8, 0x12345678);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_ERROR_STATUS) &
                  GPGPU_ERR_INVALID_CMD);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_ERROR_STATUS, GPGPU_ERR_INVALID_CMD);
    qpci_io_writeq(s.dev, doorbell, 8, UINT64_C(0x100000000));
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_ERROR_STATUS) &
                  GPGPU_ERR_INVALID_CMD);
    qpci_iounmap(s.dev, doorbell);
    qpci_io_writel(s.dev, s.regs, ring, 0x41001000);
    qpci_io_writeq(s.dev, s.regs, ring, UINT64_C(0x1234567800000000));
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0x41001000);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_HQD_SELECT, 2);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_HQD_SELECT, 7);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_HQD_SELECT), ==, 2);
    qpci_io_writel(s.dev, s.regs, ring, 0xdeadbeef);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0);

    dq_fw_write(&fw, GPGPU_CP_HQD_BASE + 2 * GPGPU_HQD_STRIDE +
                GPGPU_HQD_RING_LO, 0x41002000);
    dq_fw_write(&fw, GPGPU_CP_HQD_BASE + 2 * GPGPU_HQD_STRIDE +
                GPGPU_HQD_VMID, 7);
    fw_li(&fw, 10, RESULT_OFFSET);
    fw_finish(&fw);
    dq_run(&s, &fw);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0x41002000);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs,
                    GPGPU_REG_HQD_WINDOW + GPGPU_HQD_VMID), ==, 0);
    qpci_io_writel(s.dev, s.regs, ring, 0x41003000);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0x41002000);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0x41002000);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_HQD_SELECT, 2);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, ring), ==, 0);
    cp_stop(&s);
}

static void dq_ih_submit(CPTest *s, unsigned count)
{
    WindowFirmware fw = {0};

    for (unsigned i = 0; i < count; i++) {
        dq_fw_write(&fw, GPGPU_CP_REG_BASE + GPGPU_REG_IH_SUBMIT,
                    GPGPU_FIRMWARE_BASE + 0x100);
    }
    fw_li(&fw, 10, RESULT_OFFSET);
    fw_finish(&fw);
    dq_run(s, &fw);
}

static void test_ih_publish_ack_fault(void)
{
    CPTest s = {0};
    const uint32_t code[] = {0x00009337, 0x00100293, 0x00532023, 0x00100073};
    uint32_t status;
    uint32_t record[] = { GPGPU_IH_SIGNAL_NOTIFY, 2, 0x41002000, 1,
                          37, 0, 0, 0 };

    cp_start(&s);
    for (unsigned i = 0; i < ARRAY_SIZE(record); i++) {
        qpci_io_writel(s.dev, s.vram, GPGPU_FIRMWARE_BASE + 0x100 + i * 4,
                       record[i]);
    }
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_BASE_LO, 0x41000000);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_SIZE, 64);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_CTRL, GPGPU_IH_ENABLE);
    dq_ih_submit(&s, 1);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==, 1);
    g_assert_cmphex(qtest_readq(s.host, 0x41000008), ==,
                   UINT64_C(0x141002000));
    g_assert_cmpuint(qtest_readl(s.host, 0x41000010), ==, 37);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_IH);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                  GPGPU_IRQ_IH);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IRQ_ENABLE, GPGPU_IRQ_IH);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                  GPGPU_IRQ_IH);
    /* Host 已取得旧 W=1 后，新记录在归还旧槽与 ACK 前到达。 */
    dq_ih_submit(&s, 1);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==, 2);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_RPTR, 1);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_IH);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                  GPGPU_IRQ_IH);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_RPTR, 2);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IRQ_ACK, GPGPU_IRQ_IH);
    g_assert_false(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                   GPGPU_IRQ_IH);
    dq_ih_submit(&s, 3);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==, 4);
    status = qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_STATUS);
    g_assert_true(status & GPGPU_IH_FAULT);
    g_assert_cmpuint((status >> GPGPU_IH_ERROR_SHIFT) & 0xff, ==,
                     GPGPU_IH_ERR_FULL);
    g_assert_cmpuint((status >> GPGPU_IH_RESULT_SHIFT) & 0xff, ==,
                     GPGPU_IH_ERR_FULL);
    /* 非法 R 不得清除首次故障或改写最近一次提交的结果。 */
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_IH_RPTR, 9);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_RPTR), ==, 2);
    g_assert_cmphex(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_STATUS), ==,
                   status);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                  GPGPU_IRQ_ERROR);
    /* IH 已故障时，即使固件提交完整合法配置也不得启动新任务。 */
    qpci_io_writel(s.dev, s.vram, 0x9000, 0);
    dq_dispatch(&s, code, ARRAY_SIZE(code), 0x41000000,
                GPGPU_EXEC_ERR_INVALID_CONFIG);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.vram, 0x9000), ==, 0);
    cp_stop(&s);
}

static void dq_dispatch(CPTest *s, const uint32_t *code, unsigned words,
                        uint64_t args, uint32_t expected_error)
{
    WindowFirmware fw = {0};
    size_t loop;
    const uint32_t base = GPGPU_CP_REG_BASE;

    for (unsigned i = 0; i < words; i++) {
        qpci_io_writel(s->dev, s->vram, DMA_VRAM_OFFSET + i * 4, code[i]);
    }
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ADDR_LO, DMA_VRAM_OFFSET);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ADDR_HI, 0);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_CODE_SIZE, words * 4);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ARGS_LO, args);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ARGS_HI, args >> 32);
    dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ARGS_SIZE, 4);
    for (unsigned offset = GPGPU_REG_GRID_DIM_X;
         offset <= GPGPU_REG_BLOCK_DIM_Z; offset += 4) {
        dq_fw_write(&fw, base + offset, 1);
    }
    dq_fw_write(&fw, base + GPGPU_REG_DISPATCH, 1);
    if (!expected_error) {
        /* 长循环尚未结束，写配置和提前 ACK 都不能改变任务。 */
        dq_fw_write(&fw, base + GPGPU_REG_KERNEL_ARGS_SIZE, 16);
        dq_fw_write(&fw, base + GPGPU_REG_DISPATCH_ACK, 1);
    }
    fw_li(&fw, 10, RESULT_OFFSET);
    fw_li(&fw, 6, base + GPGPU_REG_DISPATCH_STATUS);
    fw_li(&fw, 7, GPGPU_DISPATCH_BUSY);
    loop = fw.count;
    fw_emit(&fw, rv_i(0x03, 2, 5, 6, 0));
    fw_emit(&fw, rv_branch(5, 7, ((int)loop - (int)fw.count) * 4));
    fw_emit(&fw, rv_store(2, 5, 10, 4));
    fw_emit(&fw, rv_i(0x03, 2, 5, 6, 4));
    fw_emit(&fw, rv_store(2, 5, 10, 8));
    dq_fw_write(&fw, base + GPGPU_REG_DISPATCH_ACK, 1);
    fw_finish(&fw);
    dq_run(s, &fw);
    g_assert_cmpuint(qpci_io_readl(s->dev, s->vram, RESULT_OFFSET + 4), ==,
                     expected_error ? GPGPU_DISPATCH_FAILED : GPGPU_DISPATCH_DONE);
    g_assert_cmpuint(qpci_io_readl(s->dev, s->vram, RESULT_OFFSET + 8), ==,
                     expected_error);
    g_assert_cmpuint(qpci_io_readl(s->dev, s->regs, GPGPU_REG_DISPATCH_STATUS),
                     ==, GPGPU_DISPATCH_IDLE);
    g_assert_cmpuint(qpci_io_readl(s->dev, s->regs, GPGPU_REG_KERNEL_ARGS_SIZE),
                     ==, 4);
}

static void test_executor_host_parameters(void)
{
    CPTest s = {0};
    uint64_t iova, gpa;
    /* 从 Host 窗口取值，在 VRAM 写回；重复减法使任务跨越多个分段。 */
    const uint32_t code[] = {
        0x00052283, /* lw t0, 0(a0) */
        0xfff28293, /* addi t0, t0, -1 */
        0xfe029ee3, /* bnez t0, -4 */
        0x00009337, /* lui t1, 0x9 */
        0x00532023, /* sw t0, 0(t1) */
        0x00100073,
    };

    cp_start_mode(&s, true);
    window_memory(&s, true, &iova, &gpa);
    qtest_writel(s.host, gpa + HOST_DATA_OFFSET, 10000);
    qpci_io_writel(s.dev, s.vram, 0x9000, 0xffffffff);
    dq_dispatch(&s, code, ARRAY_SIZE(code), iova + HOST_DATA_OFFSET, 0);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x9000), ==, 0);
    g_assert_cmpuint(qtest_readl(s.host, VIRT_SMMU_BASE + A_EVENTQ_PROD), ==, 0);
    cp_stop(&s);
}

static void test_executor_bounds(void)
{
    CPTest s = {0};
    const uint32_t range[] = {0x00452283, 0x00100073};
    const uint32_t write[] = {0x00552023, 0x00100073};
    const uint32_t fetch[] = {0x00000013};
    const uint32_t loop[] = {0x0000006f};
    const uint32_t invalid[] = {0xffffffff};
    const uint32_t host[] = {0x00052283, 0x00100073};

    cp_start(&s);
    dq_dispatch(&s, range, ARRAY_SIZE(range), 0x41000000,
                GPGPU_EXEC_ERR_ARG_RANGE);
    dq_dispatch(&s, write, ARRAY_SIZE(write), 0x41000000,
                GPGPU_EXEC_ERR_ARG_WRITE);
    dq_dispatch(&s, fetch, ARRAY_SIZE(fetch), 0x41000000,
                GPGPU_EXEC_ERR_CODE_RANGE);
    dq_dispatch(&s, loop, ARRAY_SIZE(loop), 0x41000000,
                GPGPU_EXEC_ERR_BUDGET);
    dq_dispatch(&s, invalid, ARRAY_SIZE(invalid), 0x41000000,
                GPGPU_EXEC_ERR_INSTRUCTION);
    dq_dispatch(&s, host, ARRAY_SIZE(host), UINT64_C(0x400000000),
                GPGPU_EXEC_ERR_ARG_READ);
    cp_stop(&s);
}

/* A57 与 RV64 CP 真正并行执行，高低半部同时变化，双向检查撕裂。 */
static void test_shared_atomic64(void)
{
    CPTest s = {0};
    WindowFirmware fw = {0};
    uint32_t arm[64];
    unsigned n = 0, arm_loop, arm_good, arm_error, arm_stop, beq, bne, cbz;
    size_t loop, equal_a, equal_b, bad_jump, good, finish;
    uint64_t iova, gpa;
    const uint64_t a = UINT64_C(0x1111222233334444);
    const uint64_t b = UINT64_C(0xaaaabbbbccccdddd);
    g_autofree char *device = NULL;
    g_autofree char *args = NULL;

    gpgpu_remote_start(&s.remote);
    device = gpgpu_remote_device_option(&s.remote);
    args = g_strdup_printf("-machine virt,gic-version=3 -cpu cortex-a57 "
                           "-m 512M -nodefaults -accel tcg -S "
                           "-device loader,addr=0x40010000,cpu-num=0 "
                           "-device %s", device);
    s.host = qtest_init(args);
    qpci_init_generic(&s.bus, s.host, NULL, false);
    qpci_device_foreach(&s.bus.bus, 0x1234, 0x1337, save_device, &s.dev);
    g_assert_nonnull(s.dev);
    qpci_device_enable(s.dev);
    s.regs = qpci_iomap(s.dev, 0, NULL);
    s.vram = qpci_iomap(s.dev, 2, NULL);
    window_memory(&s, false, &iova, &gpa);
    qtest_memset(s.host, gpa + HOST_DATA_OFFSET, 0, 40);
    qtest_writeq(s.host, gpa + HOST_DATA_OFFSET, a);
    qtest_writeq(s.host, gpa + HOST_DATA_OFFSET + 16, a);
    qpci_io_writeq(s.dev, s.vram, PARAM_OFFSET + 16, a);
    qpci_io_writeq(s.dev, s.vram, PARAM_OFFSET + 24, b);

    /* MOVZ/MOVK 生成地址和两个 64 位模式，不依赖外部 AArch64 工具链。 */
    for (unsigned reg = 0; reg < 3; reg++) {
        uint64_t value = reg == 0 ? gpa + HOST_DATA_OFFSET : reg == 1 ? a : b;

        for (unsigned part = 0; part < 4; part++) {
            arm[n++] = (part ? 0xf2800000u : 0xd2800000u) |
                       part << 21 | ((value >> (part * 16)) & 0xffff) << 5 | reg;
        }
    }
    arm_loop = n;
    arm[n++] = 0xf9000001; /* str x1, [x0] */
    arm[n++] = 0xf9000002; /* str x2, [x0] */
    arm[n++] = 0xf9400803; /* ldr x3, [x0, #16] */
    arm[n++] = 0xeb01007f; /* cmp x3, x1 */
    beq = n++;
    arm[n++] = 0xeb02007f; /* cmp x3, x2 */
    bne = n++;
    arm_good = n;
    arm[n++] = 0xf9401004; /* ldr x4, [x0, #32] */
    arm[n++] = 0x91000484; /* add x4, x4, #1 */
    arm[n++] = 0xf9001004; /* str x4, [x0, #32] */
    arm[n++] = 0xf9400403; /* ldr x3, [x0, #8] */
    cbz = n++;
    arm_stop = n;
    arm[n++] = 0xd503207f; /* wfi */
    arm[n++] = 0x17ffffff; /* b -4 */
    arm_error = n;
    arm[n++] = 0xd2800023; /* mov x3, #1 */
    arm[n++] = 0xf9000c03; /* str x3, [x0, #24] */
    arm[n] = 0x14000000 | ((arm_stop - n) & 0x03ffffff);
    n++;
    arm[beq] = 0x54000000 | ((arm_good - beq) & 0x7ffff) << 5;
    arm[bne] = 0x54000001 | ((arm_error - bne) & 0x7ffff) << 5;
    arm[cbz] = 0xb4000003 | ((arm_loop - cbz) & 0x7ffff) << 5;
    for (unsigned i = 0; i < n; i++) {
        qtest_writel(s.host, 0x40010000 + i * 4, arm[i]);
    }
    qtest_qmp_assert_success(s.host, "{ 'execute': 'cont' }");

    fw_window_setup(&fw);
    fw_emit(&fw, rv_i(0x03, 3, 28, 11, 16));
    fw_emit(&fw, rv_i(0x03, 3, 29, 11, 24));
    fw_li(&fw, 12, 2000);
    loop = fw.count;
    fw_emit(&fw, rv_i(0x03, 3, 5, 7, 0));
    equal_a = fw.count++;
    equal_b = fw.count++;
    fw_li(&fw, 5, 1);
    fw_emit(&fw, rv_store(2, 5, 10, 8));
    bad_jump = fw.count++;
    good = fw.count;
    fw_emit(&fw, rv_store(3, 28, 7, 16));
    fw_emit(&fw, rv_store(3, 29, 7, 16));
    fw_emit(&fw, rv_i(0x13, 0, 12, 12, -1));
    fw_emit(&fw, rv_branch(12, 0, ((int)loop - (int)fw.count) * 4) | (1 << 12));
    finish = fw.count;
    fw_emit(&fw, rv_i(0x03, 2, 5, 7, 24));
    fw_emit(&fw, rv_store(2, 5, 10, 12));
    fw_li(&fw, 5, 1);
    fw_emit(&fw, rv_store(3, 5, 7, 8));
    fw_finish(&fw);
    fw.insns[equal_a] = rv_branch(5, 28, (good - equal_a) * 4);
    fw.insns[equal_b] = rv_branch(5, 29, (good - equal_b) * 4);
    fw.insns[bad_jump] = rv_branch(0, 0, (finish - bad_jump) * 4);
    fw_start(&s, &fw);
    fw_wait(&s, 0);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 8), ==, 0);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.vram, RESULT_OFFSET + 12), ==, 0);
    g_assert_cmpuint(qtest_readq(s.host, gpa + HOST_DATA_OFFSET + 32), >, 0);
    cp_stop(&s);
}

static void register_device_queue_tests(void)
{
    qtest_add_func("/gpgpu/queue-device/shared-atomic64", test_shared_atomic64);
    qtest_add_func("/gpgpu/queue-device/hqd-access", test_hqd_access_rules);
    qtest_add_func("/gpgpu/queue-device/ih-publish-ack-fault",
                   test_ih_publish_ack_fault);
    qtest_add_func("/gpgpu/queue-device/executor-host-parameters",
                   test_executor_host_parameters);
    qtest_add_func("/gpgpu/queue-device/executor-bounds", test_executor_bounds);
}
