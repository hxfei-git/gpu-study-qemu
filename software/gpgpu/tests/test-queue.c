/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "queue_helpers.h"
#include "vector_add.h"
#include "matmul.h"
#include "relu.h"

/* 核对测试条件；失败时打印表达式和位置并终止测试。 */
#define CHECK(expr)                                                                             \
	do {                                                                                    \
		if (!(expr)) {                                                                  \
			fprintf(stderr, "队列检查失败 %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			exit(1);                                                                \
		}                                                                               \
	} while (0)
/* 核对 HSAKMT 调用成功；失败时报告状态并终止测试。 */
#define OK(expr)                                                                                   \
	do {                                                                                       \
		GpgpuKmtStatus result_ = (expr);                                                   \
		if (result_ != GPGPU_KMT_SUCCESS) {                                                \
			fprintf(stderr, "队列操作失败 %s:%d: %s: %s\n", __FILE__, __LINE__, #expr, \
				gpgpuKmtStatusName(result_));                                      \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/**
 * struct QueueFixture - 队列验收共用的连接、队列与数据缓冲区
 * @context: 测试持有的共享设备连接。
 * @ring: 两条队列各自独占的 Ring 内存。
 * @progress: 两条队列各自独占的 R/W 内存。
 * @staging: 上传代码和检查结果的 Host 暂存对象。
 * @vram: 保存代码与计算结果的 VRAM 对象。
 * @queue: 最多两条存活的用户队列。
 * @queues: 已成功建立的队列数，供清理路径使用。
 */
typedef struct QueueFixture {
	GpgpuKmtContext *context;
	GpgpuKmtMemory ring[2];
	GpgpuKmtMemory progress[2];
	GpgpuKmtMemory staging;
	GpgpuKmtMemory vram;
	GpgpuKmtQueue queue[2];
	unsigned int queues;
} QueueFixture;

/**
 * struct QueueTask - 一次测试任务的包、Signal 与事件
 * @memory: 保存参数与 Signal 的 Host 内存对象。
 * @event: 与源队列绑定的完成通知事件。
 * @signal: memory 内的 Signal CPU 映射。
 * @packet: 准备发布的 AQL 包副本。
 * @queue: 本任务提交到的用户队列。
 * @number: 原子预留得到的包编号。
 */
typedef struct QueueTask {
	GpgpuKmtMemory memory;
	GpgpuKmtEvent event;
	GpgpuSignal *signal;
	GpgpuAqlPacket packet;
	GpgpuKmtQueue *queue;
	uint32_t number;
} QueueTask;

/*
 * fixture_init() - 建立队列测试夹具
 *
 * 校验共享连接与 ABI，申请数据缓冲区，再为指定队列分配独立 Ring
 * 和进度。
 */
static void fixture_init(QueueFixture *f, unsigned int queues, uint32_t ring_bytes)
{
	struct gpgpu_queue_info info;
	GpgpuKmtContext *reopened;

	memset(f, 0, sizeof(*f));
	OK(gpgpuKmtOpen(NULL, &f->context));
	OK(gpgpuKmtOpen(NULL, &reopened));
	CHECK(reopened == f->context);
	OK(gpgpuKmtClose(reopened));
	OK(gpgpuKmtGetInfo(f->context, &info));
	CHECK(info.version == GPGPU_QABI_VERSION && info.max_queues == 2 &&
	      info.progress_bits == 32 && info.host_address_bits == 64 &&
	      info.firmware_base == GPGPU_FIRMWARE_BASE);
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 65536, 8, &f->staging));
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_VRAM, 65536, 8, &f->vram));
	f->queues = queues;
	for (unsigned int i = 0; i < queues; i++) {
		OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, ring_bytes, 64, &f->ring[i]));
		OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 16, 8, &f->progress[i]));
		OK(gpgpuKmtCreateQueue(f->context, &f->ring[i], 0, ring_bytes, &f->progress[i], 0,
				       8, &f->queue[i]));
	}
}

/*
 * fixture_destroy() - 清理已完成测试的夹具
 *
 * 先排空并卸载存活队列，再释放各内存对象与连接。
 */
static void fixture_destroy(QueueFixture *f)
{
	for (unsigned int i = 0; i < f->queues; i++) {
		if (f->queue[i].context) {
			OK(queueDrain(&f->queue[i], 10000));
			OK(gpgpuKmtDestroyQueue(&f->queue[i]));
		}
		OK(gpgpuKmtFreeMemory(&f->ring[i]));
		OK(gpgpuKmtFreeMemory(&f->progress[i]));
	}
	OK(gpgpuKmtFreeMemory(&f->staging));
	OK(gpgpuKmtFreeMemory(&f->vram));
	OK(gpgpuKmtClose(f->context));
}

/*
 * fixture_kernel() - 上传测试 Kernel 及描述对象
 *
 * 在 Host 暂存区组装描述符和代码，经同步 DMA 写入 VRAM。
 */
static void fixture_kernel(QueueFixture *f, const void *code, size_t size,
			   unsigned int argument_size)
{
	GpgpuKernelDescriptor descriptor = {
		.code_entry = htole32(f->vram.vram_offset + 0x100),
		.code_size = htole32(size),
		.kernarg_size = htole32(argument_size),
	};

	CHECK(size < 0xf00);
	memcpy(f->staging.cpu, &descriptor, sizeof(descriptor));
	memcpy((char *)f->staging.cpu + 0x100, code, size);
	queueHostFence();
	OK(gpgpuKmtCopyMemory(&f->staging, 0, &f->vram, 0, 0x1000, GPGPU_COPY_H2D));
}

/*
 * fixture_store_kernel() - 装载最小写值 Kernel
 *
 * 复用上传入口，代码从参数读取目的偏移和值并写回 VRAM。
 */
static void fixture_store_kernel(QueueFixture *f)
{
	static const uint32_t code[] = {
		0x00052283,
		0x00452303,
		0x0062a023,
		0x00100073,
	};

	fixture_kernel(f, code, sizeof(code), 8);
}

/*
 * task_init() - 准备一次测试任务的参数、Signal 和事件
 *
 * 为源队列创建独占事件，将小端参数及完成地址写入待发布 AQL 副本。
 */
static void task_init(QueueFixture *f, unsigned int index, QueueTask *t, const uint32_t *args,
		      unsigned int size)
{
	memset(t, 0, sizeof(*t));
	t->queue = &f->queue[index];
	OK(gpgpuKmtAllocMemory(f->context, GPGPU_MEMORY_HOST, 128, 8, &t->memory));
	OK(gpgpuKmtCreateEvent(t->queue, &t->event));
	t->signal = t->memory.cpu;
	*t->signal = (GpgpuSignal) {
		.value = htole64(1),
		.event_id = htole32(t->event.id),
		.event_mailbox_ptr = htole64(t->event.mailbox_address),
	};
	for (unsigned int i = 0; i < size / 4; i++)
		((uint32_t *)((char *)t->memory.cpu + 64))[i] = htole32(args[i]);
	t->packet = (GpgpuAqlPacket) {
		.full_header = htole32((1 << 16) | GPGPU_AQL_HEADER),
		.workgroup_size_x = htole16(1),
		.workgroup_size_y = htole16(1),
		.workgroup_size_z = htole16(1),
		.grid_size_x = htole32(1),
		.grid_size_y = htole32(1),
		.grid_size_z = htole32(1),
		.kernel_object = htole64(f->vram.vram_offset),
		.kernarg_address = htole64(t->memory.host_address + 64),
		.completion_signal = htole64(t->memory.host_address),
	};
}

/*
 * task_submit() - 提交一项已准备的测试任务
 *
 * 先预留编号，再按 Header 最后发布的协议敲 Doorbell。
 */
static void task_submit(QueueTask *t)
{
	OK(queueReserve(t->queue, &t->number));
	queuePublish(t->queue, t->number, &t->packet);
}

/*
 * task_wait() - 等待测试任务及通知处理全部结束
 *
 * 先检查 Signal，再等待事件与 Ring 排空，避免过早复用通知资源。
 */
static void task_wait(QueueTask *t)
{
	OK(queueWaitSignal(t->signal, &t->event, 10000));
	/* Signal 可早于 IH：复用事件前再确认通知已处理、Ring 槽已归还。 */
	OK(gpgpuKmtWaitEvent(&t->event, 10000));
	OK(queueDrain(t->queue, 10000));
}

/*
 * task_destroy() - 释放测试任务的事件与 Host 内存
 *
 * 调用方先确认任务和旧通知处理结束。
 */
static void task_destroy(QueueTask *t)
{
	OK(gpgpuKmtDestroyEvent(&t->event));
	OK(gpgpuKmtFreeMemory(&t->memory));
}

/*
 * group_normal() - 验证队列模式的正常计算与连续提交
 *
 * 轮流使用两条队列，修改输入并核对向量加、矩阵乘和 ReLU 输出。
 */
static void group_normal(void)
{
	QueueFixture f;
	QueueTask t;
	uint32_t args[6];
	float *x, *y, *out;

	fixture_init(&f, 2, 256);
	x = (void *)((char *)f.staging.cpu + 0x1000);
	y = (void *)((char *)f.staging.cpu + 0x2000);
	out = (void *)((char *)f.staging.cpu + 0x3000);
	args[0] = f.vram.vram_offset + 0x1000;
	args[1] = f.vram.vram_offset + 0x2000;
	args[2] = f.vram.vram_offset + 0x3000;
	args[3] = 17;
	fixture_kernel(&f, vector_add_code, sizeof(vector_add_code), 16);
	for (unsigned int round = 0; round < 12; round++) {
		for (unsigned int i = 0; i < 17; i++) {
			x[i] = round * 10 + i;
			y[i] = (float)i - (float)round;
		}
		OK(gpgpuKmtCopyMemory(&f.staging, 0x1000, &f.vram, 0x1000, 0x2000, GPGPU_COPY_H2D));
		task_init(&f, round % 2, &t, args, 16);
		t.packet.workgroup_size_x = htole16(32);
		t.packet.grid_size_x = htole32(32);
		task_submit(&t);
		task_wait(&t);
		OK(gpgpuKmtCopyMemory(&f.staging, 0x3000, &f.vram, 0x3000, 17 * 4, GPGPU_COPY_D2H));
		for (unsigned int i = 0; i < 17; i++)
			CHECK(out[i] == x[i] + y[i]);
		task_destroy(&t);
	}
	fixture_kernel(&f, matmul_code, sizeof(matmul_code), 24);
	for (unsigned int i = 0; i < 6; i++) {
		x[i] = i + 1;
		y[i] = (float)i - 2;
	}
	args[3] = 2;
	args[4] = 2;
	args[5] = 3;
	OK(gpgpuKmtCopyMemory(&f.staging, 0x1000, &f.vram, 0x1000, 0x2000, GPGPU_COPY_H2D));
	task_init(&f, 0, &t, args, 24);
	t.packet.full_header = htole32((2 << 16) | GPGPU_AQL_HEADER);
	t.packet.grid_size_x = htole32(2);
	t.packet.grid_size_y = htole32(2);
	task_submit(&t);
	task_wait(&t);
	OK(gpgpuKmtCopyMemory(&f.staging, 0x3000, &f.vram, 0x3000, 16, GPGPU_COPY_D2H));
	for (unsigned int row = 0; row < 2; row++) {
		for (unsigned int col = 0; col < 2; col++) {
			float expected = 0;

			for (unsigned int k = 0; k < 3; k++)
				expected += x[row * 3 + k] * y[k * 2 + col];
			CHECK(out[row * 2 + col] == expected);
		}
	}
	task_destroy(&t);
	fixture_kernel(&f, relu_code, sizeof(relu_code), 12);
	for (unsigned int i = 0; i < 17; i++)
		x[i] = (float)i - 8;
	args[1] = args[2];
	args[2] = 17;
	OK(gpgpuKmtCopyMemory(&f.staging, 0x1000, &f.vram, 0x1000, 17 * 4, GPGPU_COPY_H2D));
	task_init(&f, 1, &t, args, 12);
	t.packet.grid_size_x = htole32(32);
	t.packet.workgroup_size_x = htole16(32);
	task_submit(&t);
	task_wait(&t);
	OK(gpgpuKmtCopyMemory(&f.staging, 0x3000, &f.vram, 0x3000, 17 * 4, GPGPU_COPY_D2H));
	for (unsigned int i = 0; i < 17; i++)
		CHECK(out[i] == (x[i] < 0 ? 0 : x[i]));
	task_destroy(&t);
	fixture_destroy(&f);
	puts("PASS queue-group-1");
}

/*
 * submit_thread() - 在线程中提交一个测试任务
 *
 * 复用预留与发布路径，供并发生产者验收调用。
 */
static void *submit_thread(void *opaque)
{
	task_submit(opaque);
	return NULL;
}

/*
 * group_concurrent() - 验证预留顺序、发布空洞和 Ring 回绕
 *
 * 暂缓首包发布，让预留跨度超过容量，再核对全部输出与 INVALID 回收。
 */
static void group_concurrent(void)
{
	QueueFixture f;
	QueueTask task[GPGPU_TEST_SUBMIT_THREADS];
	pthread_t worker[GPGPU_TEST_SUBMIT_THREADS - 1];
	uint64_t deadline;

	fixture_init(&f, 1, 128);
	fixture_store_kernel(&f);
	for (unsigned int i = 0; i < GPGPU_TEST_SUBMIT_THREADS; i++) {
		uint32_t args[] = { f.vram.vram_offset + 0x3000 + i * 4, 0xf0010000 + i };

		task_init(&f, 0, &task[i], args, sizeof(args));
	}
	OK(queueReserve(&f.queue[0], &task[0].number));
	for (unsigned int i = 1; i < GPGPU_TEST_SUBMIT_THREADS; i++)
		CHECK(!pthread_create(&worker[i - 1], NULL, submit_thread, &task[i]));
	deadline = queueNowNs() + 10000000000ULL;
	while (__atomic_load_n(f.queue[0].wptr, __ATOMIC_ACQUIRE) < GPGPU_TEST_SUBMIT_THREADS &&
	       queueNowNs() < deadline) {
		usleep(1000);
	}
	CHECK(__atomic_load_n(f.queue[0].wptr, __ATOMIC_ACQUIRE) == 4);
	CHECK(__atomic_load_n(f.queue[0].rptr, __ATOMIC_ACQUIRE) == 0);
	CHECK(gpgpuKmtDestroyQueue(&f.queue[0]) == GPGPU_KMT_UNAVAILABLE);
	queuePublish(&f.queue[0], task[0].number, &task[0].packet);
	for (unsigned int i = 1; i < GPGPU_TEST_SUBMIT_THREADS; i++)
		CHECK(!pthread_join(worker[i - 1], NULL));
	for (unsigned int i = 0; i < GPGPU_TEST_SUBMIT_THREADS; i++)
		task_wait(&task[i]);
	OK(gpgpuKmtCopyMemory(&f.staging, 0x3000, &f.vram, 0x3000, 16, GPGPU_COPY_D2H));
	for (unsigned int i = 0; i < GPGPU_TEST_SUBMIT_THREADS; i++) {
		CHECK(((uint32_t *)((char *)f.staging.cpu + 0x3000))[i] == 0xf0010000 + i);
		task_destroy(&task[i]);
	}
	CHECK(f.queue[0].ring[0].full_header == GPGPU_AQL_INVALID &&
	      f.queue[0].ring[1].full_header == GPGPU_AQL_INVALID);
	fixture_destroy(&f);
	puts("PASS queue-group-2");
}

/**
 * struct WaitThread - 验证多等待者同时被事件唤醒的线程参数
 * @task: 所有等待者共用的测试任务。
 * @status: 线程保存的等待返回状态。
 * @tid: 等待线程的 Linux TID，用于检查是否进入阻塞。
 */
typedef struct WaitThread {
	QueueTask *task;
	GpgpuKmtStatus status;
	pid_t tid;
} WaitThread;

/**
 * struct WaitSubmit - 在等待者进入阻塞后发布任务的协调参数
 * @task: 等待提交的任务。
 * @main_tid: 主线程的 TID。
 * @waiters: 参与事件等待的线程参数数组。
 */
typedef struct WaitSubmit {
	QueueTask *task;
	pid_t main_tid;
	WaitThread *waiters;
} WaitSubmit;

/*
 * wait_thread() - 登记线程身份并阻塞等待事件
 *
 * 保存等待返回值，供主线程验证多个等待者收到同一次通知。
 */
static void *wait_thread(void *opaque)
{
	WaitThread *waiter = opaque;

	__atomic_store_n(&waiter->tid, (pid_t)syscall(SYS_gettid), __ATOMIC_RELEASE);
	waiter->status = gpgpuKmtWaitEvent(&waiter->task->event, 10000);
	return NULL;
}

/*
 * thread_wait_ioctl() - 检查目标线程是否正在事件等待 ioctl 中
 *
 * 读取 proc syscall 参数，并按 32 位命令号比较以兼容符号扩展。
 */
static int thread_wait_ioctl(pid_t tid)
{
	char path[80];
	long number;
	unsigned long fd, command;
	FILE *file;
	int fields;

	snprintf(path, sizeof(path), "/proc/self/task/%ld/syscall", (long)tid);
	file = fopen(path, "r");
	if (!file)
		return 0;
	fields = fscanf(file, "%ld %lx %lx", &number, &fd, &command);
	fclose(file);
	/* ioctl 的命令参数为 32 位；libc 传参可能将其符号扩展到 64 位。 */
	return fields == 3 && number == SYS_ioctl &&
		(uint32_t)command == (uint32_t)GPGPU_IOC_EVENT_WAIT;
}

/*
 * thread_blocked_in_event_wait() - 确认线程已进入事件阻塞等待
 *
 * 同时检查 proc 睡眠状态和前后两次 ioctl，排除用户态轮询或其他睡眠。
 */
static int thread_blocked_in_event_wait(pid_t tid)
{
	char path[80], state[512], *comm_end;
	FILE *file;
	int sleeping;

	if (!tid || !thread_wait_ioctl(tid))
		return 0;
	snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", (long)tid);
	file = fopen(path, "r");
	if (!file)
		return 0;
	sleeping = fgets(state, sizeof(state), file) != NULL;
	fclose(file);
	comm_end = sleeping ? strrchr(state, ')') : NULL;
	sleeping = comm_end && comm_end[1] == ' ' && comm_end[2] == 'S';
	/*
	 * 两次确认同一 ioctl，
	 * 避免把用户态轮询、其他睡眠或退出误作等待。
	 */
	return sleeping && thread_wait_ioctl(tid);
}

/*
 * submit_after_waiters() - 等待所有参与线程阻塞后才发布任务
 *
 * 在期限内反复核对线程状态，保证测试覆盖登记后唤醒的交错。
 */
static void *submit_after_waiters(void *opaque)
{
	WaitSubmit *submitter = opaque;
	uint64_t deadline = queueNowNs() + 10000000000ULL;

	for (;;) {
		int blocked = thread_blocked_in_event_wait(submitter->main_tid);

		for (unsigned int i = 0; i < 2; i++) {
			pid_t tid = __atomic_load_n(&submitter->waiters[i].tid, __ATOMIC_ACQUIRE);

			blocked &= thread_blocked_in_event_wait(tid);
		}
		if (blocked) {
			task_submit(submitter->task);
			return NULL;
		}
		CHECK(queueNowNs() < deadline);
		usleep(1000);
	}
}

/*
 * group_wait() - 验证超时、通知保留与多等待者唤醒
 *
 * 覆盖未完成检查、主动轮询转阻塞，
 * 以及完成通知提前到达后的重复等待。
 */
static void group_wait(void)
{
	QueueFixture f;
	QueueTask task;
	pthread_t threads[3];
	WaitThread waiters[2] = { 0 };
	WaitSubmit submitter = {
		.task = &task,
		.main_tid = (pid_t)syscall(SYS_gettid),
		.waiters = waiters,
	};
	struct gpgpu_event_request state;
	uint32_t args[2];

	fixture_init(&f, 1, 256);
	fixture_store_kernel(&f);
	args[0] = f.vram.vram_offset + 0x3000;
	args[1] = 77;
	task_init(&f, 0, &task, args, sizeof(args));
	OK(gpgpuKmtQueryEvent(&task.event, &state));
	CHECK(state.state == GPGPU_EVENT_PENDING);
	CHECK(gpgpuKmtWaitEvent(&task.event, 0) == GPGPU_KMT_WAIT_TIMEOUT);
	CHECK(gpgpuKmtWaitEvent(&task.event, 1) == GPGPU_KMT_WAIT_TIMEOUT);
	CHECK(gpgpuKmtDestroyEvent(&task.event) == GPGPU_KMT_UNAVAILABLE);
	for (unsigned int i = 0; i < 2; i++) {
		waiters[i].task = &task;
		CHECK(!pthread_create(&threads[i], NULL, wait_thread, &waiters[i]));
	}
	CHECK(!pthread_create(&threads[2], NULL, submit_after_waiters, &submitter));
	/*
	 * 提交前确认主线程也已越过约 200 us 主动检查，阻塞在事件 ioctl。
	 */
	OK(queueWaitSignal(task.signal, &task.event, 10000));
	for (unsigned int i = 0; i < 3; i++)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(waiters[0].status == GPGPU_KMT_SUCCESS && waiters[1].status == GPGPU_KMT_SUCCESS);
	OK(gpgpuKmtWaitEvent(&task.event, 0));
	OK(gpgpuKmtWaitEvent(&task.event, 0));
	OK(queueWaitSignal(task.signal, &task.event, 0));
	OK(queueDrain(&f.queue[0], 10000));
	task_destroy(&task);
	fixture_destroy(&f);
	puts("PASS queue-group-3");
}

/**
 * struct ReserveThread - 验证队列预留阻塞与故障退出的线程参数
 * @queue: 进行原子取号的队列。
 * @number: 预留得到的包编号。
 * @status: 预留接口的返回状态。
 */
typedef struct ReserveThread {
	GpgpuKmtQueue *queue;
	uint32_t number;
	GpgpuKmtStatus status;
} ReserveThread;

/*
 * reserve_thread() - 在独立线程中执行阻塞预留
 *
 * 记录包编号与返回状态，用于检查故障时生产者能及时退出。
 */
static void *reserve_thread(void *opaque)
{
	ReserveThread *submitter = opaque;

	submitter->status = queueReserve(submitter->queue, &submitter->number);
	return NULL;
}

/*
 * group_errors() - 验证队列故障隔离与失败资源保护
 *
 * 注入非法包和参数访存故障，确认 Signal 未伪造成功、R 未越过失败包。
 */
static void group_errors(void)
{
	QueueFixture f;
	QueueTask bad, good, args_fault;
	struct gpgpu_queue_state state;
	ReserveThread submitter;
	pthread_t thread;
	struct timespec join_deadline;
	uint64_t deadline;
	uint32_t args[2];

	fixture_init(&f, 2, 64);
	fixture_store_kernel(&f);
	args[0] = f.vram.vram_offset + 0x3000;
	args[1] = 88;
	task_init(&f, 0, &bad, args, sizeof(args));
	bad.packet.full_header = htole32((1 << 16) | 7);
	OK(queueReserve(bad.queue, &bad.number));
	submitter = (ReserveThread) { .queue = bad.queue };
	CHECK(!pthread_create(&thread, NULL, reserve_thread, &submitter));
	deadline = queueNowNs() + 10000000000ULL;
	while (__atomic_load_n(bad.queue->wptr, __ATOMIC_ACQUIRE) != 2 && queueNowNs() < deadline)
		usleep(1000);
	CHECK(__atomic_load_n(bad.queue->wptr, __ATOMIC_ACQUIRE) == 2);
	queuePublish(bad.queue, bad.number, &bad.packet);
	CHECK(gpgpuKmtWaitEvent(&bad.event, 10000) == GPGPU_KMT_WAIT_FAILURE);
	clock_gettime(CLOCK_REALTIME, &join_deadline);
	join_deadline.tv_sec += 10;
	CHECK(!pthread_timedjoin_np(thread, NULL, &join_deadline));
	CHECK(submitter.status == GPGPU_KMT_WAIT_FAILURE && submitter.number == 1);
	CHECK(bad.signal->value == 1);
	OK(gpgpuKmtQueryQueue(&f.queue[0], &state));
	CHECK(state.state == GPGPU_QUEUE_FAULTED && state.error == GPGPU_QUEUE_ERROR_PACKET);
	CHECK(*f.queue[0].rptr == 0 && *f.queue[0].wptr == 2);
	task_init(&f, 1, &good, args, sizeof(args));
	task_submit(&good);
	task_wait(&good);
	task_init(&f, 1, &args_fault, args, sizeof(args));
	/* 选用开发客体 RAM 和 PCI 窗口之外的物理地址。 */
	args_fault.packet.kernarg_address = htole64(0x10000000000ULL);
	task_submit(&args_fault);
	CHECK(gpgpuKmtWaitEvent(&args_fault.event, 10000) == GPGPU_KMT_WAIT_FAILURE);
	CHECK(args_fault.signal->value == 1);
	OK(gpgpuKmtQueryQueue(&f.queue[1], &state));
	CHECK(state.error == GPGPU_QUEUE_ERROR_EXECUTOR);
	CHECK(gpgpuKmtFreeMemory(&f.vram) == GPGPU_KMT_UNAVAILABLE);
	OK(gpgpuKmtDestroyQueue(&f.queue[0]));
	OK(gpgpuKmtDestroyQueue(&f.queue[1]));
	task_destroy(&bad);
	task_destroy(&good);
	task_destroy(&args_fault);
	fixture_destroy(&f);
	puts("PASS queue-group-4");
}

/*
 * child_management() - 在子进程中执行独立队列生命周期
 *
 * 通过管道与父进程协调启动，验证多个会话可并发发布管理请求。
 */
static int child_management(int ready_fd, int go_fd)
{
	QueueFixture f;
	QueueTask task;
	char byte = 1;
	uint32_t args[2];

	fixture_init(&f, 1, 256);
	fixture_store_kernel(&f);
	CHECK(write(ready_fd, &byte, 1) == 1);
	CHECK(read(go_fd, &byte, 1) == 1);
	args[0] = f.vram.vram_offset + 0x3000;
	args[1] = getpid();
	task_init(&f, 0, &task, args, sizeof(args));
	task_submit(&task);
	task_wait(&task);
	task_destroy(&task);
	fixture_destroy(&f);
	return 0;
}

/*
 * check_external_mapping() - 验证外部 VMA 引用阻止内存释放
 *
 * 建立同一对象的两次映射，解除其中一次后仍须拒绝释放。
 */
static void check_external_mapping(void)
{
	struct gpgpu_queue_info version = { .version = GPGPU_QABI_VERSION + 1 };
	struct gpgpu_memory_request memory = {
		.version = GPGPU_QABI_VERSION,
		.kind = GPGPU_MEMORY_HOST,
		.size = 4096,
		.alignment = 8,
	};
	struct gpgpu_object_request release;
	void *first, *second;
	int fd = open("/dev/gpgpu0", O_RDWR | O_CLOEXEC);

	CHECK(fd >= 0);
	CHECK(ioctl(fd, GPGPU_IOC_QINFO, &version) == -1 && errno == EPROTONOSUPPORT);
	CHECK(!ioctl(fd, GPGPU_IOC_MEMORY_ALLOC, &memory));
	first = mmap(NULL, memory.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, memory.mmap_offset);
	second =
		mmap(NULL, memory.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, memory.mmap_offset);
	CHECK(first != MAP_FAILED && second != MAP_FAILED);
	__atomic_store_n((uint32_t *)first, 0xaabbccdd, __ATOMIC_RELEASE);
	CHECK(__atomic_load_n((uint32_t *)second, __ATOMIC_ACQUIRE) == 0xaabbccdd);
	release = (struct gpgpu_object_request) {
		.version = GPGPU_QABI_VERSION,
		.handle = memory.handle,
	};
	CHECK(!munmap(first, memory.size));
	CHECK(ioctl(fd, GPGPU_IOC_MEMORY_FREE, &release) == -1 && errno == EBUSY);
	CHECK(!munmap(second, memory.size));
	CHECK(!ioctl(fd, GPGPU_IOC_MEMORY_FREE, &release));
	CHECK(!close(fd));
}

/*
 * check_queue_storage_alias() - 验证队列存储对象不能交叉复用
 *
 * 尝试让新 Ring 覆盖旧进度或反向借用，要求驱动拒绝别名。
 */
static void check_queue_storage_alias(void)
{
	struct gpgpu_memory_request memory[3];
	struct gpgpu_queue_create create = {
		.version = GPGPU_QABI_VERSION,
		.ring_size = 256,
		.rptr_offset = 0,
		.wptr_offset = 8,
	};
	struct gpgpu_queue_state destroy = { .version = GPGPU_QABI_VERSION };
	int fd = open("/dev/gpgpu0", O_RDWR | O_CLOEXEC);

	CHECK(fd >= 0);
	for (unsigned int i = 0; i < 3; i++) {
		memory[i] = (struct gpgpu_memory_request) {
			.version = GPGPU_QABI_VERSION,
			.kind = GPGPU_MEMORY_HOST,
			.size = 4096,
			.alignment = 64,
		};
		CHECK(!ioctl(fd, GPGPU_IOC_MEMORY_ALLOC, &memory[i]));
	}
	create.ring_handle = memory[0].handle;
	create.progress_handle = memory[1].handle;
	CHECK(!ioctl(fd, GPGPU_IOC_QUEUE_CREATE, &create));
	destroy.queue_id = create.queue_id;
	/* 新 Ring 不能覆盖旧进度页；新进度也不能借用旧 Ring 的空字段。 */
	create.ring_handle = memory[1].handle;
	create.progress_handle = memory[2].handle;
	CHECK(ioctl(fd, GPGPU_IOC_QUEUE_CREATE, &create) == -1 && errno == EBUSY);
	create.ring_handle = memory[2].handle;
	create.progress_handle = memory[0].handle;
	create.rptr_offset = 8;
	create.wptr_offset = 16;
	CHECK(ioctl(fd, GPGPU_IOC_QUEUE_CREATE, &create) == -1 && errno == EBUSY);
	CHECK(!ioctl(fd, GPGPU_IOC_QUEUE_DESTROY, &destroy));
	for (unsigned int i = 0; i < 3; i++) {
		struct gpgpu_object_request release = {
			.version = GPGPU_QABI_VERSION,
			.handle = memory[i].handle,
		};

		CHECK(!ioctl(fd, GPGPU_IOC_MEMORY_FREE, &release));
	}
	CHECK(!close(fd));
}

/*
 * child_orphan() - 制造带未发布预留的异常退出会话
 *
 * 保留真实编号空洞后直接退出，用于验证设备仍可能访问时的资源保留。
 */
static int child_orphan(int ready_fd)
{
	QueueFixture f;

	fixture_init(&f, 1, 256);
	/*
	 * 预留之后异常退出：不取消编号、不伪造 Header，
	 * 保留一个真实空洞。
	 */
	uint32_t number;

	OK(queueReserve(&f.queue[0], &number));
	CHECK(number == 0);
	CHECK(write(ready_fd, &f.vram.vram_offset, sizeof(f.vram.vram_offset)) ==
	      sizeof(f.vram.vram_offset));
	_exit(0);
}

/*
 * check_orphan_retention() - 验证进程退出后保留未结束的队列资源
 *
 * 创建新会话核对旧 VRAM 未复用，且旧队列仍占用全设备名额。
 */
static void check_orphan_retention(void)
{
	int ready[2], status;
	pid_t child;
	uint32_t old_offset;
	QueueFixture f;
	GpgpuKmtMemory ring, progress;
	GpgpuKmtQueue queue;

	CHECK(!pipe(ready));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		char argument[24];

		close(ready[0]);
		snprintf(argument, sizeof(argument), "%d", ready[1]);
		execl("/proc/self/exe", "test-queue", "--orphan-child", argument, NULL);
		_exit(127);
	}
	close(ready[1]);
	CHECK(read(ready[0], &old_offset, sizeof(old_offset)) == sizeof(old_offset));
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(ready[0]);
	fixture_init(&f, 1, 256);
	CHECK((uint64_t)f.vram.vram_offset + f.vram.size <= old_offset ||
	      (uint64_t)old_offset + 65536 <= f.vram.vram_offset);
	OK(gpgpuKmtAllocMemory(f.context, GPGPU_MEMORY_HOST, 256, 64, &ring));
	OK(gpgpuKmtAllocMemory(f.context, GPGPU_MEMORY_HOST, 16, 8, &progress));
	CHECK(gpgpuKmtCreateQueue(f.context, &ring, 0, 256, &progress, 0, 8, &queue) ==
	      GPGPU_KMT_OUT_OF_RESOURCES);
	OK(gpgpuKmtFreeMemory(&ring));
	OK(gpgpuKmtFreeMemory(&progress));
	fixture_destroy(&f);
	/*
	 * 子进程资源留到客体测试脚本移除设备；
	 * 仍可能访问的内存不提前复用。
	 */
}

/*
 * group_lifetime() - 验证对象引用、旧通知及跨进程生命周期
 *
 * 依次检查销毁限制、入口复用、并发管理、外部映射与 orphan 保留。
 */
static void group_lifetime(void)
{
	QueueFixture f;
	QueueTask task;
	GpgpuKmtMemory extra_ring, extra_progress;
	GpgpuKmtQueue extra;
	uint32_t args[2];
	int ready[2], go[2], status;
	pid_t child[2];
	char byte[2];

	fixture_init(&f, 2, 256);
	CHECK(gpgpuKmtClose(f.context) == GPGPU_KMT_UNAVAILABLE);
	CHECK(gpgpuKmtFreeMemory(&f.ring[0]) == GPGPU_KMT_UNAVAILABLE);
	OK(gpgpuKmtAllocMemory(f.context, GPGPU_MEMORY_HOST, 256, 64, &extra_ring));
	OK(gpgpuKmtAllocMemory(f.context, GPGPU_MEMORY_HOST, 16, 8, &extra_progress));
	CHECK(gpgpuKmtCreateQueue(f.context, &extra_ring, 0, 256, &extra_progress, 0, 8, &extra) ==
	      GPGPU_KMT_OUT_OF_RESOURCES);
	OK(gpgpuKmtFreeMemory(&extra_ring));
	OK(gpgpuKmtFreeMemory(&extra_progress));
	fixture_store_kernel(&f);
	args[0] = f.vram.vram_offset + 0x3000;
	args[1] = 99;
	task_init(&f, 0, &task, args, sizeof(args));
	task_submit(&task);
	task_wait(&task);
	OK(gpgpuKmtDestroyQueue(&f.queue[0]));
	/* 旧事件仍可查询；新队列不得收到旧任务通知。 */
	OK(gpgpuKmtWaitEvent(&task.event, 0));
	CHECK(gpgpuKmtCreateQueue(f.context, &f.ring[0], 0, 256, &f.progress[0], 0, 8,
				  &f.queue[0]) == GPGPU_KMT_OUT_OF_RESOURCES);
	/*
	 * 同进程门铃共享独占页面；
	 * 最后一个旧映射结束后才能复用页面入口。
	 */
	OK(gpgpuKmtDestroyQueue(&f.queue[1]));
	/*
	 * 映射已全部解除，只保留旧事件：
	 * 回收仍须保留其队列和门铃入口。
	 */
	OK(gpgpuKmtCreateQueue(f.context, &f.ring[1], 0, 256, &f.progress[1], 0, 8, &f.queue[1]));
	CHECK(gpgpuKmtCreateQueue(f.context, &f.ring[0], 0, 256, &f.progress[0], 0, 8,
				  &f.queue[0]) == GPGPU_KMT_OUT_OF_RESOURCES);
	OK(gpgpuKmtDestroyQueue(&f.queue[1]));
	task_destroy(&task);
	OK(gpgpuKmtCreateQueue(f.context, &f.ring[0], 0, 256, &f.progress[0], 0, 8, &f.queue[0]));
	fixture_destroy(&f);
	CHECK(!pipe(ready) && !pipe(go));
	for (unsigned int i = 0; i < 2; i++) {
		child[i] = fork();
		CHECK(child[i] >= 0);
		if (!child[i]) {
			char a[24], b[24];

			snprintf(a, sizeof(a), "%d", ready[1]);
			snprintf(b, sizeof(b), "%d", go[0]);
			close(ready[0]);
			close(go[1]);
			execl("/proc/self/exe", "test-queue", "--management-child", a, b, NULL);
			_exit(127);
		}
	}
	close(ready[1]);
	close(go[0]);
	CHECK(read(ready[0], byte, 1) == 1);
	CHECK(read(ready[0], byte + 1, 1) == 1);
	CHECK(write(go[1], "xx", 2) == 2);
	for (unsigned int i = 0; i < 2; i++) {
		CHECK(waitpid(child[i], &status, 0) == child[i]);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	close(ready[0]);
	close(go[1]);
	check_external_mapping();
	check_queue_storage_alias();
	check_orphan_retention();
	puts("PASS queue-group-5");
}

/*
 * main() - 运行五组队列验收或子进程辅助流程
 *
 * 按参数选择管理/orphan 子进程入口，正常入口逐组核对并输出通过标记。
 */
int main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	if (argc == 4 && !strcmp(argv[1], "--management-child"))
		return child_management(atoi(argv[2]), atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "--orphan-child"))
		return child_orphan(atoi(argv[2]));
	CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "/dev/gpgpu0")));
	group_normal();
	group_concurrent();
	group_wait();
	group_errors();
	group_lifetime();
	puts("GPGPU queue tests: 5/5 PASS");
	return 0;
}
