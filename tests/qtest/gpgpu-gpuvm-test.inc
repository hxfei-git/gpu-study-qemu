/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 通过真实 CP 固件覆盖每条用户访存路径，页表由测试 Host 显式填写。 */
#define VM_ROOT 0x20000U
#define VM_RING 0x00100000U
#define VM_PROGRESS 0x00101000U
#define VM_ARGS 0x00102000U
#define VM_SIGNAL 0x00103000U
#define VM_DESC 0x00400000U
#define VM_CODE 0x00401000U
#define VM_DATA 0x00402000U

/* 创建三级路径并返回叶项的 VRAM 偏移，表页均不向用户映射。 */
static uint32_t vm_test_leaf_root(CPTest *s, uint32_t root, uint32_t va, uint32_t *next)
{
    uint32_t table = root;
    const unsigned shifts[] = {30, 21};

    for (unsigned i = 0; i < 2; i++) {
        uint32_t offset = table + ((va >> shifts[i]) & (i ? 511 : 3)) * 8;
        uint64_t entry = qpci_io_readq(s->dev, s->vram, offset);

        if (!(entry & GPGPU_VM_VALID)) {
            table = *next;
            *next += 4096;
            qpci_io_writeq(s->dev, s->vram, offset, table | GPGPU_VM_VALID);
        } else {
            table = entry & GPGPU_VM_ADDR_MASK;
        }
    }
    return table + ((va >> 12) & 511) * 8;
}

static void vm_test_map(CPTest *s, uint32_t va, uint64_t target,
                        uint64_t flags, uint32_t *next)
{
    qpci_io_writeq(s->dev, s->vram, vm_test_leaf_root(s, VM_ROOT, va, next),
                   target | flags | GPGPU_VM_VALID);
}

/* GPUVM 根发布后才装载用户 MQD；管理 MQD 和结果仍使用 Host DMA 地址。 */
static void vm_test_prepare(CPTest *s, int broken)
{
    uint32_t next = VM_ROOT + 4096;
    const uint32_t va[] = {VM_RING, VM_PROGRESS, VM_DESC, VM_CODE,
                           VM_ARGS, VM_DATA, VM_SIGNAL, GPGPU_VM_NOTIFY_VA};
    const uint64_t target[] = {QUEUE_RING(2), QUEUE_HOST + 0x6000, 0x10000, 0x11000,
                               QUEUE_HOST + 0xb000, 0x12000,
                               QUEUE_HOST + 0x9000, QUEUE_HOST + 0xa000};
    const uint64_t flags[] = {
        GPGPU_VM_SYSTEM | GPGPU_VM_READ | GPGPU_VM_WRITE,
        GPGPU_VM_SYSTEM | GPGPU_VM_READ | GPGPU_VM_WRITE,
        GPGPU_VM_READ, GPGPU_VM_READ | GPGPU_VM_EXEC,
        GPGPU_VM_SYSTEM | GPGPU_VM_READ, GPGPU_VM_READ | GPGPU_VM_WRITE,
        GPGPU_VM_SYSTEM | GPGPU_VM_READ | GPGPU_VM_WRITE,
        GPGPU_VM_SYSTEM | GPGPU_VM_WRITE,
    };
    GpgpuMqd mqd = {
        .ring_base = cpu_to_le64(VM_RING),
        .rptr_report_addr = cpu_to_le64(VM_PROGRESS + 40),
        .wptr_poll_addr = cpu_to_le64(VM_PROGRESS + 32),
        .pq_control = cpu_to_le32(256),
        .doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | 16),
        .aql_control = cpu_to_le32(1), .vmid = cpu_to_le32(1),
    };
    GpgpuKernelDescriptor desc = {
        .code_entry = cpu_to_le32(VM_CODE), .code_size = cpu_to_le32(16),
        .kernarg_size = cpu_to_le32(8),
    };

    s->gpuvm = true;
    queue_boot(s);
    queue_create(s, 1, 0);
    queue_kernel(s);
    qpci_memwrite(s->dev, s->vram, 0x10000, &desc, sizeof(desc));
    for (unsigned i = 0; i < ARRAY_SIZE(va); i++) {
        uint64_t perms = flags[i];

        /* 各次只破坏一条路径；进度页和完成页保留读权限以推进到写阶段。 */
        if ((int)i == broken) {
            if (i == 1 || i == 6 || i == 7)
                perms &= ~GPGPU_VM_WRITE;
            else if (i == 3)
                perms &= ~GPGPU_VM_EXEC;
            else if (i == 5)
                perms &= ~GPGPU_VM_WRITE;
            else
                continue;
        }
        vm_test_map(s, va[i], target[i], perms, &next);
    }
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_CONTEXT(1), VM_ROOT);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_INVALIDATE, 1);
    g_assert_cmpuint(qpci_io_readl(s->dev, s->regs, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE, 1);
    qtest_memwrite(s->host, QUEUE_MQD(2), &mqd, sizeof(mqd));
    g_assert_cmpuint(queue_manage(s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(2)), ==, 0);
}

static void vm_test_submit_data(CPTest *s, uint32_t data)
{
    queue_packet(s, 2, 0, 0, false);
    qtest_writeq(s->host, QUEUE_RING(2) + 32, VM_DESC);
    qtest_writeq(s->host, QUEUE_RING(2) + 40, VM_ARGS);
    qtest_writeq(s->host, QUEUE_RING(2) + 56, VM_SIGNAL);
    qtest_writeq(s->host, QUEUE_SIGNAL(0) + 16, GPGPU_VM_NOTIFY_VA);
    qtest_writel(s->host, QUEUE_ARGS(0), data);
    qtest_writel(s->host, QUEUE_RING(2), (1 << 16) | GPGPU_AQL_HEADER);
    qtest_writel(s->host, QUEUE_WPTR(2), 1);
}

static void vm_test_submit(CPTest *s)
{
    vm_test_submit_data(s, VM_DATA);
}

/* 完整成功路径、忙时失效拒绝和本次 ACK；CP 单独复位保留翻译上下文。 */
static void test_gpuvm_translation(void)
{
    CPTest s = {0};

    vm_test_prepare(&s, -1);
    vm_test_submit(&s);
    queue_wait32(&s, QUEUE_RPTR(2), 1);
    g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x12000), ==, 0xaabb0000);
    g_assert_cmpuint(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 0);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_VM_INVALIDATE, 1);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ERROR);
    g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_VM_INVALIDATE, 1);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_STATUS), ==, GPGPU_VM_ACK);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_CP_CTRL, GPGPU_CP_RESET_HOLD);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_FIRMWARE), ==, 0);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE), ==, 1);
    qpci_io_writel(s.dev, s.regs, GPGPU_REG_GLOBAL_CTRL, GPGPU_CTRL_RESET);
    g_assert_cmpuint(qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_CONTEXT(1) + GPGPU_VM_ENABLE), ==, 0);
    cp_stop(&s);
}

/* 源队列停止后，另一 VM 使用完整映射继续运行并发布完成。 */
static void vm_test_other(CPTest *s)
{
    uint32_t next = 0x31000;
    const uint32_t va[] = {VM_RING, VM_PROGRESS, VM_DESC, VM_CODE,
                           VM_ARGS, VM_DATA, VM_SIGNAL, GPGPU_VM_NOTIFY_VA};
    const uint64_t target[] = {QUEUE_RING(3), QUEUE_HOST + 0x6000, 0x10000, 0x11000,
                               QUEUE_HOST + 0xb000, 0x12000,
                               QUEUE_HOST + 0x9000, QUEUE_HOST + 0xa000};
    GpgpuMqd mqd = {
        .ring_base = cpu_to_le64(VM_RING),
        .rptr_report_addr = cpu_to_le64(VM_PROGRESS + 56),
        .wptr_poll_addr = cpu_to_le64(VM_PROGRESS + 48),
        .pq_control = cpu_to_le32(256), .vmid = cpu_to_le32(2),
        .doorbell_control = cpu_to_le32(GPGPU_DOORBELL_ENABLE | 24),
        .aql_control = cpu_to_le32(1),
    };

    for (unsigned i = 0; i < ARRAY_SIZE(va); i++) {
        uint32_t leaf = vm_test_leaf_root(s, 0x30000, va[i], &next);
        uint64_t flags = GPGPU_VM_VALID | GPGPU_VM_PERMS;

        if (target[i] >= QUEUE_HOST)
            flags |= GPGPU_VM_SYSTEM;
        qpci_io_writeq(s->dev, s->vram, leaf, target[i] | flags);
    }
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_CONTEXT(2), 0x30000);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_INVALIDATE, 2);
    qpci_io_writel(s->dev, s->regs, GPGPU_REG_VM_CONTEXT(2) + GPGPU_VM_ENABLE, 1);
    qtest_memwrite(s->host, QUEUE_MQD(3), &mqd, sizeof(mqd));
    g_assert_cmpuint(queue_manage(s, 1, GPGPU_MAP_QUEUES, QUEUE_MQD(3)), ==, 0);
    queue_packet(s, 3, 0, 1, false);
    qtest_writeq(s->host, QUEUE_RING(3) + 32, VM_DESC);
    qtest_writeq(s->host, QUEUE_RING(3) + 40, VM_ARGS + 8);
    qtest_writeq(s->host, QUEUE_RING(3) + 56, VM_SIGNAL + 32);
    qtest_writeq(s->host, QUEUE_SIGNAL(1) + 16, GPGPU_VM_NOTIFY_VA + 8);
    qtest_writel(s->host, QUEUE_ARGS(1), VM_DATA + 4);
    qtest_writel(s->host, QUEUE_RING(3), (1 << 16) | GPGPU_AQL_HEADER);
    qtest_writel(s->host, QUEUE_WPTR(3), 1);
    queue_wait32(s, QUEUE_RPTR(3), 1);
    g_assert_cmpuint(qtest_readq(s->host, QUEUE_SIGNAL(1)), ==, 0);
    g_assert_cmphex(qpci_io_readl(s->dev, s->vram, 0x12004), ==, 0xaabb0001);
    g_assert_cmpuint(queue_manage(s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(3)), ==, 0);
}

/* 八条不同用户路径分别故障，验证原始地址、访问类型和完成状态。 */
static void test_gpuvm_fault_paths(void)
{
    const uint32_t errors[] = {GPGPU_QUEUE_ERROR_RING, GPGPU_QUEUE_ERROR_RETIRE,
        GPGPU_QUEUE_ERROR_DESCRIPTOR, GPGPU_QUEUE_ERROR_EXECUTOR,
        GPGPU_QUEUE_ERROR_EXECUTOR, GPGPU_QUEUE_ERROR_EXECUTOR,
        GPGPU_QUEUE_ERROR_SIGNAL, GPGPU_QUEUE_ERROR_MAILBOX};
    const uint32_t addresses[] = {VM_RING, VM_PROGRESS + 40, VM_DESC, VM_CODE,
        VM_ARGS, VM_DATA, VM_SIGNAL, GPGPU_VM_NOTIFY_VA};
    const uint32_t access[] = {GPGPU_VM_READ, GPGPU_VM_WRITE, GPGPU_VM_READ,
        GPGPU_VM_EXEC, GPGPU_VM_READ, GPGPU_VM_WRITE, GPGPU_VM_WRITE, GPGPU_VM_WRITE};
    const uint32_t phases[] = {GPGPU_QUEUE_ERROR_RING, GPGPU_QUEUE_ERROR_RETIRE,
        GPGPU_QUEUE_ERROR_DESCRIPTOR, GPGPU_VM_PHASE_FETCH, GPGPU_VM_PHASE_ARGS,
        GPGPU_VM_PHASE_DATA, GPGPU_QUEUE_ERROR_SIGNAL, GPGPU_QUEUE_ERROR_MAILBOX};

    for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
        CPTest s = {0};
        uint32_t reason;

        vm_test_prepare(&s, i);
        vm_test_submit(&s);
        queue_wait_error(&s, 2, errors[i]);
        reason = qpci_io_readl(s.dev, s.regs, GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_REASON);
        g_assert_cmpuint(reason, ==, (i == 0 || i == 2 || i == 4) ?
                         GPGPU_VM_FAULT_UNMAPPED : GPGPU_VM_FAULT_PERMISSION);
        g_assert_cmpuint(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_VMID), ==, 1);
        g_assert_cmphex(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_ADDR_LO), ==, addresses[i]);
        g_assert_cmpuint(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_ADDR_HI), ==, 0);
        g_assert_cmpuint(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_ACCESS), ==, access[i]);
        g_assert_cmpuint(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_PHASE), ==, phases[i]);
        g_assert_cmpuint(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, i == 1 || i == 7 ? 0 : 1);
        vm_test_other(&s);
        g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
        cp_stop(&s);
    }

    /* Host 仅在本测试中破坏尚未访问的表项，覆盖保留位和管理区越界。 */
    for (unsigned i = 0; i < 4; i++) {
        CPTest s = {0};
        uint32_t next = 0x28000, leaf;

        vm_test_prepare(&s, -1);
        leaf = vm_test_leaf_root(&s, VM_ROOT, VM_CODE, &next);
        if (i == 0) {
            qpci_io_writeq(s.dev, s.vram, leaf,
                            (1ULL << 63) | 0x11000 | GPGPU_VM_VALID | GPGPU_VM_PERMS);
        } else if (i == 1) {
            uint32_t table = qpci_io_readq(s.dev, s.vram, VM_ROOT) & GPGPU_VM_ADDR_MASK;
            uint64_t pde = qpci_io_readq(s.dev, s.vram, table + 2 * 8);

            qpci_io_writeq(s.dev, s.vram, table + 2 * 8, pde | GPGPU_VM_SYSTEM);
        } else if (i == 2) {
            qpci_io_writeq(s.dev, s.vram, leaf,
                            GPGPU_FIRMWARE_BASE | GPGPU_VM_VALID | GPGPU_VM_PERMS);
        } else {
            /* 先写本页最后四字节，再写未映射的下一页；已写结果必须保留。 */
            qpci_io_writel(s.dev, s.vram, 0x10004, 20);
            qpci_io_writel(s.dev, s.vram, 0x1100c, 0x0062a223);
            qpci_io_writel(s.dev, s.vram, 0x11010, 0x00100073);
        }
        vm_test_submit_data(&s, i == 3 ? VM_DATA + 4092 : VM_DATA);
        queue_wait_error(&s, 2, i == 1 ? GPGPU_QUEUE_ERROR_DESCRIPTOR :
                                       GPGPU_QUEUE_ERROR_EXECUTOR);
        g_assert_cmpuint(qpci_io_readl(s.dev, s.regs,
                         GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_REASON), ==,
                         i == 3 ? GPGPU_VM_FAULT_UNMAPPED : GPGPU_VM_FAULT_TABLE);
        g_assert_cmpuint(qtest_readq(s.host, QUEUE_SIGNAL(0)), ==, 1);
        if (i == 3) {
            g_assert_cmphex(qpci_io_readl(s.dev, s.vram, 0x12ffc), ==, 0xaabb0000);
            g_assert_cmphex(qpci_io_readl(s.dev, s.regs,
                             GPGPU_REG_VM_FAULT(2) + GPGPU_VM_FAULT_ADDR_LO), ==,
                             VM_DATA + 4096);
        }
        g_assert_cmpuint(queue_manage(&s, 1, GPGPU_UNMAP_QUEUES, QUEUE_MQD(2)), ==, 0);
        cp_stop(&s);
    }
}

static void register_gpuvm_tests(void)
{
    qtest_add_func("/gpgpu/gpuvm/translation", test_gpuvm_translation);
    qtest_add_func("/gpgpu/gpuvm/fault-paths", test_gpuvm_fault_paths);
}
