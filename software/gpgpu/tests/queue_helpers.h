/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPGPU_QUEUE_HELPERS_H
#define GPGPU_QUEUE_HELPERS_H

#include "gpgpu_kmt.h"

/*
 * 实验调用方固定最多 4 个提交线程，每线程最多保留一个未发布编号。
 */
/* 并发提交测试的线程数，每线程最多保留一个未发布编号。 */
#define GPGPU_TEST_SUBMIT_THREADS 4
/* 连接 Host 普通内存与 Doorbell 的访问顺序。 */
void queueHostFence(void);
/* 取得单调递增的纳秒时间。 */
uint64_t queueNowNs(void);
/* 失败时保留已预留的 W，不填补编号；调用方停止使用故障队列。 */
/* 原子预留一个 AQL 包编号并等待旧槽归还。 */
GpgpuKmtStatus queueReserve(GpgpuKmtQueue *queue, uint32_t *number);
/* 发布已填好的 AQL 包并敲 Doorbell。 */
void queuePublish(GpgpuKmtQueue *queue, uint32_t number, const GpgpuAqlPacket *packet);
/* 等待 Signal 完成并同步结果可见性。 */
GpgpuKmtStatus queueWaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms);
/* 等待队列所有预留包归还。 */
GpgpuKmtStatus queueDrain(GpgpuKmtQueue *queue, uint32_t timeout_ms);
#endif
