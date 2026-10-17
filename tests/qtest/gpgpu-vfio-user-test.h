/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * 控制 vfio-user 报文顺序，验证请求交错。
 */
#ifndef TESTS_QTEST_GPGPU_VFIO_USER_TEST_H
#define TESTS_QTEST_GPGPU_VFIO_USER_TEST_H

#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/sockets.h"
#include "hw/vfio-user/protocol.h"

typedef struct InterleaveFrame {
    uint16_t id;
    uint16_t command;
    uint32_t flags;
    uint32_t error;
    size_t size;
    uint8_t *body;
} InterleaveFrame;

static void interleave_io(int fd, void *buffer, size_t size, bool send_data)
{
    uint8_t *p = buffer;

    while (size) {
        ssize_t n = send_data ? send(fd, p, size, MSG_NOSIGNAL) :
                                recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(n, >, 0);
        p += n;
        size -= n;
    }
}

static void interleave_send(int fd, uint16_t id, uint16_t command,
                            uint32_t flags, void *body, size_t size)
{
    uint8_t header[16] = { 0 };

    stw_le_p(header, id);
    stw_le_p(header + 2, command);
    stl_le_p(header + 4, sizeof(header) + size);
    stl_le_p(header + 8, flags);
    interleave_io(fd, header, sizeof(header), true);
    if (size) {
        interleave_io(fd, body, size, true);
    }
}

static InterleaveFrame interleave_recv(int fd)
{
    uint8_t header[16];
    InterleaveFrame frame;
    uint32_t total;

    interleave_io(fd, header, sizeof(header), false);
    total = ldl_le_p(header + 4);
    g_assert_cmpuint(total, >=, sizeof(header));
    g_assert_cmpuint(total, <=, 4096);
    frame.id = lduw_le_p(header);
    frame.command = lduw_le_p(header + 2);
    frame.flags = ldl_le_p(header + 8);
    frame.error = ldl_le_p(header + 12);
    frame.size = total - sizeof(header);
    frame.body = g_malloc(frame.size);
    if (frame.size) {
        interleave_io(fd, frame.body, frame.size, false);
    }
    return frame;
}

static InterleaveFrame interleave_reply(int fd, uint16_t id)
{
    InterleaveFrame reply = interleave_recv(fd);

    g_assert_cmpuint(reply.id, ==, id);
    g_assert_cmpuint(reply.flags & VFIO_USER_TYPE, ==, VFIO_USER_REPLY);
    g_assert_cmpuint(reply.flags & VFIO_USER_ERROR, ==, 0);
    g_assert_cmpuint(reply.error, ==, 0);
    return reply;
}

static void interleave_write(int fd, uint16_t id, unsigned region,
                             uint64_t offset, uint32_t value, bool wait_reply)
{
    uint8_t body[20];

    stq_le_p(body, offset);
    stl_le_p(body + 8, region);
    stl_le_p(body + 12, 4);
    stl_le_p(body + 16, value);
    interleave_send(fd, id, VFIO_USER_REGION_WRITE, VFIO_USER_REQUEST,
                    body, sizeof(body));
    if (wait_reply) {
        InterleaveFrame reply = interleave_reply(fd, id);
        g_free(reply.body);
    }
}

static void interleave_read(int fd, uint16_t id, unsigned region,
                            uint64_t offset, unsigned size)
{
    uint8_t body[16];

    stq_le_p(body, offset);
    stl_le_p(body + 8, region);
    stl_le_p(body + 12, size);
    interleave_send(fd, id, VFIO_USER_REGION_READ, VFIO_USER_REQUEST,
                    body, sizeof(body));
}

static void test_window_interleave(void)
{
    GPGPURemote remote = { 0 };
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval timeout = { .tv_sec = 5 };
    uint16_t id = 100, start_id, read_id;
    uint8_t version[] = { 0, 0, 1, 0 };
    uint8_t map[32] = { 0 };
    const uint64_t iova = UINT64_C(0x112340000);
    const uint64_t expected = UINT64_C(0xf12345675a6b7c8d);
    uint32_t firmware[] = {
        (GPGPU_CP_IOVA_BASE_LO & 0xfffff000u) | 0x2b7, /* t0 = 配置地址 */
        0x02029293,                   /* slli t0, t0, 32 */
        0x0202d293,                   /* srli t0, t0, 32，零扩展地址 */
        0x12345337,                   /* t1 = IOVA 低位 */
        0x0062a023,                   /* sw t1, 0(t0) */
        0x00100313,                   /* t1 = IOVA 高位 */
        0x0062a223,                   /* sw t1, 4(t0) */
        (GPGPU_CP_HOST_WINDOW_BASE & 0xfffff000u) | 0x2b7,
        0x02029293,                   /* slli t0, t0, 32 */
        0x0202d293,                   /* srli t0, t0, 32，零扩展地址 */
        0x0002b303,                   /* ld t1, 0(t0)，发起 8 字节读取 */
        0x000062b7,                   /* t0 = 结果地址 0x6000 */
        0x0062b023,                   /* sd t1, 0(t0) */
        0x10500073,                   /* wfi */
        0xffdff06f,                   /* 回到 wfi */
    };
    InterleaveFrame frame;
    bool start_replied = false, read_replied = false;
    int fd;

    gpgpu_remote_start(&remote);
    g_assert_cmpuint(strlen(remote.socket), <, sizeof(address.sun_path));
    pstrcpy(address.sun_path, sizeof(address.sun_path), remote.socket);
    fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                              &timeout, sizeof(timeout)), ==, 0);
    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                              &timeout, sizeof(timeout)), ==, 0);
    g_assert_cmpint(connect(fd, (struct sockaddr *)&address,
                           sizeof(address)), ==, 0);

    interleave_send(fd, id, VFIO_USER_VERSION, VFIO_USER_REQUEST,
                    version, sizeof(version));
    frame = interleave_reply(fd, id++);
    g_free(frame.body);
    stl_le_p(map, sizeof(map));
    stl_le_p(map + 4, 3);             /* 允许设备读写，无共享 fd */
    stq_le_p(map + 16, iova);
    stq_le_p(map + 24, 4096);
    interleave_send(fd, id, VFIO_USER_DMA_MAP, VFIO_USER_REQUEST,
                    map, sizeof(map));
    frame = interleave_reply(fd, id++);
    g_free(frame.body);
    /* 为 BAR 配置互不重叠的 PCI 地址，保持区域查找有效。 */
    interleave_write(fd, id++, 7, PCI_BASE_ADDRESS_0, 0x10000000, true);
    interleave_write(fd, id++, 7, PCI_BASE_ADDRESS_2, 0x20000000, true);
    /* 配置空间区域编号 7，允许 PCI bus master 发出 DMA。 */
    interleave_write(fd, id++, 7, PCI_COMMAND,
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, true);
    interleave_write(fd, id++, 0, GPGPU_REG_GLOBAL_CTRL,
                     GPGPU_CTRL_ENABLE, true);
    for (size_t i = 0; i < ARRAY_SIZE(firmware); i++) {
        interleave_write(fd, id++, 2, 0x4000 + i * 4, firmware[i], true);
    }
    interleave_write(fd, id++, 2, 0x6000, 0, true);
    interleave_write(fd, id++, 2, 0x6004, 0, true);
    interleave_write(fd, id++, 0, GPGPU_REG_CP_RESET_VECTOR, 0x4000, true);
    start_id = id++;
    interleave_write(fd, start_id, 0, GPGPU_REG_CP_CTRL, 0, false);

    /* CP 可能在启动命令的回复之前就已经发出了 DMA。 */
    for (;;) {
        frame = interleave_recv(fd);
        if ((frame.flags & VFIO_USER_TYPE) == VFIO_USER_REQUEST) {
            g_assert_cmpuint(frame.command, ==, VFIO_USER_DMA_READ);
            g_assert_cmpuint(frame.size, ==, 16);
            g_assert_cmphex(ldq_le_p(frame.body), ==, iova);
            g_assert_cmpuint(ldq_le_p(frame.body + 8), ==, 8);
            break;
        }
        g_assert_false(start_replied);
        g_assert_cmpuint(frame.id, ==, start_id);
        g_assert_cmpuint(frame.error, ==, 0);
        start_replied = true;
        g_free(frame.body);
    }

    /* 先发 Host BAR 请求，再发 DMA 回复，强制发生交错。 */
    read_id = id++;
    interleave_read(fd, read_id, 0, GPGPU_REG_DEV_ID, 4);
    {
        uint8_t reply[24];

        memcpy(reply, frame.body, 16);
        stq_le_p(reply + 16, expected);
        interleave_send(fd, frame.id, VFIO_USER_DMA_READ, VFIO_USER_REPLY,
                        reply, sizeof(reply));
        g_free(frame.body);
    }
    /* 不再发新消息，验证 BH 能处理暂存请求。 */
    while (!start_replied || !read_replied) {
        frame = interleave_recv(fd);
        g_assert_cmpuint(frame.flags & VFIO_USER_TYPE, ==, VFIO_USER_REPLY);
        g_assert_cmpuint(frame.error, ==, 0);
        if (frame.id == start_id) {
            g_assert_false(start_replied);
            start_replied = true;
        } else {
            g_assert_cmpuint(frame.id, ==, read_id);
            g_assert_false(read_replied);
            g_assert_cmpuint(frame.size, ==, 20);
            g_assert_cmphex(ldl_le_p(frame.body + 16), ==,
                            GPGPU_DEV_ID_VALUE);
            read_replied = true;
        }
        g_free(frame.body);
    }
    {
        int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        uint64_t result;

        do {
            interleave_read(fd, id, 2, 0x6000, 8);
            frame = interleave_reply(fd, id++);
            g_assert_cmpuint(frame.size, ==, 24);
            result = ldq_le_p(frame.body + 16);
            g_free(frame.body);
            if (result == expected) {
                break;
            }
            g_usleep(1000);
        } while (g_get_monotonic_time() < deadline);
        g_assert_cmphex(result, ==, expected);
    }
    interleave_write(fd, id++, 0, GPGPU_REG_CP_CTRL,
                     GPGPU_CP_RESET_HOLD, true);
    close(fd);
    gpgpu_remote_stop(&remote);
}

#endif
