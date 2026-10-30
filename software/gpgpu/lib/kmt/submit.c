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
/* 每毫秒的纳秒数。 */
#define KMT_NS_PER_MS 1000000ull

void gpgpuKmtHostFence(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* CLOCK_MONOTONIC 的当前时间，单位纳秒。 */
static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* 把相对期限换成绝对时间；timeout_ms 为 UINT32_MAX 表示不设期限。 */
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
 * gpgpuKmtReserve() - 为一个 AQL 包取号，并等到它的槽可以写
 *
 * 多个 Host 线程可以同时向同一条队列提交，编号靠原子加法分配，
 * 不加锁。取到编号 N 只表示轮到自己，槽 N mod 槽数里可能还是 CP
 * 没处理完的旧包，必须等 CP 回报的 R 追上来才能写：
 *
 * 1. 对共享 W 做原子 fetch-add，旧值就是本包编号 N；
 * 2. 循环读 R，直到 N - R < 槽数，即 CP 已消费并归还这个槽。每隔
 *    KMT_QUEUE_CHECK_NS 查询一次队列状态：队列出错后 CP 不再推进 R，
 *    此时返回 WAIT_FAILURE，避免永远等下去；
 * 3. CP 归还槽时已把 Header 写回 INVALID，这里再确认一次，读到其他值
 *    说明协议被破坏，同样返回 WAIT_FAILURE。
 *
 * 取号无法撤销。失败时编号 N 留下空洞，CP 会一直停在这个 INVALID 包上，
 * 该队列不能再使用。
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

	/* 1. 原子加 1 并取旧值，多个线程同时取号也不会重复。 */
	n = __atomic_fetch_add(q->wptr, 1, __ATOMIC_RELAXED);
	*number = n;

	/*
	 * 2. N - R 按 32 位无符号差计算，W 回绕后仍然成立。
	 *    acquire 读 R，使后续对该槽的访问排在看到 R 前进之后。
	 */
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

	/*
	 * 3. 屏障后读 Header。CP 先写回 INVALID 再推进 R，
	 *    这里必须读到 INVALID。
	 */
	gpgpuKmtHostFence();
	if (__atomic_load_n(&q->ring[n % q->slots].full_header, __ATOMIC_ACQUIRE) !=
	    htole32(GPGPU_AQL_INVALID))
		status = GPGPU_KMT_WAIT_FAILURE;
out:
	kmt_leave(q->context);
	return status;
}

/*
 * gpgpuKmtPublish() - 把包写进 Reserve 得到的槽并通知 CP
 *
 * CP 按 Header 是否为 INVALID 判断包是否已发布，所以 Header 必须最后写：
 *
 * 1. 先写 Header 之后的包体。此时 Header 仍是 INVALID，CP 即使检查到
 *    这个槽也只会等待；
 * 2. 屏障保证包体先于 Header 可见，再以完整 32 位 release 写入
 *    full_header（低 16 位 Header、高 16 位维度数），CP 不会读到半个
 *    Header；
 * 3. 再次屏障保证 Header 先于 Doorbell 可见，然后写入零扩展的 64 位
 *    包编号。Doorbell 只起提醒作用，CP 仍从 R 开始按序检查 Header，
 *    多个线程乱序敲 Doorbell 也不会漏包。
 */
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

	/* 1. 跳过开头 4 字节的 full_header，只复制包体。 */
	memcpy((char *)slot + sizeof(slot->full_header),
	       (const char *)packet + sizeof(packet->full_header),
	       sizeof(*packet) - sizeof(packet->full_header));

	/* 2. 包体可见后发布 Header，CP 从此可以取走这个包。 */
	gpgpuKmtHostFence();
	__atomic_store_n(&slot->full_header, packet->full_header, __ATOMIC_RELEASE);

	/* 3. Header 可见后再敲 Doorbell，CP 被提醒时一定能读到有效 Header。 */
	gpgpuKmtHostFence();
	*q->doorbell = htole64(number);
	kmt_leave(q->context);
	return GPGPU_KMT_SUCCESS;
}

/*
 * Signal 值读到 0 才算完成。返回 true 前再加一次屏障，保证调用者
 * 随后读到的输出是 CP 发布 Signal 之前已经写回的数据。
 */
static bool signal_done(GpgpuSignal *signal)
{
	if (__atomic_load_n(&signal->value, __ATOMIC_ACQUIRE) != 0)
		return false;
	gpgpuKmtHostFence();
	return true;
}

/*
 * wait_signal() - 等待 Signal 从 1 变为 0
 *
 * 任务往往很快完成，直接睡眠要多付一次系统调用和唤醒的代价，
 * 所以先忙等一小段，再交给驱动的事件等待：
 *
 * 1. 主动轮询 Signal 至多 KMT_ACTIVE_WAIT_NS（200 μs），总期限到了就
 *    返回 WAIT_TIMEOUT；
 * 2. 之后调用 gpgpuKmtWaitEvent() 阻塞，期限为剩余时间向上取整的毫秒数，
 *    不设期限时传 UINT32_MAX；
 * 3. 事件被通知后回到第 1 步复查 Signal。事件等待超时也复查一次：CP 先写
 *    Signal、再写通知槽和 IH 记录，Signal 可能已经是 0。其他失败（例如
 *    队列出错时的 WAIT_FAILURE）直接返回。
 *
 * 调用期间登记为进行中的库调用，最后一次 Close 不会在轮询途中
 * 解除 Signal 的映射。
 */
static GpgpuKmtStatus wait_signal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	uint64_t start = now_ns(), deadline = deadline_ns(timeout_ms);
	GpgpuKmtStatus status;

	for (;;) {
		uint64_t now;
		uint32_t remaining_ms;

		/*
		 * 1. 先查 Signal，再查总期限；
		 *    开始等待不满 200 μs 时继续忙等。
		 */
		if (signal_done(signal))
			return GPGPU_KMT_SUCCESS;
		now = now_ns();
		if (now >= deadline)
			return GPGPU_KMT_WAIT_TIMEOUT;
		if (now - start < KMT_ACTIVE_WAIT_NS)
			continue;

		/* 2. 剩余时间向上取整成毫秒，避免早于期限返回。 */
		remaining_ms = deadline == UINT64_MAX ?
				       UINT32_MAX :
				       (uint32_t)((deadline - now + KMT_NS_PER_MS - 1) / KMT_NS_PER_MS);
		status = gpgpuKmtWaitEvent(event, remaining_ms);

		/*
		 * 3. 超时时最后复查一次 Signal；
		 *    事件已通知则回到循环开头复查。
		 */
		if (status == GPGPU_KMT_WAIT_TIMEOUT)
			return signal_done(signal) ? GPGPU_KMT_SUCCESS : status;
		if (status != GPGPU_KMT_SUCCESS)
			return status;
	}
}

GpgpuKmtStatus gpgpuKmtWaitSignal(GpgpuSignal *signal, GpgpuKmtEvent *event, uint32_t timeout_ms)
{
	KmtEvent *e = kmt_event(event);
	GpgpuKmtStatus status;

	if (!signal || ((uintptr_t)signal & 7) || !e)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(e->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = wait_signal(signal, event, timeout_ms);
	kmt_leave(e->context);
	return status;
}

/*
 * 轮询到 R == W，即所有已取号的包都已被 CP 消费。调用者必须
 * 已停止提交，否则 W 还会继续增长。每轮查询一次队列状态：
 * 队列出错后 R 不再前进，直接返回 WAIT_FAILURE。
 */
static GpgpuKmtStatus drain_queue(GpgpuKmtQueue *q, uint32_t timeout_ms)
{
	uint64_t deadline = deadline_ns(timeout_ms);
	GpgpuKmtStatus status;

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

GpgpuKmtStatus gpgpuKmtDrainQueue(GpgpuKmtQueue *q, uint32_t timeout_ms)
{
	GpgpuKmtStatus status;

	if (!q)
		return GPGPU_KMT_INVALID_PARAMETER;
	status = kmt_enter(q->context);
	if (status != GPGPU_KMT_SUCCESS)
		return status;
	status = drain_queue(q, timeout_ms);
	kmt_leave(q->context);
	return status;
}
