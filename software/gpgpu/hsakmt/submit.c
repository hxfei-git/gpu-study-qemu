/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <endian.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include "internal.h"


/*
 * gpgpuKmtHostFence() - 连接 Host 普通内存与 Doorbell 的访问顺序
 *
 * ARM64 使用 dmb sy 并约束编译器，其余主机使用顺序一致的原子屏障。
 */
void gpgpuKmtHostFence(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/*
 * queue_now_ns() - 取得单调递增的纳秒时间
 *
 * 供轮询间隔和相对超时计算使用，不受实时时钟调整影响。
 */
static uint64_t queue_now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/*
 * gpgpuKmtReserve() - 原子预留一个 AQL 包编号并等待旧槽归还
 *
 * 先递增 W，再等待自己的编号落入可复用范围；故障不回滚已预留编号。
 */
static GpgpuKmtStatus submit_Reserve(GpgpuKmtQueue *queue, uint32_t *reserved)
{
	uint32_t number, rptr, slots = queue->ring_size / sizeof(*queue->ring);
	uint64_t next_check = queue_now_ns();

	if (!reserved)
		return GPGPU_KMT_INVALID_PARAMETER;
	/* A57 小端共享普通内存上的 32 位原子取号；不以 W-R 判满。 */
	number = __atomic_fetch_add(queue->wptr, 1, __ATOMIC_RELAXED);
	*reserved = number;
	do {
		rptr = __atomic_load_n(queue->rptr, __ATOMIC_ACQUIRE);
		if ((uint32_t)(number - rptr) < slots)
			break;
		if (queue_now_ns() >= next_check) {
			struct gpgpu_queue_state state;
			GpgpuKmtStatus status = gpgpuKmtQueryQueue(queue, &state);

			if (status != GPGPU_KMT_SUCCESS || state.state != GPGPU_QUEUE_READY) {
				return status != GPGPU_KMT_SUCCESS ? status :
								     GPGPU_KMT_WAIT_FAILURE;
			}
			next_check = queue_now_ns() + 200000;
		}
		sched_yield();
	} while (1);
	gpgpuKmtHostFence();
	if (__atomic_load_n(&queue->ring[number % slots].full_header, __ATOMIC_ACQUIRE) !=
	    GPGPU_AQL_INVALID)
		return GPGPU_KMT_WAIT_FAILURE;
	return GPGPU_KMT_SUCCESS;
}

/*
 * gpgpuKmtPublish() - 发布已填好的 AQL 包并敲 Doorbell
 *
 * 先复制包体，屏障后完整发布 Header，再写零扩展的包编号；
 * 必须持有预留。
 */
static GpgpuKmtStatus submit_Publish(GpgpuKmtQueue *queue, uint32_t number, const GpgpuAqlPacket *packet)
{
	uint32_t slot = number % (queue->ring_size / sizeof(*queue->ring));
	GpgpuAqlPacket *dest = &queue->ring[slot];

	if (!packet)
		return GPGPU_KMT_INVALID_PARAMETER;
	memcpy((char *)dest + 4, (const char *)packet + 4, sizeof(*packet) - 4);
	gpgpuKmtHostFence();
	__atomic_store_n(&dest->full_header, packet->full_header, __ATOMIC_RELEASE);
	gpgpuKmtHostFence();
	*queue->doorbell = htole64(number);
	return GPGPU_KMT_SUCCESS;
}

/*
 * gpgpuKmtWaitSignal() - 等待 Signal 完成并同步结果可见性
 *
 * 先主动检查约 200 us，再阻塞等待事件；每次唤醒或超时均按规则复查
 * Signal。
 */
static GpgpuKmtStatus submit_WaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	uint64_t start = queue_now_ns();
	uint64_t deadline =
		timeout_ms == UINT32_MAX ? UINT64_MAX : start + (uint64_t)timeout_ms * 1000000;

	for (;;) {
		uint64_t now;
		GpgpuKmtStatus status;
		uint32_t remaining;

		if (__atomic_load_n(&signal->value, __ATOMIC_ACQUIRE) == 0) {
			gpgpuKmtHostFence();
			return GPGPU_KMT_SUCCESS;
		}
		now = queue_now_ns();
		if (now >= deadline)
			return GPGPU_KMT_WAIT_TIMEOUT;
		if (now - start < 200000)
			continue;
		remaining = deadline == UINT64_MAX ?
			UINT32_MAX :
			(uint32_t)((deadline - now + 999999) / 1000000);
		status = gpgpuKmtWaitEvent(event, remaining);
		if (status == GPGPU_KMT_WAIT_TIMEOUT) {
			/*
			 * Signal 先于 IH 发布；
			 * 事件超时后仍需取得最后一次完成值。
			 */
			if (__atomic_load_n(&signal->value, __ATOMIC_ACQUIRE) == 0) {
				gpgpuKmtHostFence();
				return GPGPU_KMT_SUCCESS;
			}
			return status;
		}
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		/*
		 * 唤醒仅提示复查，后续等待沿用第一次开始时的截止时间。
		 */
	}
}

/*
 * gpgpuKmtDrainQueue() - 等待队列所有预留包归还
 *
 * 调用方先停止生产；轮询 R/W 并检查队列错误，截止时间到达返回超时。
 */
static GpgpuKmtStatus submit_DrainQueue(GpgpuKmtQueue *queue, uint32_t timeout_ms)
{
	uint64_t deadline = timeout_ms == UINT32_MAX ? UINT64_MAX :
		queue_now_ns() + (uint64_t)timeout_ms * 1000000;

	do {
		struct gpgpu_queue_state state;
		GpgpuKmtStatus status;

		if (__atomic_load_n(queue->rptr, __ATOMIC_ACQUIRE) ==
		    __atomic_load_n(queue->wptr, __ATOMIC_ACQUIRE)) {
			gpgpuKmtHostFence();
			return GPGPU_KMT_SUCCESS;
		}
		status = gpgpuKmtQueryQueue(queue, &state);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		if (state.state == GPGPU_QUEUE_FAULTED)
			return GPGPU_KMT_WAIT_FAILURE;
		sched_yield();
	} while (queue_now_ns() < deadline);
	return GPGPU_KMT_WAIT_TIMEOUT;
}

/* 库调用期间保留连接；调用方在销毁 Queue 前停止所有提交线程。 */
static bool queue_valid(GpgpuKmtQueue *queue)
{
	return queue && queue->context && queue->ring && queue->rptr && queue->wptr &&
		queue->doorbell && queue->ring_size >= GPGPU_QUEUE_RING_MIN &&
		queue->ring_size <= GPGPU_QUEUE_RING_MAX &&
		!(queue->ring_size & (queue->ring_size - 1));
}

GpgpuKmtStatus gpgpuKmtReserve(GpgpuKmtQueue *queue, uint32_t *number)
{
	GpgpuKmtStatus status;

	if (!queue_valid(queue) || !number)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = gpgpu_kmt_begin(queue->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = submit_Reserve(queue, number);
	gpgpu_kmt_end(queue->context);
	return status;
}

GpgpuKmtStatus gpgpuKmtPublish(GpgpuKmtQueue *queue, uint32_t number,
                             const GpgpuAqlPacket *packet)
{
	GpgpuKmtStatus status;

	if (!queue_valid(queue) || !packet)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = gpgpu_kmt_begin(queue->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = submit_Publish(queue, number, packet);
	gpgpu_kmt_end(queue->context);
	return status;
}

GpgpuKmtStatus gpgpuKmtWaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event,
                                uint32_t timeout_ms)
{
	GpgpuKmtStatus status;

	if (!signal || ((uintptr_t)signal & 7) || !event || !event->context)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = gpgpu_kmt_begin(event->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = submit_WaitSignal(signal, event, timeout_ms);
	gpgpu_kmt_end(event->context);
	return status;
}

GpgpuKmtStatus gpgpuKmtDrainQueue(GpgpuKmtQueue *queue, uint32_t timeout_ms)
{
	GpgpuKmtStatus status;

	if (!queue_valid(queue))
		return GPGPU_KMT_INVALID_PARAMETER;
	status = gpgpu_kmt_begin(queue->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = submit_DrainQueue(queue, timeout_ms);
	gpgpu_kmt_end(queue->context);
	return status;
}
