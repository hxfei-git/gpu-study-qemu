/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * GPGPU 远端用例共用的进程与 socket 生命周期。
 */

#ifndef TESTS_QTEST_GPGPU_REMOTE_H
#define TESTS_QTEST_GPGPU_REMOTE_H

#include <glib/gstdio.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

typedef struct GPGPURemote {
    QTestState *qts;
    char *directory;
    char *socket;
    char *log;
} GPGPURemote;

static inline void gpgpu_remote_start(GPGPURemote *remote)
{
    static unsigned int sequence;
    const char *logs = g_getenv("QTEST_GPGPU_REMOTE_LOG_DIR");
    g_autofree char *args = NULL;
    g_autofree char *quoted_log = NULL;

    g_assert_nonnull(g_getenv("QTEST_GPGPU_REMOTE_BINARY"));
    remote->directory = g_dir_make_tmp("gpgpu-qtest-XXXXXX", NULL);
    g_assert_nonnull(remote->directory);
    remote->socket = g_build_filename(remote->directory, "gpu.sock", NULL);
    if (logs) {
        g_assert_cmpint(g_mkdir_with_parents(logs, 0700), ==, 0);
    }
    remote->log = g_strdup_printf("%s/device-%u.log",
                                  logs ? logs : remote->directory, ++sequence);
    quoted_log = g_shell_quote(remote->log);
    args = g_strdup_printf("-machine x-remote,vfio-user=on,"
                           "auto-shutdown=off -device gpgpu,id=gpu0 "
                           "-accel tcg -nodefaults -serial none 2>%s",
                           quoted_log);
    remote->qts = qtest_init_ext("QTEST_GPGPU_REMOTE_BINARY", args, NULL, true);

    /* 先建立 QMP，再创建服务端，避免握手被阻塞。 */
    qtest_qmp_assert_success(remote->qts,
        "{ 'execute': 'object-add', 'arguments': {"
        "'qom-type': 'x-vfio-user-server', 'id': 'vfu0', 'device': 'gpu0',"
        "'socket': { 'type': 'unix', 'path': %s } } }", remote->socket);
    g_assert_true(g_file_test(remote->socket, G_FILE_TEST_EXISTS));
}

static inline char *gpgpu_remote_device_option(GPGPURemote *remote)
{
    g_autofree char *json = g_strdup_printf(
        "{\"driver\":\"vfio-user-pci\",\"bus\":\"pcie.0\","
        "\"x-dma-via-socket\":true,"
        "\"socket\":{\"type\":\"unix\",\"path\":\"%s\"}}",
        remote->socket);

    return g_shell_quote(json);
}

static inline void gpgpu_remote_stop(GPGPURemote *remote)
{
    if (!remote->qts) {
        return;
    }
    qtest_quit(remote->qts);
    g_unlink(remote->socket);
    if (!g_getenv("QTEST_GPGPU_REMOTE_LOG_DIR")) {
        g_unlink(remote->log);
    }
    g_rmdir(remote->directory);
    g_free(remote->log);
    g_free(remote->socket);
    g_free(remote->directory);
}

#endif
