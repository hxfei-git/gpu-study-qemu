// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GPGPU KMT：预留、发布、等待与排空
 *
 * 多生产者提交协议（与 CP 固件的取包规则配合）：
 *
 *   1. Reserve：原子 W += 1 取得编号 N；等 N - R < 槽数，即槽 N mod 槽数已被
 *      CP 归还；此时槽的 Header 应为 INVALID
 *   2. 调用者填写包体（Header 仍为 INVALID）
 *   3. Publish：屏障后以完整 32 位写入有效 Header，再屏障，写 Doorbell
 *   4. WaitSignal：Signal 由 1 变 0 表示完成
 *
 * W 可能领先 R 超过槽数（取号后在等空槽的生产者），因此不能用 W - R 判满。
 */
#include <endian.h>
#include <sched.h>
#include <time.h>
#include "kmt_internal.h"

/* 先主动轮询的时长，之后改为阻塞等待事件，单位纳秒。 */
#define KMT_ACTIVE_WAIT_NS 200000ull
/* 等空槽时查询队列状态的间隔，单位纳秒。 */
#define KMT_QUEUE_CHECK_NS 200000ull
#define KMT_NS_PER_MS 1000000ull

void gpgpuKmtHostFence(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* timeout_ms 为 UINT32_MAX 表示不设期限。 */
static uint64_t deadline_ns(uint32_t timeout_ms)
{
	return timeout_ms == UINT32_MAX ? UINT64_MAX : now_ns() + timeout_ms * KMT_NS_PER_MS;
}

/* 队列是否已出错；查询失败也视为不可继续。 */
static GpgpuKmtStatus queue_check(GpgpuKmtQueue *q)
{
	struct gpgpu_queue_state state;
	GpgpuKmtStatus status = gpgpuKmtQueryQueue(q, &state);

	if (status != GPGPU_KMT_SUCCESS)
		return status;
	return state.state == GPGPU_QUEUE_READY ? GPGPU_KMT_SUCCESS : GPGPU_KMT_WAIT_FAILURE;
}

/*
 * 1. 原子取号；2. 等待对应槽被 CP 归还，期间定期检查队列是否出错；
 * 3. 确认槽的 Header 为 INVALID。
 */
GpgpuKmtStatus gpgpuKmtReserve(GpgpuKmtQueue *q, uint32_t *number)
{
	uint64_t next_check;
	GpgpuKmtStatus status;
	uint32_t n, rptr;

	if (!q || !number)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(q->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;

	/* 1. 取号。 */
	n = __atomic_fetch_add(q->wptr, 1, __ATOMIC_RELAXED);
	*number = n;

	/* 2. 等空槽。 */
	next_check = now_ns();
	for (;;) {
		rptr = __atomic_load_n(q->rptr, __ATOMIC_ACQUIRE);
		if (n - rptr < q->slots)
			break;
		if (now_ns() >= next_check) {
			status = queue_check(q);
			if (status != GPGPU_KMT_SUCCESS)
				goto out;
			next_check = now_ns() + KMT_QUEUE_CHECK_NS;
		}
		sched_yield();
	}

	/* 3. 槽已归还。 */
	gpgpuKmtHostFence();
	if (__atomic_load_n(&q->ring[n % q->slots].full_header, __ATOMIC_ACQUIRE) !=
	    htole32(GPGPU_AQL_INVALID))
		status = GPGPU_KMT_WAIT_FAILURE;
out:
	kmt_leave(q->context);
	return status;
}

/* 1. 写包体；2. 屏障后发布 Header；3. 屏障后写 Doorbell（零扩展的包编号）。 */
GpgpuKmtStatus gpgpuKmtPublish(GpgpuKmtQueue *q, uint32_t number, const GpgpuAqlPacket *packet)
{
	GpgpuAqlPacket *slot;
	GpgpuKmtStatus status;

	if (!q || !packet)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(q->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	slot = &q->ring[number % q->slots];

	/* 1. 包体。 */
	memcpy((char *)slot + sizeof(slot->full_header),
	       (const char *)packet + sizeof(packet->full_header),
	       sizeof(*packet) - sizeof(packet->full_header));

	/* 2. Header。 */
	gpgpuKmtHostFence();
	__atomic_store_n(&slot->full_header, packet->full_header, __ATOMIC_RELEASE);

	/* 3. Doorbell。 */
	gpgpuKmtHostFence();
	*q->doorbell = htole64(number);
	kmt_leave(q->context);
	return GPGPU_KMT_SUCCESS;
}

/* Signal 已完成时建立读取结果的顺序。 */
static bool signal_done(GpgpuSignal *signal)
{
	if (__atomic_load_n(&signal->value, __ATOMIC_ACQUIRE) != 0)
		return false;
	gpgpuKmtHostFence();
	return true;
}

/*
 * 1. 主动轮询 Signal 至多 KMT_ACTIVE_WAIT_NS；
 * 2. 之后阻塞等待事件，按剩余时间换算毫秒；
 * 3. 每次唤醒或事件超时都复查 Signal：Signal 先于 IH 发布，事件超时时
 *    Signal 可能已经完成。
 */
GpgpuKmtStatus gpgpuKmtWaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	uint64_t start = now_ns(), deadline = deadline_ns(timeout_ms);
	GpgpuKmtStatus status;

	if (!signal || ((uintptr_t)signal & 7) || !event)
		return GPGPU_KMT_INVALID_PARAMETER;
	for (;;) {
		uint64_t now;
		uint32_t remaining_ms;

		/* 1. 轮询。 */
		if (signal_done(signal))
			return GPGPU_KMT_SUCCESS;
		now = now_ns();
		if (now >= deadline)
			return GPGPU_KMT_WAIT_TIMEOUT;
		if (now - start < KMT_ACTIVE_WAIT_NS)
			continue;

		/* 2. 阻塞。 */
		remaining_ms = deadline == UINT64_MAX ?
				       UINT32_MAX :
				       (uint32_t)((deadline - now + KMT_NS_PER_MS - 1) / KMT_NS_PER_MS);
		status = gpgpuKmtWaitEvent(event, remaining_ms);

		/* 3. 复查。 */
		if (status == GPGPU_KMT_WAIT_TIMEOUT)
			return signal_done(signal) ? GPGPU_KMT_SUCCESS : status;
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
}

/* 轮询 R == W；队列出错时返回 WAIT_FAILURE。调用者已停止提交。 */
GpgpuKmtStatus gpgpuKmtDrainQueue(GpgpuKmtQueue *q, uint32_t timeout_ms)
{
	uint64_t deadline = deadline_ns(timeout_ms);
	GpgpuKmtStatus status;

	if (!q)
		return GPGPU_KMT_INVALID_PARAMETER;
	do {
		if (__atomic_load_n(q->rptr, __ATOMIC_ACQUIRE) ==
		    __atomic_load_n(q->wptr, __ATOMIC_ACQUIRE)) {
			gpgpuKmtHostFence();
			return GPGPU_KMT_SUCCESS;
		}
		status = queue_check(q);
		if (status != GPGPU_KMT_SUCCESS)
			return status;
		sched_yield();
	} while (now_ns() < deadline);
	return GPGPU_KMT_WAIT_TIMEOUT;
}
