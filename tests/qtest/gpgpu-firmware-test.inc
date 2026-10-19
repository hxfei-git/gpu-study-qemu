/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 与 gpgpu-cp-test.c 共用双进程设备和 PCI 访问辅助函数。 */
#include "software/gpgpu/include/gpgpu_queue.h"

#define QUEUE_HOST 0x41000000ULL
#define QUEUE_MQD(slot) (QUEUE_HOST + (slot) * 0x100)
#define QUEUE_RING(slot) (QUEUE_HOST + 0x1000 + (slot) * 0x1000)
#define QUEUE_WPTR(slot) (QUEUE_HOST + 0x6000 + (slot) * 16)
#define QUEUE_RPTR(slot) (QUEUE_WPTR(slot) + 8)
#define QUEUE_IH (QUEUE_HOST + 0x8000)
#define QUEUE_SIGNAL(index) (QUEUE_HOST + 0x9000 + (index) * 32)
#define QUEUE_MAILBOX(index) (QUEUE_HOST + 0xa000 + (index) * 8)
#define QUEUE_ARGS(index) (QUEUE_HOST + 0xb000 + (index) * 8)
#define QUEUE_RESULT (QUEUE_HOST + 0xc000)

static void queue_wait32(CPTest *s, uint64_t address, uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint32_t value;

    do {
        value = qtest_readl(s->host, address);
        if (value == expected) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmphex(value, ==, expected);
}

static void queue_mqd(CPTest *s, unsigned slot, uint32_t start)
{
    GpgpuMqd mqd = {
        .ring_base = cpu_to_le64(QUEUE_RING(slot)),
        .rptr_report_addr = cpu_to_le64(QUEUE_RPTR(slot)),
        .wptr_poll_addr = cpu_to_le64(QUEUE_WPTR(slot)),
        .pq_control = cpu_to_le32(256),
        .doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | slot * 8),
        .aql_control = cpu_to_le32(slot > 1),
        .saved_rptr = cpu_to_le64(start),
    };

    qtest_memwrite(s->host, QUEUE_MQD(slot), &mqd, sizeof(mqd));
    qtest_writel(s->host, QUEUE_WPTR(slot), start);
    qtest_writel(s->host, QUEUE_RPTR(slot), start);
    if (slot > 1) {
        for (unsigned i = 0; i < 4; i++) {
            qtest_writel(s->host, QUEUE_RING(slot) + i * 64, GPGPU_AQL_INVALID);
        }
    }
}

static void queue_boot_ih_size(CPTest *s, unsigned ih_size)
{
    const char *path = g_getenv("QTEST_GPGPU_FIRMWARE");
    g_autofree char *image = NULL;
    gsize size;
    const uint32_t fields[] = {
        GPGPU_HQD_MQD_LO, GPGPU_HQD_MQD_HI, GPGPU_HQD_RING_LO,
        GPGPU_HQD_RING_HI, GPGPU_HQD_RPTR_ADDR_LO, GPGPU_HQD_RPTR_ADDR_HI,
        GPGPU_HQD_WPTR_ADDR_LO, GPGPU_HQD_WPTR_ADDR_HI,
        GPGPU_HQD_CONTROL, GPGPU_HQD_DOORBELL,
    };
    const uint32_t values[] = {
        QUEUE_MQD(0), 0, QUEUE_RING(0), 0, QUEUE_RPTR(0), 0,
        QUEUE_WPTR(0), 0, 256, GPGPU_DOORBELL_ENABLE,
    };

    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &image, &size, NULL));
    g_assert_cmpuint(size, <, GPGPU_FIRMWARE_STACK_BASE - GPGPU_FIRMWARE_BASE);
    cp_start(s);
    qtest_memset(s->host, QUEUE_HOST, 0, 0x10000);
    queue_mqd(s, 0, 0);
    qpci_memwrite(s->dev, s->vram, GPGPU_FIRMWARE_BASE, image, size);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_IH_BASE_LO, QUEUE_IH);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_IH_BASE_HI, 0);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_IH_SIZE, ih_size);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_IH_CTRL, GPGPU_IH_ENABLE);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_HQD_SELECT, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(fields); i++) {
        qpci_io_writel(s->dev, s->regs, GPGPU_REG_HQD_WINDOW + fields[i],
                       values[i]);
    }
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ACTIVE, 1);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_RESET_VECTOR, GPGPU_FIRMWARE_BASE);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_ENABLE);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_CP_CTRL, 0);
}

static void queue_boot(CPTest *s)
{
    queue_boot_ih_size(s, 4096);
}

static uint32_t queue_hqd(CPTest *s, unsigned slot, unsigned field)
{
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_HQD_SELECT, slot);
    return qpci_io_readl(s->dev, s->regs, GPGPU_REG_HQD_WINDOW + field);
}

static void queue_wait_error(CPTest *s, unsigned slot, uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint32_t value;

    do {
        value = queue_hqd(s, slot, GPGPU_HQD_ERROR);
        if (value == expected) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(value, ==, expected);
}

static uint32_t queue_manage(CPTest *s, unsigned source, unsigned opcode,
                             uint64_t mqd)
{
    static uint64_t sequence;
    uint32_t wptr = qtest_readl(s->host, QUEUE_WPTR(source));
    GpgpuManagementPacket packet = {
        .opcode = cpu_to_le32(opcode),
        .queue_kind = cpu_to_le32(source + 1),
        .address = cpu_to_le64(mqd),
    };
    GpgpuManagementPacket completion = {
        .opcode = cpu_to_le32(GPGPU_WRITE_COMPLETION),
        .address = cpu_to_le64(QUEUE_RESULT),
        .value = cpu_to_le64(++sequence),
    };

    qtest_writeq(s->host, QUEUE_RESULT, 0);
    qtest_writel(s->host, QUEUE_RESULT + 8, GPGPU_MANAGEMENT_PENDING);
    qtest_memwrite(s->host, QUEUE_RING(source) + (wptr & 7) * 32,
                   &packet, sizeof(packet));
    qtest_memwrite(s->host, QUEUE_RING(source) + ((wptr + 1) & 7) * 32,
                   &completion, sizeof(completion));
    qtest_writel(s->host, QUEUE_WPTR(source), wptr + 2);
    queue_wait32(s, QUEUE_RPTR(source), wptr + 2);
    g_assert_cmphex(qtest_readq(s->host, QUEUE_RESULT), ==, sequence);
    return qtest_readl(s->host, QUEUE_RESULT + 8);
}

static void queue_create(CPTest *s, unsigned slot, uint32_t start)
{
    queue_mqd(s, slot, start);
    g_assert_cmpuint(queue_manage(s, slot == 1 ? 0 : 1, GPGPU_MAP_QUEUES,
                                  QUEUE_MQD(slot)), ==, GPGPU_MANAGEMENT_OK);
}

static void test_queue_management(void)
{
    CPTest s = {0};

    queue_boot(&s);
    queue_create(&s, 1, 0);
    queue_create(&s, 2, 0);
    queue_create(&s, 3, 0);
    /* 管理关闭顺序不能跳过尚占用的用户槽。 */
    g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, QUEUE_MQD(1)), ==,
                     GPGPU_MANAGEMENT_RECOVERY_REQUIRED);
    g_assert_cmpuint(queue_hqd(&s, 1, GPGPU_HQD_ACTIVE), ==, 1);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(2)), ==,
                     GPGPU_MANAGEMENT_DUPLICATE);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(4)), ==,
                     GPGPU_MANAGEMENT_NO_SLOT);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==,
                     GPGPU_MANAGEMENT_NOT_MAPPED);
    queue_create(&s, 2, 0);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(3)), ==, 0);
    g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES, QUEUE_MQD(1)), ==, 0);
    cp_stop(&s);
}

static void queue_kernel(CPTest *s)
{
    const uint32_t code[] = { 0x00052283, 0x00452303, 0x0062a023, 0x00100073 };
    GpgpuKernelDescriptor descriptor = {
        .code_entry = cpu_to_le32(0x11000),
        .code_size = cpu_to_le32(sizeof(code)),
        .kernarg_size = cpu_to_le32(8),
    };

    qpci_memwrite(s->dev, s->vram, 0x10000, &descriptor, sizeof(descriptor));
    for (unsigned i = 0; i < ARRAY_SIZE(code); i++) {
        qpci_io_writel(s->dev, s->vram, 0x11000 + 4 * i, code[i]);
    }
}

static void queue_packet(CPTest *s, unsigned slot, uint32_t number,
                         unsigned task, bool publish)
{
    GpgpuAqlPacket packet = {
        .full_header = cpu_to_le32(GPGPU_AQL_INVALID),
        .workgroup_size_x = cpu_to_le16(1),
        .workgroup_size_y = cpu_to_le16(1),
        .workgroup_size_z = cpu_to_le16(1),
        .grid_size_x = cpu_to_le32(1),
        .grid_size_y = cpu_to_le32(1),
        .grid_size_z = cpu_to_le32(1),
        .kernel_object = cpu_to_le64(0x10000),
        .kernarg_address = cpu_to_le64(QUEUE_ARGS(task)),
        .completion_signal = cpu_to_le64(QUEUE_SIGNAL(task)),
    };
    GpgpuSignal signal = {
        .value = cpu_to_le64(1),
        .event_id = cpu_to_le32(task + 1),
        .event_mailbox_ptr = cpu_to_le64(QUEUE_MAILBOX(task)),
    };
    uint64_t address = QUEUE_RING(slot) + (number & 3) * 64;

    qtest_memwrite(s->host, QUEUE_SIGNAL(task), &signal, sizeof(signal));
    qtest_writel(s->host, QUEUE_ARGS(task), 0x12000 + task * 4);
    qtest_writel(s->host, QUEUE_ARGS(task) + 4, 0xaabb0000 + task);
    qtest_memwrite(s->host, address, &packet, sizeof(packet));
    if (publish) {
        qtest_writel(s->host, address, (1 << 16) | GPGPU_AQL_HEADER);
    }
}

static void test_queue_dispatch_publish(void)
{
    CPTest s = {0};
    uint32_t start = UINT32_MAX - 1;
    QPCIBar doorbell;

    queue_boot(&s);
    doorbell = qpci_iomap(s.dev, 4, NULL);
    queue_create(&s, 1, 0);
    queue_create(&s, 2, start);
    queue_create(&s, 3, 0);
    queue_kernel(&s);
    queue_packet(&s, 2, start, 0, false);
    queue_packet(&s, 2, start + 1, 1, true);
    qtest_writel(s.host, QUEUE_WPTR(2), 0);
    qpci_io_writeq(s.dev, doorbell, 2 * 8, start + 1);
    queue_packet(&s, 3, 0, 2, true);
    qtest_writel(s.host, QUEUE_WPTR(3), 1);
    queue_wait32(&s, QUEUE_RPTR(3), 1);
    g_assert_cmphex(qtest_readl(s.host, QUEUE_RPTR(2)), ==, start);
    /* W 不变时发布前包；固件必须重新读取 Header 并跨 32 位回绕。 */
    qtest_writel(s.host, QUEUE_RING(2) + (start & 3) * 64,
                 (1 << 16) | GPGPU_AQL_HEADER);
    /* 后到的通知编号更小，不能撤销后一包，也不能覆盖共享 W/R。 */
    qpci_io_writeq(s.dev, doorbell, 2 * 8, start);
    queue_wait32(&s, QUEUE_RPTR(2), 0);
    queue_packet(&s, 2, 0, 3, true);
    qtest_writel(s.host, QUEUE_WPTR(2), 1);
    qpci_io_writeq(s.dev, doorbell, 2 * 8, 0);
    queue_wait32(&s, QUEUE_RPTR(2), 1);
    for (unsigned task = 0; task < 4; task++) {
        g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(task)), ==, 0);
        g_assert_cmphex(qtest_readq(s.host, QUEUE_MAILBOX(task)), ==, task + 1);
        g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x12000 + 4 * task), ==,
                        0xaabb0000 + task);
    }
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==, 4);
    g_assert_cmphex(qtest_readl(s.host, QUEUE_RING(2)), ==, GPGPU_AQL_INVALID);
    g_assert_cmphex(qtest_readl(s.host, QUEUE_RING(2) + 128), ==,
                    GPGPU_AQL_INVALID);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    g_assert_cmphex(qtest_readq(s.host, QUEUE_MQD(2) + 0x28), ==, 1);
    qpci_iounmap(s.dev, doorbell);
    cp_stop(&s);
}

/* 孤立完成命令是管理通道故障，必须与普通请求失败区别处理。 */
static void queue_channel_fault(unsigned source)
{
    CPTest s = {0};
    GpgpuManagementPacket orphan = {
        .opcode = cpu_to_le32(GPGPU_WRITE_COMPLETION),
        .address = cpu_to_le64(QUEUE_RESULT),
        .value = cpu_to_le64(UINT64_C(0x1234567887654321)),
    };
    uint32_t progress, notifications;
    int64_t deadline;

    g_test_message("固件管理通道停止范围：%s", source ? "HIQ" : "KIQ");
    queue_boot(&s);
    queue_create(&s, 1, 0);
    queue_create(&s, 2, 0);
    queue_create(&s, 3, 0);
    queue_kernel(&s);
    progress = qtest_readl(s.host, QUEUE_WPTR(source));
    qtest_memwrite(s.host, QUEUE_RING(source) + (progress & 7) * 32,
                   &orphan, sizeof(orphan));
    qtest_writel(s.host, QUEUE_WPTR(source), progress + 1);
    queue_wait_error(&s, source, GPGPU_QUEUE_ERROR_PAIR);
    g_assert_cmpuint(queue_hqd(&s, source, GPGPU_HQD_ACTIVE), ==, 0);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(source)), ==, progress);
    g_assert_cmphex(qtest_readq(s.host, QUEUE_RESULT), !=,
                    UINT64_C(0x1234567887654321));
    queue_packet(&s, 2, 0, 0, true);
    qtest_writel(s.host, QUEUE_WPTR(2), 1);
    if (source) {
        queue_wait32(&s, QUEUE_RPTR(2), 1);
        /* KIQ 仍能返回请求结果，故障 HIQ 的重建继续留给后续恢复。 */
        g_assert_cmpuint(queue_manage(&s, 0, GPGPU_UNMAP_QUEUES,
                                      QUEUE_MQD(1)), ==,
                         GPGPU_MANAGEMENT_RECOVERY_REQUIRED);
    } else {
        g_usleep(100000);
        g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
        g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);
        g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x12000), ==, 0);
    }
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        notifications = qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR);
        if (notifications == (source ? 2 : 1)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(notifications, ==, source ? 2 : 1);
    /* 停止后追加不同坏命令不覆盖首次错误，也不重复发送错误记录。 */
    qtest_writel(s.host, QUEUE_RING(source) + ((progress + 1) & 7) * 32, 99);
    qtest_writel(s.host, QUEUE_WPTR(source), progress + 2);
    g_usleep(20000);
    g_assert_cmpuint(queue_hqd(&s, source, GPGPU_HQD_ERROR), ==,
                     GPGPU_QUEUE_ERROR_PAIR);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(source)), ==, progress);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==,
                     notifications);
    cp_stop(&s);
}

/* 满环发生在 Signal 和通知槽写回之后，固件保留这些写入且停止归还包槽。 */
static void queue_ih_fault_retirement(void)
{
    CPTest s = {0};
    int64_t deadline;
    uint32_t status;

    g_test_message("固件 IH 满环：保留已写 Signal，停止 Header/R 和后续派发");
    queue_boot_ih_size(&s, sizeof(GpgpuIhRecord));
    queue_create(&s, 1, 0);
    queue_create(&s, 2, 0);
    queue_create(&s, 3, 0);
    queue_kernel(&s);
    queue_packet(&s, 2, 0, 0, true);
    qtest_writel(s.host, QUEUE_WPTR(2), 1);
    queue_wait32(&s, QUEUE_RPTR(2), 1);
    queue_packet(&s, 2, 1, 1, true);
    qtest_writel(s.host, QUEUE_WPTR(2), 2);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        status = qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_STATUS);
        if (status & GPGPU_IH_FAULT) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_true(status & GPGPU_IH_FAULT);
    g_assert_cmpuint((status >> GPGPU_IH_ERROR_SHIFT) & 0xff, ==,
                     GPGPU_IH_ERR_FULL);
    g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(1)), ==, 0);
    g_assert_cmphex(qtest_readq(s.host, QUEUE_MAILBOX(1)), ==, 2);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 1);
    g_assert_cmphex(qtest_readl(s.host, QUEUE_RING(2) + 64), ==,
                    (1 << 16) | GPGPU_AQL_HEADER);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IH_WPTR), ==, 1);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_IH + 0x10), ==, 1);
    g_assert_true(qpci_io_readl(s.dev, s.regs, GPGPU_REG_IRQ_STATUS) &
                  GPGPU_IRQ_ERROR);
    queue_packet(&s, 3, 0, 2, true);
    qtest_writel(s.host, QUEUE_WPTR(3), 1);
    g_usleep(100000);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(3)), ==, 0);
    g_assert_cmphex(qtest_readq(s.host, QUEUE_SIGNAL(2)), ==, 1);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x12008), ==, 0);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_DISPATCH_STATUS), ==,
                     GPGPU_DISPATCH_IDLE);
    cp_stop(&s);
}

static void test_queue_fault_scope(void)
{
    CPTest s = {0};
    int64_t deadline;
    uint32_t error;

    queue_boot(&s);
    queue_create(&s, 1, 0);
    queue_mqd(&s, 2, 0);
    /* 地址对齐且不溢出，但不在 Host RAM 中，触发原生 load access fault。 */
    qtest_writeq(s.host, QUEUE_MQD(2), 0x70000000);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    qtest_writel(s.host, QUEUE_WPTR(2), 1);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_HQD_SELECT, 2);
    deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        error = qpci_io_readl(s.dev, s.regs,
                              GPGPU_REG_HQD_WINDOW + GPGPU_HQD_ERROR);
        if (error) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(error, ==, GPGPU_QUEUE_ERROR_RING);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_RPTR(2)), ==, 0);
    queue_create(&s, 3, 0);
    queue_kernel(&s);
    queue_packet(&s, 3, 0, 0, true);
    qtest_writel(s.host, QUEUE_WPTR(3), 1);
    queue_wait32(&s, QUEUE_RPTR(3), 1);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    g_assert_cmpuint(qtest_readl(s.host, QUEUE_MQD(2) + 0x30), ==,
                     GPGPU_QUEUE_ERROR_RING);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(2)), ==,
                     GPGPU_MANAGEMENT_INVALID);
    g_assert_cmpuint(queue_hqd(&s, 2, GPGPU_HQD_ERROR), ==,
                     GPGPU_QUEUE_ERROR_RING);
    cp_stop(&s);
    queue_channel_fault(0);
    queue_channel_fault(1);
    queue_ih_fault_retirement();
}

static void register_firmware_queue_tests(void)
{
    qtest_add_func("/gpgpu/firmware/management", test_queue_management);
    qtest_add_func("/gpgpu/firmware/dispatch-publish", test_queue_dispatch_publish);
    qtest_add_func("/gpgpu/firmware/fault-scope", test_queue_fault_scope);
}
