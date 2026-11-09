// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 模型算子回放：流式装载真实数组、分块派发与完整结果比较
 *
 * job 文本与小端命令模板 -> 固定小暂存区 -> VRAM 数组
 *   -> 两槽 RT 参数（命令地址、全局 tile 起点）
 *   -> 手写 model_eval Kernel -> 分块回读 -> 原始 reference
 *
 * 本文件不计算 CPU 算子结果。复合算子的中间数组也由 GPU 产生。
 * --wrap=ioctl 只观察本程序的 BO_CREATE，记录 RT 内部对象和通知页，
 * 不改变驱动返回值、错误或资源寿命；其他程序不使用该链接包装。
 */
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "gpgpu/gpgpu_eval_abi.h"
#include "gpgpu_rt.h"
#include "model_eval.h"

/* 文件读缓冲与 RT 暂存区的字节数，不随张量大小增长。 */
#define EVAL_STAGING_BYTES (64u * 1024u)
/* 每个 X 方向工作组的线程数，可调整但仍受共享执行上限约束。 */
#define EVAL_BLOCK_THREADS 64u
/* 未指定时的目标 tile 工作项数，实际值按工作组取整并限制上界。 */
#define EVAL_DEFAULT_TILE 1024u
/* 单次同步派发的默认等待上限，单位毫秒。 */
#define EVAL_DEFAULT_TIMEOUT_MS 60000u
/* 本轮预定的绝对与相对容差；CLI 只允许收紧。 */
#define EVAL_DEFAULT_TOLERANCE 0.0002
/* 单调时钟每秒的纳秒数。 */
#define EVAL_NS_PER_SECOND UINT64_C(1000000000)
/* 文件 barrier 的轮询间隔，单位纳秒。 */
#define EVAL_BARRIER_POLL_NS 10000000L
/* BO 记录数组首次分配的记录数，后续按需扩容。 */
#define EVAL_BO_INITIAL_RECORDS 8u
/* 命令前缀中的输入地址槽数，从共享偏移推导。 */
#define EVAL_INPUTS ((GPGPU_EVAL_IN5 - GPGPU_EVAL_IN0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* 命令前缀中可保存 scratch 地址的槽数。 */
#define EVAL_SCRATCHES ((GPGPU_EVAL_AUX2 - GPGPU_EVAL_AUX0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* 命令前缀的小端 64 位槽数。 */
#define EVAL_WORDS (GPGPU_EVAL_PARAM_BYTES / GPGPU_EVAL_SLOT_BYTES)
/* 命令前缀中的维度槽数。 */
#define EVAL_DIMS ((GPGPU_EVAL_D15 - GPGPU_EVAL_D0) / GPGPU_EVAL_SLOT_BYTES + 1)
/* RT 接收的参数槽数：命令地址与 tile 起点。 */
#define EVAL_RT_ARGS ((GPGPU_EVAL_RT_BASE + GPGPU_EVAL_SLOT_BYTES) / GPGPU_EVAL_SLOT_BYTES)
/* Copy 形状与 stride 的最大秩，取自共享张量 ABI。 */
#define EVAL_COPY_RANK_MAX GPGPU_TENSOR_MAX_RANK

/* 输出的两种存储类型。 */
enum EvalType {
	EVAL_F32, /* 小端 IEEE FP32，每项 4 字节。 */
	EVAL_I64, /* 小端有符号 INT64，每项 8 字节。 */
};

/**
 * struct EvalOptions - 单次进程的命令行配置
 * @job_path: 冻结 job 文件路径。
 * @ready: 准备完成标记路径；未启用双进程屏障时为 NULL。
 * @start: 主机放行计算的标记路径。
 * @done: 计算与比较完成的标记路径。
 * @release: 主机放行资源回收的标记路径。
 * @timeout_ms: 单次同步派发等待上限，毫秒。
 * @repeats: 正式算子的重复次数。
 * @warmups: 不计入正式样本的预热次数。
 * @tile: 请求的每次派发工作项数，实际值经能力上限约束。
 * @backend: 请求的 GPGPU_EVAL_BACKEND_* 执行方式。
 * @atol: 有限算术值的绝对容差，只允许保持基准或收紧。
 * @rtol: 有限算术值的相对容差，只允许保持基准或收紧。
 * @uncached: 评估数据、命令和 scratch 是否申请不可缓存映射。
 */
typedef struct EvalOptions {
	const char *job_path;
	const char *ready;
	const char *start;
	const char *done;
	const char *release;
	uint32_t timeout_ms;
	size_t repeats;
	size_t warmups;
	uint64_t tile;
	uint64_t backend;
	double atol;
	double rtol;
	bool uncached;
} EvalOptions;

/**
 * struct EvalFile - 流式读取的原始数组文件
 * @path: 由本进程保存的文件路径，报告输出后释放。
 * @stream: 已打开的普通文件；未提供的可选输入为 NULL。
 * @bytes: 对同一文件描述符执行 fstat 得到的字节数。
 */
typedef struct EvalFile {
	char *path;
	FILE *stream;
	uint64_t bytes;
} EvalFile;

/**
 * struct EvalJob - 冻结输入、输出约定和已核对的执行规模
 * @case_id: 冻结用例编号，报告输出后释放。
 * @type: 输出的 FP32 或 INT64 存储类型。
 * @inputs: 各输入文件，与共享命令前缀的输入槽一一对应。
 * @reference: 仅供回读比较的 CPU 参考文件，不上传到设备输出区。
 * @words: CPU 本机字节序命令模板，上传时统一编码为小端。
 * @op: GPGPU_EVAL_* 软件算子编号。
 * @count: 完整输出的逻辑元素数，不含派发补齐线程。
 * @output_bytes: 完整输出占用的字节数。
 * @rows: 由维度推导的归约行数。
 * @work_count: 复合步骤需要覆盖的工作项数。
 * @vocabulary: embedding 权重中的词表项数。
 * @launch_count: 基本算子的工作项数，不覆盖输出和 scratch 长度。
 * @backend: 实际采用的后端；不支持块执行的算子保持 SCALAR。
 */
typedef struct EvalJob {
	char *case_id;
	enum EvalType type;
	EvalFile inputs[EVAL_INPUTS];
	EvalFile reference;
	uint64_t words[EVAL_WORDS];
	uint64_t op;
	uint64_t count;
	uint64_t output_bytes;
	uint64_t rows;
	uint64_t work_count;
	uint64_t vocabulary;
	/* 基本算子派发量，不覆盖 count/output_bytes 或 scratch 长度。 */
	uint64_t launch_count;
	/* 只有支持张量块的基本算子可选 BLOCK，其余保持 SCALAR。 */
	uint64_t backend;
} EvalJob;

/**
 * struct EvalBoRecord - 成功 BO_CREATE 的一次观测
 * @role: 分配用途；引用的路径或固定字符串存活到报告输出。
 * @kind: 驱动返回的存储类型。
 * @requested: 原始请求字节数。
 * @mapped: 页取整的映射字节数，不代表 buddy 实际保留量。
 */
typedef struct EvalBoRecord {
	const char *role;
	uint32_t kind;
	uint64_t requested;
	uint64_t mapped;
} EvalBoRecord;

/**
 * struct EvalMetrics - 全部已比较元素的累计误差
 * @compared: 已比较的逻辑元素数。
 * @failed: 不满足既定比较策略的元素数。
 * @nonfinite_mismatches: NaN/Inf 种类或符号不匹配的元素数。
 * @maxabs: 最大逐值绝对误差。
 * @maxrel: 最大逐值相对误差。
 * @error_squared: 误差平方和，用 long double 累计。
 * @reference_squared: 参考值平方和，用于计算相对 L2。
 */
typedef struct EvalMetrics {
	uint64_t compared;
	uint64_t failed;
	uint64_t nonfinite_mismatches;

	long double maxabs;
	long double maxrel;
	long double error_squared;
	long double reference_squared;
} EvalMetrics;

/**
 * struct EvalSample - 一次完整算子及其回读比较
 * @warmup: 是否为不计入正式成绩的预热样本。
 * @complete: 全部计算步骤是否已完成。
 * @checked: 是否已比较完整输出。
 * @operator_ns: 全部步骤、参数上传和同步耗时，纳秒；不含验证。
 * @params_ns: 参数上传的累计耗时，纳秒。
 * @params_bytes: 参数上传的累计字节数。
 * @dispatch_ns: 同步派发的累计耗时，纳秒。
 * @tile_dispatches: 全部计算步骤的派发次数。
 * @work_items: 全部步骤的逻辑工作项数，不含尾部补齐线程。
 * @d2h_ns: 输出回读耗时，纳秒。
 * @d2h_bytes: 输出回读字节数。
 * @reference_read_ns: 参考文件读取耗时，纳秒。
 * @compare_ns: CPU 逐值比较耗时，纳秒。
 * @metrics: 本次样本的完整输出比较结果。
 */
typedef struct EvalSample {
	bool warmup;
	bool complete;
	bool checked;
	uint64_t operator_ns;
	uint64_t params_ns;
	uint64_t params_bytes;
	uint64_t dispatch_ns;
	uint64_t tile_dispatches;
	/* 所有 lowering 步骤的逻辑派发量，不含尾部补齐线程。 */
	uint64_t work_items;
	uint64_t d2h_ns;
	uint64_t d2h_bytes;
	uint64_t reference_read_ns;
	uint64_t compare_ns;
	EvalMetrics metrics;
} EvalSample;

/**
 * struct EvalRun - 一个独立客体进程持有的回放状态
 * @options: 已解析的命令行配置。
 * @job: 冻结文件和已核对的算子形状。
 * @context: 本进程打开的 KMT 上下文。
 * @rt: 串行使用的小运行时。
 * @kernel: 已装载的 model_eval Kernel。
 * @inputs: 各输入的设备对象。
 * @output: 完整输出的设备对象。
 * @command: 公共命令前缀的设备对象。
 * @scratch: 复合算子使用的设备临时对象。
 * @bos: ioctl 包装器观测到的 BO_CREATE 记录。
 * @bo_count: 已填入的 BO 记录数。
 * @bo_capacity: BO 记录数组容量，单位条。
 * @allocation_role: 当前分配的用途，由 ioctl 观测器写入记录。
 * @samples: 预热与正式样本数组。
 * @sample_count: 已开始的样本数。
 * @buffer: 固定大小的文件读取和输出回读缓冲。
 * @reference_buffer: 固定大小的参考文件读取缓冲。
 * @error: 首次错误说明；后续错误不覆盖。
 * @kmt_status: 最近一次记录的 KMT 返回状态。
 * @uncertain: 尚不能确认设备访问已停止，禁止正常释放在途对象。
 * @retained: 收尾因不确定状态或失败而保留了资源。
 * @ready: 是否已经完成准备并发布就绪标记。
 * @clock_error: 单调时钟读取失败，计时结果不可作为成功成绩。
 * @stats_error: 分配统计失败，不能输出不完整的成功统计。
 * @cleanup_ok: 是否完成正常设备资源回收。
 * @tile: 经能力与工作组对齐约束的实际派发工作项上限。
 * @requested_bytes: BO_CREATE 请求的累计字节数。
 * @mapped_bytes: 页取整映射的累计字节数，不代表物理峰值。
 * @alloc_clear_ns: 分配与清零累计耗时，纳秒。
 * @buffer_setup_ns: 其余缓冲准备累计耗时，纳秒。
 * @runtime_setup_ns: RT 创建耗时，纳秒。
 * @kernel_load_ns: Kernel 装载耗时，纳秒。
 * @input_read_ns: 输入文件读取累计耗时，纳秒。
 * @h2d_ns: 输入上传累计耗时，纳秒。
 * @h2d_bytes: 输入上传累计字节数。
 * @h2d_chunks: 输入上传块数。
 * @queue_prepare_ns: 预先建立空闲队列的耗时，纳秒。
 * @rss_max_kib: 本进程报告的最大 RSS，KiB。
 *
 * 所有对象由本进程持有到共同回收屏障放行；状态不确定时沿用 RT/KMT
 * 的资源保留规则，不以进程侧错误返回推定设备访问已经停止。
 */
typedef struct EvalRun {
	EvalOptions options;
	EvalJob job;
	GpgpuKmtContext *context;
	GpgpuRt *rt;
	GpgpuRtKernel kernel;
	GpgpuKmtMemory *inputs[EVAL_INPUTS];
	GpgpuKmtMemory *output;
	GpgpuKmtMemory *command;
	GpgpuKmtMemory *scratch[EVAL_SCRATCHES];
	EvalBoRecord *bos;
	size_t bo_count;
	size_t bo_capacity;
	const char *allocation_role;
	EvalSample *samples;
	size_t sample_count;
	uint8_t *buffer;
	uint8_t *reference_buffer;
	const char *error;
	GpgpuKmtStatus kmt_status;
	bool uncertain;
	bool retained;
	bool ready;
	bool clock_error;
	bool stats_error;
	bool cleanup_ok;
	uint64_t tile;
	uint64_t requested_bytes;
	uint64_t mapped_bytes;
	uint64_t alloc_clear_ns;
	uint64_t buffer_setup_ns;
	uint64_t runtime_setup_ns;
	uint64_t kernel_load_ns;
	uint64_t input_read_ns;
	uint64_t h2d_ns;
	uint64_t h2d_bytes;
	uint64_t h2d_chunks;
	uint64_t queue_prepare_ns;
	long rss_max_kib;
} EvalRun;

/* glibc 与 Alpine musl 的 ioctl 请求参数类型不同。 */
#ifdef __GLIBC__
typedef unsigned long EvalIoctlRequest;
#else
typedef int EvalIoctlRequest;
#endif

/* 只供本 binary 的链接包装使用，不改变 KMT/RT 的公开接口。 */
static EvalRun *eval_observer;
__typeof__(ioctl) __real_ioctl;

/* 无符号乘法可表示时写入结果。 */
static bool multiply_ok(uint64_t a, uint64_t b, uint64_t *result)
{
	if (a && b > UINT64_MAX / a)
		return false;
	*result = a * b;
	return true;
}

/* 无符号加法可表示时写入结果。 */
static bool add_ok(uint64_t a, uint64_t b, uint64_t *result)
{
	if (b > UINT64_MAX - a)
		return false;
	*result = a + b;
	return true;
}

/* 单调时钟失败会使整次结果失败，不能伪造有效耗时。 */
static uint64_t clock_ns(EvalRun *run)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value)) {
		run->clock_error = true;
		return 0;
	}
	return (uint64_t)value.tv_sec * EVAL_NS_PER_SECOND + value.tv_nsec;
}

/* 本程序的所有时间差都使用同一个单调时钟。 */
static uint64_t elapsed_ns(EvalRun *run, uint64_t start)
{
	uint64_t end = clock_ns(run);

	return end >= start ? end - start : 0;
}

/* 保留首次错误，后续收尾错误仍写到 stderr。 */
static int eval_error(EvalRun *run, const char *message)
{
	if (!run->error)
		run->error = message;
	fprintf(stderr, "%s\n", message);
	return -1;
}

/* 等待失败不能推定设备停止，之后只保留资源，不强制回收。 */
static int kmt_result(EvalRun *run, GpgpuKmtStatus status, const char *message)
{
	if (status == GPGPU_KMT_SUCCESS)
		return 0;
	if (status == GPGPU_KMT_WAIT_TIMEOUT || status == GPGPU_KMT_WAIT_FAILURE)
		run->uncertain = true;
	if (!run->error) {
		run->error = message;
		run->kmt_status = status;
	}
	fprintf(stderr, "%s：%s\n", message, gpgpuKmtStatusString(status));
	return -1;
}

/* 扩容失败只令统计失败，不改变已经成功的 ioctl。 */
static void record_bo(EvalRun *run, const struct gpgpu_bo_create *bo)
{
	EvalBoRecord *records;
	size_t capacity;
	uint64_t requested, mapped;

	if (run->stats_error)
		return;
	if (run->bo_count == run->bo_capacity) {
		capacity = run->bo_capacity ? run->bo_capacity * 2 : EVAL_BO_INITIAL_RECORDS;
		if (capacity < run->bo_capacity || capacity > SIZE_MAX / sizeof(*records)) {
			run->stats_error = true;
			return;
		}
		records = realloc(run->bos, capacity * sizeof(*records));
		if (!records) {
			run->stats_error = true;
			return;
		}
		run->bos = records;
		run->bo_capacity = capacity;
	}
	if (!add_ok(run->requested_bytes, bo->size, &requested) ||
	    !add_ok(run->mapped_bytes, bo->mapped_size, &mapped)) {
		run->stats_error = true;
		return;
	}
	run->bos[run->bo_count++] = (EvalBoRecord) {
		.role = run->allocation_role,
		.kind = bo->kind,
		.requested = bo->size,
		.mapped = bo->mapped_size,
	};
	run->requested_bytes = requested;
	run->mapped_bytes = mapped;
}

/*
 * __wrap_ioctl() - 观察本回放器创建的 BO
 *
 * KMT 的 ioctl 全部带一个指针参数；包装只在本 binary 链接时启用。
 * 1. 原样调用真实 ioctl，保留返回值与 errno；
 * 2. BO_CREATE 的耗时包含驱动的分配、清零；成功时记录输出长度。
 *
 * 不接管释放，不重试命令，也不把统计失败变成设备成功。
 *
 * Return: 真实 ioctl 的返回值。
 */
int __wrap_ioctl(int fd, EvalIoctlRequest request, ...)
{
	EvalRun *run = eval_observer;
	bool create = (uint32_t)request == (uint32_t)GPGPU_DRM_BO_CREATE;
	uint64_t start = 0;
	va_list ap;
	void *argument;
	int result, saved_errno;

	/* 1. 标准 KMT 调用约定是 ioctl(fd, request, &args)。 */
	va_start(ap, request);
	argument = va_arg(ap, void *);
	va_end(ap);
	if (run && create)
		start = clock_ns(run);
	result = __real_ioctl(fd, request, argument);
	saved_errno = errno;

	/* 2. 记录所有 BO，包括通知页和 RT 内部 Host 对象。 */
	if (run && create) {
		run->alloc_clear_ns += elapsed_ns(run, start);
		if (!result)
			record_bo(run, argument);
	}
	errno = saved_errno;
	return result;
}

/* 只接受完整的十进制非负数。 */
static bool unsigned_ok(const char *text, uint64_t *value)
{
	unsigned long long number;
	char *end;

	if (!text || *text < '0' || *text > '9')
		return false;
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno || *end)
		return false;
	*value = number;
	return true;
}

/* 容差只能保持本轮基准或收紧，不能失败后自动放宽。 */
static bool tolerance_ok(const char *text, double *value)
{
	char *end;
	double number;

	errno = 0;
	number = strtod(text, &end);
	if (errno || end == text || *end || !isfinite(number) || number < 0 ||
	    number > EVAL_DEFAULT_TOLERANCE)
		return false;
	*value = number;
	return true;
}

/* 参数说明不依赖设备是否可用。 */
static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "用法：%s --job PATH [--timeout-ms N] [--repeats N] [--warmups N]\n"
		"       [--backend scalar|block] [--tile N] [--atol X] [--rtol X] [--uncached]\n"
		"       [--ready PATH --start PATH --done PATH --release PATH]\n", program);
}

/* 四个 barrier 路径必须一同出现；其余数值只检查接口可表示范围。 */
static int parse_options(int argc, char **argv, EvalOptions *options)
{
	static const struct option names[] = {
		{ "job", required_argument, NULL, 'j' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "repeats", required_argument, NULL, 'r' },
		{ "warmups", required_argument, NULL, 'w' },
		{ "tile", required_argument, NULL, 'l' },
		{ "backend", required_argument, NULL, 'b' },
		{ "ready", required_argument, NULL, 'a' },
		{ "start", required_argument, NULL, 's' },
		{ "done", required_argument, NULL, 'd' },
		{ "release", required_argument, NULL, 'e' },
		{ "atol", required_argument, NULL, 'A' },
		{ "rtol", required_argument, NULL, 'R' },
		{ "uncached", no_argument, NULL, 'u' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint64_t value;
	int option;

	*options = (EvalOptions) {
		.timeout_ms = EVAL_DEFAULT_TIMEOUT_MS,
		.repeats = 1,
		.tile = EVAL_DEFAULT_TILE,
		.backend = GPGPU_EVAL_BACKEND_SCALAR,
		.atol = EVAL_DEFAULT_TOLERANCE,
		.rtol = EVAL_DEFAULT_TOLERANCE,
	};
	opterr = 0;
	while ((option = getopt_long(argc, argv, "", names, NULL)) != -1) {
		switch (option) {
		case 'j':
			options->job_path = optarg;
			break;
		case 'a':
			options->ready = optarg;
			break;
		case 's':
			options->start = optarg;
			break;
		case 'd':
			options->done = optarg;
			break;
		case 'e':
			options->release = optarg;
			break;
		case 'b':
			if (!strcmp(optarg, "scalar"))
				options->backend = GPGPU_EVAL_BACKEND_SCALAR;
			else if (!strcmp(optarg, "block"))
				options->backend = GPGPU_EVAL_BACKEND_BLOCK;
			else
				return -1;
			break;
		case 'A':
			if (!tolerance_ok(optarg, &options->atol))
				return -1;
			break;
		case 'R':
			if (!tolerance_ok(optarg, &options->rtol))
				return -1;
			break;
		case 't':
		case 'r':
		case 'w':
		case 'l':
			if (!unsigned_ok(optarg, &value))
				return -1;
			if (option == 't') {
				if (!value || value > UINT32_MAX)
					return -1;
				options->timeout_ms = value;
			} else if (option == 'l') {
				if (!value)
					return -1;
				options->tile = value;
			} else {
				if (value > SIZE_MAX || (option == 'r' && !value))
					return -1;
				if (option == 'r')
					options->repeats = value;
				else
					options->warmups = value;
			}
			break;
		case 'u':
			options->uncached = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 1;
		default:
			return -1;
		}
	}
	if (!options->job_path || optind != argc ||
	    options->warmups > SIZE_MAX - options->repeats)
		return -1;
	if ((options->ready || options->start || options->done || options->release) &&
	    !(options->ready && options->start && options->done && options->release))
		return -1;
	return 0;
}

/* 文本行必须以换行结束，避免把二进制模板当作路径的一部分。 */
static char *job_line(FILE *stream)
{
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length = getline(&line, &capacity, stream);

	if (length <= 0 || line[length - 1] != '\n' || memchr(line, 0, length)) {
		free(line);
		return NULL;
	}
	line[--length] = 0;
	if (length && line[length - 1] == '\r')
		line[--length] = 0;
	return line;
}

/* 原始数组必须是有确定长度的普通文件，不一次性读入 RAM。 */
static int open_data(EvalRun *run, EvalFile *file)
{
	struct stat info;

	file->stream = fopen(file->path, "rb");
	if (!file->stream || fstat(fileno(file->stream), &info) || !S_ISREG(info.st_mode) ||
	    info.st_size < 0) {
		fprintf(stderr, "数据路径：%s\n", file->path);
		return eval_error(run, "无法打开有确定长度的原始数据文件");
	}
	file->bytes = info.st_size;
	return 0;
}

/* 命令槽按共享字节偏移访问，内存中不重复定义字段布局。 */
static uint64_t command_get(const uint64_t *words, size_t offset)
{
	return words[offset / GPGPU_EVAL_SLOT_BYTES];
}

/* 修改的是本机字节序命令，上传函数统一编码成小端。 */
static void command_set(uint64_t *words, size_t offset, uint64_t value)
{
	words[offset / GPGPU_EVAL_SLOT_BYTES] = value;
}

/*
 * load_job() - 读取十行文本和紧随其后的命令模板
 *
 * 1. 读取版本、标识、dtype、六个输入路径与 reference 路径；
 * 2. 精确读取共享 ABI 指定的模板长度，不接受尾随数据；
 * 3. 打开原始文件，记录真实长度。数组内容稍后流式上传。
 *
 * Return: 0 或 -1；已打开文件统一由收尾函数关闭。
 */
static int load_job(EvalRun *run)
{
	EvalJob *job = &run->job;
	FILE *stream = fopen(run->options.job_path, "rb");
	char *line;
	int result = -1;

	if (!stream)
		return eval_error(run, "无法打开 job 文件");

	/* 1. 路径行与模板之间不插入 JSON 或其他文本。 */
	line = job_line(stream);
	if (!line || strcmp(line, "GPGPU-EVAL-1")) {
		free(line);
		eval_error(run, "job 版本行不正确");
		goto out;
	}
	free(line);
	job->case_id = job_line(stream);
	line = job_line(stream);
	if (!job->case_id || !*job->case_id || !line) {
		free(line);
		eval_error(run, "job 缺少 case_id 或 dtype");
		goto out;
	}
	if (!strcmp(line, "float32"))
		job->type = EVAL_F32;
	else if (!strcmp(line, "int64"))
		job->type = EVAL_I64;
	else {
		free(line);
		eval_error(run, "输出 dtype 只支持 float32 或 int64");
		goto out;
	}
	free(line);
	for (unsigned int i = 0; i < EVAL_INPUTS; i++) {
		line = job_line(stream);
		if (!line || !*line) {
			free(line);
			eval_error(run, "job 输入路径行不完整");
			goto out;
		}
		if (!strcmp(line, "-"))
			free(line);
		else
			job->inputs[i].path = line;
	}
	job->reference.path = job_line(stream);
	if (!job->reference.path || !*job->reference.path) {
		eval_error(run, "job 缺少 reference 路径");
		goto out;
	}

	/* 2. sizeof(words) 由共享参数字节数推导。 */
	if (fread(job->words, 1, sizeof(job->words), stream) != sizeof(job->words) ||
	    fgetc(stream) != EOF || ferror(stream)) {
		eval_error(run, "job 命令模板长度不正确");
		goto out;
	}
	for (unsigned int i = 0; i < EVAL_WORDS; i++)
		job->words[i] = le64toh(job->words[i]);
	job->op = command_get(job->words, GPGPU_EVAL_OP);
	job->count = command_get(job->words, GPGPU_EVAL_COUNT);
	if (!job->count ||
	    !multiply_ok(job->count, job->type == EVAL_F32 ? sizeof(float) : sizeof(int64_t),
			 &job->output_bytes) || job->output_bytes > SIZE_MAX) {
		eval_error(run, "输出长度为空或超出可表示范围");
		goto out;
	}

	/* 3. 只按实际文件长度分配，支持 strided 存储中的合法间隙。 */
	for (unsigned int i = 0; i < EVAL_INPUTS; i++) {
		if (!job->inputs[i].path)
			continue;
		if (open_data(run, &job->inputs[i]))
			goto out;
		if (!job->inputs[i].bytes || job->inputs[i].bytes > SIZE_MAX) {
			eval_error(run, "输入文件为空或超过地址空间");
			goto out;
		}
	}
	if (open_data(run, &job->reference))
		goto out;
	if (job->reference.bytes != job->output_bytes) {
		eval_error(run, "reference 长度与 COUNT、dtype 不一致");
		goto out;
	}
	result = 0;
out:
	if (fclose(stream) && !result)
		result = eval_error(run, "关闭 job 文件失败");
	return result;
}

/* 检查一个已提供数组能覆盖本算子实际需要的元素范围。 */
static bool input_size_ok(const EvalJob *job, unsigned int index, uint64_t elements,
			  uint64_t element_bytes)
{
	uint64_t bytes;

	return index < EVAL_INPUTS && job->inputs[index].stream && elements &&
		multiply_ok(elements, element_bytes, &bytes) &&
		job->inputs[index].bytes >= bytes && !(job->inputs[index].bytes % element_bytes);
}

/* 连续 shape 的正维度乘积必须可表示。 */
static bool shape_product_ok(const uint64_t *shape, unsigned int rank, uint64_t *elements)
{
	uint64_t product = 1;

	for (unsigned int i = 0; i < rank; i++)
		if (!shape[i] || !multiply_ok(product, shape[i], &product))
			return false;
	*elements = product;
	return true;
}

/* 非负元素 stride 的最大访问下标加 1；零 stride 合法。 */
static bool storage_span_ok(const uint64_t *shape, const uint64_t *stride, unsigned int rank,
			    uint64_t *span)
{
	uint64_t last = 0;

	for (unsigned int i = 0; i < rank; i++) {
		uint64_t part;

		if (!shape[i] || !multiply_ok(shape[i] - 1, stride[i], &part) ||
		    !add_ok(last, part, &last))
			return false;
	}
	return add_ok(last, 1, span);
}

/* 对归约布局检查 width、inner 与完整行数。 */
static bool rows_ok(uint64_t count, uint64_t width, uint64_t inner, uint64_t *rows)
{
	if (!width || !inner || count % width)
		return false;
	*rows = count / width;
	return *rows && !(*rows % inner);
}

/*
 * layout_valid() - 检查会影响真实数组访问范围的算子属性
 *
 * 1. 按 Kernel 的 IN/DIMS 契约计算正维度、逻辑长度或 stride span；
 * 2. 用真实文件长度检查访问上界，并确认输出 dtype；
 * 3. 为复合算子保存输入工作项数与归约行数，用于暂存分配。
 *
 * 不登记原始 shape 副本，不改写 Host 的布局，也不计算参考结果。
 *
 * Return: 契约与长度均成立时为 true。
 */
static bool layout_valid(EvalJob *job)
{
	uint64_t d[EVAL_DIMS];
	uint64_t shape[4], count = job->count;
	uint64_t a, b, c, n, width, inner;
	bool integer = false;

	for (unsigned int i = 0; i < EVAL_DIMS; i++)
		d[i] = command_get(job->words, GPGPU_EVAL_DIMS + i * GPGPU_EVAL_SLOT_BYTES);
	job->work_count = count;

	/* 1. 只检查本次操作真正使用的维度和数组。 */
	switch (job->op) {
	case GPGPU_EVAL_LINEAR_F32:
	case GPGPU_EVAL_LINEAR_W8:
		if (!d[0] || !d[1] || !d[2] || !multiply_ok(d[0], d[1], &a) || a != count ||
		    !multiply_ok(d[0], d[2], &a) || !multiply_ok(d[1], d[2], &b) ||
		    !input_size_ok(job, 0, a, sizeof(float)) ||
		    !input_size_ok(job, 1, b,
				   job->op == GPGPU_EVAL_LINEAR_W8 ? 1 : sizeof(float)) ||
		    (job->op == GPGPU_EVAL_LINEAR_W8 &&
		     !input_size_ok(job, 2, d[1], sizeof(float))) ||
		    (job->inputs[3].stream && !input_size_ok(job, 3, d[1], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_BMM:
		shape[0] = d[0];
		shape[1] = d[1];
		shape[2] = d[2];
		if (!shape_product_ok(shape, 3, &a) || a != count || !d[3] || d[10] > 1)
			return false;
		shape[2] = d[3];
		if (d[10]) {
			if (!storage_span_ok(shape, d + 4, 3, &a))
				return false;
		} else if (!shape_product_ok(shape, 3, &a)) {
			return false;
		}
		shape[1] = d[3];
		shape[2] = d[2];
		if (d[10]) {
			if (!storage_span_ok(shape, d + 7, 3, &b))
				return false;
		} else if (!shape_product_ok(shape, 3, &b)) {
			return false;
		}
		if (!input_size_ok(job, 0, a, sizeof(float)) ||
		    !input_size_ok(job, 1, b, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_EMBEDDING_W8:
		if (!d[0] || !job->inputs[1].stream || job->inputs[1].bytes % d[0] ||
		    !job->inputs[0].stream || job->inputs[0].bytes % sizeof(int64_t))
			return false;
		job->vocabulary = job->inputs[1].bytes / d[0];
		a = job->inputs[0].bytes / sizeof(int64_t);
		if (!job->vocabulary || !multiply_ok(a, d[0], &b) || b != count ||
		    !input_size_ok(job, 2, job->vocabulary, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_ADD:
	case GPGPU_EVAL_MUL:
	case GPGPU_EVAL_SUB:
	case GPGPU_EVAL_DIV:
		if (!input_size_ok(job, 0, d[0], sizeof(float)) ||
		    !input_size_ok(job, 1, d[1], sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_MUL_SCALAR:
	case GPGPU_EVAL_ADD_SCALAR:
	case GPGPU_EVAL_SILU:
	case GPGPU_EVAL_SIGMOID:
	case GPGPU_EVAL_SIN:
	case GPGPU_EVAL_COS:
		if (!input_size_ok(job, 0, count, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_REDUCE_MAX:
	case GPGPU_EVAL_REDUCE_SUM:
	case GPGPU_EVAL_REDUCE_RMS:
	case GPGPU_EVAL_DFL_PROJECT:
		if (!d[0] || !d[1] || count % d[1] || !multiply_ok(count, d[0], &a) ||
		    !input_size_ok(job, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_EXP_SHIFT:
	case GPGPU_EVAL_DIV_ROW:
	case GPGPU_EVAL_RMS_APPLY:
	case GPGPU_EVAL_SOFTMAX:
	case GPGPU_EVAL_RMSNORM:
		if (!rows_ok(count, d[0], d[1], &job->rows) ||
		    !input_size_ok(job, 0, count, sizeof(float)))
			return false;
		if ((job->op == GPGPU_EVAL_EXP_SHIFT || job->op == GPGPU_EVAL_DIV_ROW) &&
		    !input_size_ok(job, 1, job->rows, sizeof(float)))
			return false;
		if ((job->op == GPGPU_EVAL_RMS_APPLY || job->op == GPGPU_EVAL_RMSNORM) &&
		    !input_size_ok(job, 1, d[0], sizeof(float)))
			return false;
		if (job->op == GPGPU_EVAL_RMS_APPLY &&
		    !input_size_ok(job, 2, job->rows, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_DFL:
		if (!d[0] || !d[1] || count % d[1] ||
		    !multiply_ok(count, d[0], &job->work_count) ||
		    !input_size_ok(job, 0, job->work_count, sizeof(float)))
			return false;
		job->rows = count;
		break;
	case GPGPU_EVAL_BATCHNORM:
		if (!d[0] || !d[1] || !multiply_ok(d[0], d[1], &a) || count % a ||
		    !input_size_ok(job, 0, count, sizeof(float)))
			return false;
		for (unsigned int i = 1; i <= 4; i++)
			if (!input_size_ok(job, i, d[0], sizeof(float)))
				return false;
		break;
	case GPGPU_EVAL_CONV2D:
		shape[0] = d[3];
		shape[1] = d[4];
		shape[2] = d[5];
		if (!shape_product_ok(shape, 3, &a) || count % a || !d[14] || !d[0] ||
		    d[0] % d[14] || d[3] % d[14] || !d[8] || !d[9] || !d[12] || !d[13])
			return false;
		n = count / a;
		shape[0] = n;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		if (!shape_product_ok(shape, 4, &a) || !input_size_ok(job, 0, a, sizeof(float)))
			return false;
		shape[0] = d[3];
		shape[1] = d[0] / d[14];
		shape[2] = d[6];
		shape[3] = d[7];
		if (!shape_product_ok(shape, 4, &a) || !input_size_ok(job, 1, a, sizeof(float)) ||
		    (job->inputs[2].stream && !input_size_ok(job, 2, d[3], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_MAXPOOL2D:
		shape[0] = d[0];
		shape[1] = d[3];
		shape[2] = d[4];
		if (!shape_product_ok(shape, 3, &a) || count % a || !d[5] || !d[6] ||
		    !d[7] || !d[8] || !d[11] || !d[12])
			return false;
		n = count / a;
		shape[0] = n;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		if (!shape_product_ok(shape, 4, &a) || !input_size_ok(job, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_NEAREST2D:
		if (!d[0] || !d[1] || !d[2] || !d[3] ||
		    !multiply_ok(d[2], d[3], &a) || count % a)
			return false;
		n = count / a;
		if (!multiply_ok(n, d[0], &a) || !multiply_ok(a, d[1], &a) ||
		    !input_size_ok(job, 0, a, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_CONCAT:
		width = d[1];
		inner = d[0];
		if (!width || !inner || !d[2] || d[2] > EVAL_INPUTS ||
		    !multiply_ok(width, inner, &a) || count % a)
			return false;
		n = count / a;
		b = 0;
		for (unsigned int i = 0; i < d[2]; i++) {
			if (!d[3 + i] || !add_ok(b, d[3 + i], &b) ||
			    !multiply_ok(n, d[3 + i], &a) || !multiply_ok(a, inner, &a) ||
			    !input_size_ok(job, i, a, sizeof(float)))
				return false;
		}
		if (b != width)
			return false;
		break;
	case GPGPU_EVAL_COPY:
		if (d[0] > GPGPU_EVAL_COPY_I64 || d[1] > EVAL_COPY_RANK_MAX)
			return false;
		integer = d[0] == GPGPU_EVAL_COPY_I64;
		a = count;
		if (d[1] && (!shape_product_ok(d + 2, d[1], &b) || b != count ||
			     !storage_span_ok(d + 2, d + 2 + d[1], d[1], &a)))
			return false;
		if (!input_size_ok(job, 0, a,
				   d[0] == GPGPU_EVAL_COPY_F32 ? sizeof(float) : sizeof(int64_t)))
			return false;
		break;
	case GPGPU_EVAL_ROPE:
		if (!d[0] || !d[1] || !d[2] || (d[2] & 1) || d[7] > 1 ||
		    !multiply_ok(d[0], d[1], &a) || !multiply_ok(a, d[2], &a) || count % a)
			return false;
		n = count / a;
		shape[0] = n;
		shape[1] = d[0];
		shape[2] = d[1];
		shape[3] = d[2];
		a = count;
		if (d[7] && !storage_span_ok(shape, d + 3, 4, &a))
			return false;
		if (!multiply_ok(n, d[1], &b) || !multiply_ok(b, d[2], &b) ||
		    !input_size_ok(job, 0, a, sizeof(float)) ||
		    !input_size_ok(job, 1, b, sizeof(float)) ||
		    !input_size_ok(job, 2, b, sizeof(float)))
			return false;
		break;
	case GPGPU_EVAL_BOX_DECODE:
		if (!d[0] || d[1] > 1 || !multiply_ok(d[0], 4, &a) || count % a)
			return false;
		n = count / a;
		if (!multiply_ok(n, 2, &b) || !multiply_ok(b, d[0], &b) ||
		    !input_size_ok(job, 0, count, sizeof(float)) ||
		    !input_size_ok(job, 1, b, sizeof(float)) ||
		    (job->inputs[2].stream && !input_size_ok(job, 2, d[0], sizeof(float))))
			return false;
		break;
	case GPGPU_EVAL_ARANGE:
	case GPGPU_EVAL_FULL:
		if (d[2] != GPGPU_EVAL_TYPE_F32 && d[2] != GPGPU_EVAL_TYPE_I64)
			return false;
		integer = d[2] == GPGPU_EVAL_TYPE_I64;
		break;
	default:
		return false;
	}

	/* 2. dtype 只由共享模式决定，其他算子输出 FP32。 */
	if (integer != (job->type == EVAL_I64))
		return false;

	/* 3. scratch 的字节换算必须能交给 size_t 接口。 */
	return multiply_ok(job->work_count, sizeof(float), &c) && c <= SIZE_MAX;
}

/* 按正块宽向上取整，避免 count+width-1 的加法溢出。 */
static uint64_t blocks_for(uint64_t count, uint64_t width)
{
	return count / width + !!(count % width);
}

/*
 * select_backend() - 为基本算子选择标量元素或张量块工作项
 *
 * main() 在 layout_valid() 成功后调用，维度均已按原始文件验证。
 * 例：Linear 的 M=9、N=33 时，输出仍有 297 项，通用 tile 对应 2*2 个工作项。
 *
 * 1. 默认保留标量路径与完整输出计数，非目标算子不解释 AUX 后端标志；
 * 2. 按共享块宽计算派发量；Conv 推导 batch 后逐组分块。
 *
 * 不改原始 count/output_bytes、复合算子的 work_count 或输入存储布局。
 *
 * Return: 0 表示工作项数可表示，否则为 -1。
 */
static int select_backend(EvalRun *run)
{
	EvalJob *job = &run->job;
	uint64_t d[EVAL_DIMS];
	uint64_t m, n, blocks, batches = 1;

	/* 1. 请求 block 也不会改变其他算子及复合 lowering 的原路径。 */
	job->backend = GPGPU_EVAL_BACKEND_SCALAR;
	job->launch_count = job->count;
	if (run->options.backend != GPGPU_EVAL_BACKEND_BLOCK)
		return 0;

	/* 2. 只对既有基本算子启用通用指令，块宽不在回放器中另抄。 */
	for (unsigned int i = 0; i < EVAL_DIMS; i++)
		d[i] = command_get(job->words, GPGPU_EVAL_DIMS + i * GPGPU_EVAL_SLOT_BYTES);
	switch (job->op) {
	case GPGPU_EVAL_LINEAR_F32:
	case GPGPU_EVAL_LINEAR_W8:
		m = d[0];
		n = d[1];
		break;
	case GPGPU_EVAL_BMM:
		batches = d[0];
		m = d[1];
		n = d[2];
		break;
	case GPGPU_EVAL_CONV2D:
		if (!multiply_ok(d[4], d[5], &m) || !multiply_ok(d[3], m, &batches))
			return eval_error(run, "卷积每张输出元素数溢出");
		batches = job->count / batches;
		if (!multiply_ok(batches, d[14], &batches))
			return eval_error(run, "卷积 batch 与 groups 的乘积溢出");
		n = d[3] / d[14];
		break;
	case GPGPU_EVAL_COPY:
	case GPGPU_EVAL_CONCAT:
	case GPGPU_EVAL_NEAREST2D:
		job->launch_count = blocks_for(job->count, GPGPU_TENSOR_BLOCK_ELEMENTS);
		job->backend = GPGPU_EVAL_BACKEND_BLOCK;
		return 0;
	default:
		return 0;
	}
	if (!multiply_ok(batches, blocks_for(m, GPGPU_TENSOR_TILE_M), &blocks) ||
	    !multiply_ok(blocks, blocks_for(n, GPGPU_TENSOR_TILE_N), &job->launch_count))
		return eval_error(run, "矩阵块工作项数溢出");
	job->backend = GPGPU_EVAL_BACKEND_BLOCK;
	return 0;
}

/* FP32 位不经过整数数值转换，命令槽高 32 位保持零。 */
static void constant_set(uint64_t *words, size_t offset, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	command_set(words, offset, bits);
}

/* 数学常量只在这里初始化，Host 模板和 Kernel 不保存第二份数值。 */
static void initialize_constants(uint64_t *words)
{
	constant_set(words, GPGPU_EVAL_ONE, 1.0f);
	constant_set(words, GPGPU_EVAL_LN2_HI, 0.693145751953125f);
	constant_set(words, GPGPU_EVAL_LN2_LO, 1.428606765330187045e-6f);
	constant_set(words, GPGPU_EVAL_LOG2E, 1.4426950408889634074f);
	constant_set(words, GPGPU_EVAL_C2, 1.0f / 2.0f);
	constant_set(words, GPGPU_EVAL_C3, 1.0f / 6.0f);
	constant_set(words, GPGPU_EVAL_C4, 1.0f / 24.0f);
	constant_set(words, GPGPU_EVAL_C5, 1.0f / 120.0f);
	constant_set(words, GPGPU_EVAL_C6, 1.0f / 720.0f);
	constant_set(words, GPGPU_EVAL_EXP_LO, -80.0f);
	constant_set(words, GPGPU_EVAL_EXP_HI, 80.0f);
	constant_set(words, GPGPU_EVAL_ZERO, 0.0f);
	constant_set(words, GPGPU_EVAL_NEGONE, -1.0f);
	constant_set(words, GPGPU_EVAL_NEGINF, -INFINITY);
	constant_set(words, GPGPU_EVAL_TWO, 2.0f);
	constant_set(words, GPGPU_EVAL_INV_TWO_PI, 0.15915494309189533577f);
	constant_set(words, GPGPU_EVAL_TWO_PI_HI, 6.28125f);
	constant_set(words, GPGPU_EVAL_TWO_PI_LO, 0.0019353071795864769253f);
	constant_set(words, GPGPU_EVAL_PI, 3.14159265358979323846f);
	constant_set(words, GPGPU_EVAL_HALF_PI, 1.57079632679489661923f);
	constant_set(words, GPGPU_EVAL_INV_FACT7, 1.0f / 5040.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT8, 1.0f / 40320.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT9, 1.0f / 362880.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT10, 1.0f / 3628800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT11, 1.0f / 39916800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT12, 1.0f / 479001600.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT13, 1.0f / 6227020800.0f);
	constant_set(words, GPGPU_EVAL_INV_FACT14, (float)(1.0 / 87178291200.0));
}

/* 小端 INT64 原始位，供 token 值域和精确比较使用。 */
static int64_t integer_value(const uint8_t *bytes)
{
	uint64_t value;

	memcpy(&value, bytes, sizeof(value));
	return (int64_t)le64toh(value);
}

/* 只检查需要整数值域约束的输入，不在 CPU 上生成算子输出。 */
static bool integer_input_valid(const EvalJob *job, unsigned int index,
				const uint8_t *bytes, size_t length)
{
	bool embedding = job->op == GPGPU_EVAL_EMBEDDING_W8 && !index;
	bool conversion = job->op == GPGPU_EVAL_COPY && !index &&
		command_get(job->words, GPGPU_EVAL_D0) == GPGPU_EVAL_COPY_I64_TO_F32;

	if (!embedding && !conversion)
		return true;
	for (size_t i = 0; i < length; i += sizeof(int64_t)) {
		int64_t value = integer_value(bytes + i);

		if ((embedding && (value < 0 || (uint64_t)value >= job->vocabulary)) ||
		    (conversion && (value < INT32_MIN || value > INT32_MAX)))
			return false;
	}
	return true;
}

/*
 * 数据、命令及 scratch 默认交给 RT；UNCACHED 仅改变这些评估对象。
 * 额外 KMT 对象要在 RT 卸载队列之后释放，控制对象的缓存约定不变。
 */
static int allocate_buffer(EvalRun *run, const char *role, uint64_t bytes,
			   GpgpuKmtMemory **memory)
{
	GpgpuKmtStatus status;
	uint64_t start, allocated, duration;

	run->allocation_role = role;
	if (!bytes || bytes > SIZE_MAX)
		return eval_error(run, "BO 字节长度不可表示");

	start = clock_ns(run);
	allocated = run->alloc_clear_ns;
	if (run->options.uncached)
		status = gpgpuKmtAllocMemory(run->context, GPGPU_MEMORY_VRAM, GPGPU_BO_UNCACHED,
					     bytes, GPGPU_VM_PAGE_SIZE,
					     GPGPU_VM_READ | GPGPU_VM_WRITE, 0, memory);
	else
		status = gpgpu_rt_buffer_alloc(run->rt, bytes, memory);
	duration = elapsed_ns(run, start);
	allocated = run->alloc_clear_ns - allocated;
	run->buffer_setup_ns += duration >= allocated ? duration - allocated : 0;
	return kmt_result(run, status, "分配 VRAM BO 失败");
}

/*
 * upload_input() - 经固定小缓冲流式装载一个原始文件
 *
 * 1. 每次从原文件读取不超过暂存区的一段，核对必要的整数值域；
 * 2. 同步 upload_range 到该文件的完整 BO 偏移，成功后才复用读缓冲；
 * 3. 长度变化或 I/O 失败直接返回，已上传段不回退。
 *
 * Return: 0 或 -1，未知完成由 kmt_result() 标记并保留资源。
 */
static int upload_input(EvalRun *run, unsigned int index)
{
	EvalFile *file = &run->job.inputs[index];

	for (uint64_t offset = 0; offset < file->bytes;) {
		size_t length = file->bytes - offset > EVAL_STAGING_BYTES ? EVAL_STAGING_BYTES :
								       file->bytes - offset;
		uint64_t start = clock_ns(run);

		/* 1. 读取原始字节，不把整块权重保留在客体 RAM 中。 */
		if (fread(run->buffer, 1, length, file->stream) != length)
			return eval_error(run, "读取输入失败或长度改变");
		run->input_read_ns += elapsed_ns(run, start);
		if (!integer_input_valid(&run->job, index, run->buffer, length))
			return eval_error(run, "token 或 signed32 值域不合法");

		/* 2. range 的 offset 始终以完整数组起点为基准。 */
		start = clock_ns(run);
		if (kmt_result(run, gpgpu_rt_upload_range(run->rt, run->inputs[index], offset,
							run->buffer, length), "上传输入失败"))
			return -1;
		run->h2d_ns += elapsed_ns(run, start);
		run->h2d_bytes += length;
		run->h2d_chunks++;
		offset += length;
	}

	/* 3. 防止统计长度之后原文件又追加内容。 */
	if (fgetc(file->stream) != EOF || ferror(file->stream))
		return eval_error(run, "输入文件在装载期间改变长度");
	return 0;
}

/* tile 的硬上界只来自共享执行能力，不复制设备数值。 */
static int select_tile(EvalRun *run)
{
	uint64_t blocks, limit, wanted;

	if (!EVAL_BLOCK_THREADS || EVAL_BLOCK_THREADS > GPGPU_MAX_BLOCK_THREADS)
		return eval_error(run, "工作组大小超过共享执行能力");
	blocks = GPGPU_MAX_TOTAL_THREADS / EVAL_BLOCK_THREADS;
	if (blocks > GPGPU_MAX_BLOCKS)
		blocks = GPGPU_MAX_BLOCKS;
	if (blocks > UINT32_MAX)
		blocks = UINT32_MAX;
	if (!blocks || !multiply_ok(blocks, EVAL_BLOCK_THREADS, &limit))
		return eval_error(run, "共享执行能力不能容纳一个工作组");
	wanted = run->options.tile < limit ? run->options.tile : limit;
	blocks = wanted / EVAL_BLOCK_THREADS + !!(wanted % EVAL_BLOCK_THREADS);
	if (!multiply_ok(blocks, EVAL_BLOCK_THREADS, &run->tile) || run->tile > limit)
		return eval_error(run, "tile 工作组取整超过执行上限");
	return 0;
}

/*
 * prepare_memory() - 建立固定 RT 对象和本用例所需数组
 *
 * 1. 建立小暂存区并装载内建手写 Kernel；
 * 2. 按真实文件长度分配、流式上传输入，再分配输出与命令块；
 * 3. 只为复合算子分配必需 scratch，填设备地址与数学常量。
 *
 * BO_CREATE 的驱动耗时由包装单独记录，从代码加载总耗时中扣除，
 * 避免分配清零与代码加载重复计时。
 *
 * Return: 0 或 -1；不在局部失败路径释放仍可能使用的资源。
 */
static int prepare_memory(EvalRun *run)
{
	EvalJob *job = &run->job;
	uint64_t start, allocated, duration, bytes;

	/* 1. 所有 RT 参数、Ring 和 Signal 仍使用现有实现。 */
	run->allocation_role = "notify";
	start = clock_ns(run);
	allocated = run->alloc_clear_ns;
	if (kmt_result(run, gpgpuKmtOpen(NULL, &run->context), "打开 DRM 失败"))
		return -1;
	run->allocation_role = "runtime";
	if (kmt_result(run, gpgpu_rt_create(run->context, EVAL_STAGING_BYTES, &run->rt),
		       "建立 RT 失败"))
		return -1;
	duration = elapsed_ns(run, start);
	allocated = run->alloc_clear_ns - allocated;
	run->runtime_setup_ns = duration >= allocated ? duration - allocated : 0;
	run->allocation_role = "kernel";
	start = clock_ns(run);
	allocated = run->alloc_clear_ns;
	if (kmt_result(run, gpgpu_rt_kernel_load(run->rt, model_eval_code,
					      sizeof(model_eval_code), EVAL_RT_ARGS, &run->kernel),
		       "装载手写 Kernel 失败"))
		return -1;
	duration = elapsed_ns(run, start);
	allocated = run->alloc_clear_ns - allocated;
	run->kernel_load_ns = duration >= allocated ? duration - allocated : 0;

	/* 2. 输入文件与 BO 一一对应，输出长度只取完整 COUNT。 */
	for (unsigned int i = 0; i < EVAL_INPUTS; i++) {
		if (!job->inputs[i].stream)
			continue;
		if (allocate_buffer(run, job->inputs[i].path, job->inputs[i].bytes,
				    &run->inputs[i]) || upload_input(run, i))
			return -1;
	}
	if (allocate_buffer(run, "output", job->output_bytes, &run->output) ||
	    allocate_buffer(run, "command", GPGPU_EVAL_PARAM_BYTES, &run->command))
		return -1;

	/* 3. stats 跨归约步骤复用，最多使用共享 ABI 的三个 scratch 槽。 */
	if (job->op == GPGPU_EVAL_SOFTMAX || job->op == GPGPU_EVAL_RMSNORM ||
	    job->op == GPGPU_EVAL_DFL) {
		if (!multiply_ok(job->rows, sizeof(float), &bytes) ||
		    allocate_buffer(run, "scratch_stats", bytes, &run->scratch[0]))
			return -1;
	}
	if (job->op == GPGPU_EVAL_SOFTMAX || job->op == GPGPU_EVAL_DFL) {
		if (!multiply_ok(job->work_count, sizeof(float), &bytes) ||
		    allocate_buffer(run, "scratch_exp", bytes, &run->scratch[1]))
			return -1;
	}
	if (job->op == GPGPU_EVAL_DFL &&
	    allocate_buffer(run, "scratch_prob", bytes, &run->scratch[2]))
		return -1;
	for (unsigned int i = 0; i < EVAL_INPUTS; i++)
		command_set(job->words, GPGPU_EVAL_IN0 + i * GPGPU_EVAL_SLOT_BYTES,
			    run->inputs[i] ? run->inputs[i]->gpu_address : 0);
	for (unsigned int i = 0; i < EVAL_SCRATCHES; i++)
		command_set(job->words, GPGPU_EVAL_AUX0 + i * GPGPU_EVAL_SLOT_BYTES,
			    run->scratch[i] ? run->scratch[i]->gpu_address : 0);
	command_set(job->words, GPGPU_EVAL_OUT, run->output->gpu_address);
	if (job->backend == GPGPU_EVAL_BACKEND_BLOCK) {
		/* 仅块基本算子复用这两个槽，复合算子仍保存 scratch。 */
		command_set(job->words, GPGPU_EVAL_AUX1, job->count);
		command_set(job->words, GPGPU_EVAL_AUX2, GPGPU_EVAL_BACKEND_BLOCK);
	}
	initialize_constants(job->words);
	if (run->stats_error)
		return eval_error(run, "无法完整记录 BO 分配统计");
	return 0;
}

/* 上传一个完整命令，设备不会看见只更新了一半的字段。 */
static int upload_command(EvalRun *run, const uint64_t *words, EvalSample *sample)
{
	uint64_t wire[EVAL_WORDS];
	uint64_t start = clock_ns(run);

	for (unsigned int i = 0; i < EVAL_WORDS; i++)
		wire[i] = htole64(words[i]);
	if (kmt_result(run, gpgpu_rt_upload_range(run->rt, run->command, 0, wire, sizeof(wire)),
		       "上传算子命令失败"))
		return -1;
	if (sample) {
		sample->params_ns += elapsed_ns(run, start);
		sample->params_bytes += sizeof(wire);
	}
	return 0;
}

/* COUNT=0 只用于建立空闲队列，不计作真实算子吞吐。 */
static int prepare_queue(EvalRun *run)
{
	uint64_t words[EVAL_WORDS];
	uint64_t args[EVAL_RT_ARGS] = { 0 };
	uint64_t start = clock_ns(run);

	memcpy(words, run->job.words, sizeof(words));
	command_set(words, GPGPU_EVAL_COUNT, 0);
	args[GPGPU_EVAL_RT_COMMAND / GPGPU_EVAL_SLOT_BYTES] = run->command->gpu_address;
	args[GPGPU_EVAL_RT_BASE / GPGPU_EVAL_SLOT_BYTES] = 0;
	if (upload_command(run, words, NULL) ||
	    kmt_result(run, gpgpu_rt_dispatch(run->rt, &run->kernel, args, EVAL_RT_ARGS,
					   (GpgpuRtDim3) { 1, 1, 1 },
					   (GpgpuRtDim3) { 1, 1, 1 }, run->options.timeout_ms),
		       "建立空闲队列失败"))
		return -1;
	run->queue_prepare_ns = elapsed_ns(run, start);
	return 0;
}

/*
 * dispatch_step() - 执行一个 lowering 步骤的全部工作项
 *
 * 1. 只改 OP、COUNT 和本步输入输出，原始 DIMS 与完整数组地址保持不变；
 * 2. 上传一次命令，再让 RT 参数 BASE 逐 tile 前进；
 * 3. 每个 tile 同步完成后才发布下一个，尾部 Lane 由 COUNT 排除。
 *
 * 例：tile=128、COUNT=300，BASE 依次为 0、128、256；最后一个工作组
 * 覆盖到 319，但 300..319 不访存。不能把数组指针也加上 BASE。
 *
 * Return: 0 或 -1；失败前已完成的 tile 不回退。
 */
static int dispatch_step(EvalRun *run, EvalSample *sample, uint64_t op, uint64_t count,
			 uint64_t in0, uint64_t in1, uint64_t in2, uint64_t output)
{
	uint64_t words[EVAL_WORDS];
	uint64_t args[EVAL_RT_ARGS] = { 0 };

	/* 1. 未被覆盖的输入槽仍来自原始命令。 */
	memcpy(words, run->job.words, sizeof(words));
	command_set(words, GPGPU_EVAL_OP, op);
	command_set(words, GPGPU_EVAL_COUNT, count);
	command_set(words, GPGPU_EVAL_IN0, in0);
	command_set(words, GPGPU_EVAL_IN1, in1);
	command_set(words, GPGPU_EVAL_IN2, in2);
	command_set(words, GPGPU_EVAL_OUT, output);

	/* 2. 命令块复用，但从不覆盖未知完成的上一条命令。 */
	if (upload_command(run, words, sample))
		return -1;
	args[GPGPU_EVAL_RT_COMMAND / GPGPU_EVAL_SLOT_BYTES] = run->command->gpu_address;
	for (uint64_t base = 0; base < count;) {
		uint64_t length = count - base > run->tile ? run->tile : count - base;
		uint64_t blocks = length / EVAL_BLOCK_THREADS + !!(length % EVAL_BLOCK_THREADS);
		uint64_t start = clock_ns(run);
		GpgpuKmtStatus status;

		/* 3. BASE 是全局编号，不能在每块从 0 重新开始。 */
		args[GPGPU_EVAL_RT_BASE / GPGPU_EVAL_SLOT_BYTES] = base;
		/* 记录派发尝试，不把补齐线程计入工作项。 */
		sample->work_items += length;
		sample->tile_dispatches++;
		status = gpgpu_rt_dispatch(run->rt, &run->kernel, args, EVAL_RT_ARGS,
					  (GpgpuRtDim3) { blocks, 1, 1 },
					  (GpgpuRtDim3) { EVAL_BLOCK_THREADS, 1, 1 },
					  run->options.timeout_ms);
		sample->dispatch_ns += elapsed_ns(run, start);
		if (kmt_result(run, status, "算子 tile 派发失败")) {
			/* RT 的发布失败也可能留下未确认的参数访问。 */
			run->uncertain = true;
			return -1;
		}
		base += length;
	}
	return 0;
}

/* Softmax 的 max 与 sum 复用同一个 stats BO，不额外申请对象。 */
static int softmax_steps(EvalRun *run, EvalSample *sample, uint64_t output)
{
	uint64_t input = run->inputs[0]->gpu_address;
	uint64_t stats = run->scratch[0]->gpu_address;
	uint64_t exponentials = run->scratch[1]->gpu_address;

	if (dispatch_step(run, sample, GPGPU_EVAL_REDUCE_MAX, run->job.rows,
			  input, 0, 0, stats) ||
	    dispatch_step(run, sample, GPGPU_EVAL_EXP_SHIFT, run->job.work_count,
			  input, stats, 0, exponentials) ||
	    dispatch_step(run, sample, GPGPU_EVAL_REDUCE_SUM, run->job.rows,
			  exponentials, 0, 0, stats) ||
	    dispatch_step(run, sample, GPGPU_EVAL_DIV_ROW, run->job.work_count,
			  exponentials, stats, 0, output))
		return -1;
	return 0;
}

/* 完整算子计时包含所有参数上传、步骤与同步派发，不含验证回读。 */
static int run_operator(EvalRun *run, EvalSample *sample)
{
	uint64_t start = clock_ns(run);
	uint64_t input[EVAL_INPUTS];
	int result;

	for (unsigned int i = 0; i < EVAL_INPUTS; i++)
		input[i] = run->inputs[i] ? run->inputs[i]->gpu_address : 0;
	switch (run->job.op) {
	case GPGPU_EVAL_SOFTMAX:
		result = softmax_steps(run, sample, run->output->gpu_address);
		break;
	case GPGPU_EVAL_RMSNORM:
		result = dispatch_step(run, sample, GPGPU_EVAL_REDUCE_RMS, run->job.rows,
				       input[0], 0, 0, run->scratch[0]->gpu_address);
		if (!result)
			result = dispatch_step(run, sample, GPGPU_EVAL_RMS_APPLY, run->job.count,
					       input[0], input[1], run->scratch[0]->gpu_address,
					       run->output->gpu_address);
		break;
	case GPGPU_EVAL_DFL:
		result = softmax_steps(run, sample, run->scratch[2]->gpu_address);
		if (!result)
			result = dispatch_step(run, sample, GPGPU_EVAL_DFL_PROJECT, run->job.count,
					       run->scratch[2]->gpu_address, 0, 0,
					       run->output->gpu_address);
		break;
	default:
		result = dispatch_step(run, sample, run->job.op, run->job.launch_count,
				       input[0], input[1], input[2], run->output->gpu_address);
		break;
	}
	sample->operator_ns = elapsed_ns(run, start);
	sample->complete = !result;
	return result;
}

/* 小端 FP32 原始位解码，与 CPU 本机字节序无关。 */
static float floating_value(const uint8_t *bytes)
{
	uint32_t bits;
	float value;

	memcpy(&bits, bytes, sizeof(bits));
	bits = le32toh(bits);
	memcpy(&value, &bits, sizeof(value));
	return value;
}

/* 有限数值的误差只用于比较与统计，不参与设备算子计算。 */
static void record_difference(EvalMetrics *metrics, long double actual, long double expected,
			      bool passed)
{
	long double difference = fabsl(actual - expected);
	long double relative = expected ? difference / fabsl(expected) :
					 difference ? INFINITY : 0;

	metrics->compared++;
	metrics->failed += !passed;
	if (difference > metrics->maxabs)
		metrics->maxabs = difference;
	if (relative > metrics->maxrel)
		metrics->maxrel = relative;
	metrics->error_squared += difference * difference;
	metrics->reference_squared += expected * expected;
}

/* 复制、拼接和 nearest 只移动元素，不能用浮点容差掩盖位模式损坏。 */
static bool exact_compare_enabled(const EvalJob *job)
{
	return job->op == GPGPU_EVAL_COPY || job->op == GPGPU_EVAL_CONCAT ||
		job->op == GPGPU_EVAL_NEAREST2D;
}

/* 布局逐位比较；算术 NaN/Inf 查种类与符号，有限值用预定容差。 */
static void compare_floating(EvalRun *run, EvalMetrics *metrics, const uint8_t *actual_bytes,
			     const uint8_t *expected_bytes)
{
	long double difference;
	bool passed;
	float actual = floating_value(actual_bytes);
	float expected = floating_value(expected_bytes);
	bool exact = exact_compare_enabled(&run->job);
	bool same_bits = !memcmp(actual_bytes, expected_bytes, sizeof(float));

	if (!isfinite(actual) || !isfinite(expected)) {
		passed = exact ? same_bits :
			(((isnan(actual) && isnan(expected)) ||
			  (isinf(actual) && isinf(expected))) &&
			 !!signbit(actual) == !!signbit(expected));
		metrics->compared++;
		metrics->failed += !passed;
		metrics->nonfinite_mismatches += !passed;
		if (!passed) {
			metrics->maxabs = INFINITY;
			metrics->maxrel = INFINITY;
			metrics->error_squared = INFINITY;
		}
		return;
	}
	difference = fabsl((long double)actual - expected);
	passed = exact ? same_bits :
		difference <= run->options.atol + run->options.rtol * fabsl(expected);
	record_difference(metrics, actual, expected, passed);
}

/*
 * compare_output() - 分段回读并比较 reference 的全部元素
 *
 * 1. 同步回读一个小段，不读取失败 COPY 的暂存内容；
 * 2. 从原 reference 读取同一段，按 dtype 比较所有值；
 * 3. 检查 reference 没有在比较期间改变长度，保留完整误差统计。
 *
 * Return: I/O 或设备错误为 -1；数值不一致记入 failed，不打印逐值日志。
 */
static int compare_output(EvalRun *run, EvalSample *sample)
{
	EvalFile *reference = &run->job.reference;
	size_t width = run->job.type == EVAL_F32 ? sizeof(float) : sizeof(int64_t);

	if (fseeko(reference->stream, 0, SEEK_SET))
		return eval_error(run, "无法重新定位 reference");
	for (uint64_t offset = 0; offset < reference->bytes;) {
		size_t length = EVAL_STAGING_BYTES;
		uint64_t start;
		GpgpuKmtStatus status;

		if (length > reference->bytes - offset)
			length = reference->bytes - offset;

		/* 1. D2H 不计入完整算子的执行时间。 */
		start = clock_ns(run);
		status = gpgpu_rt_download_range(run->rt, run->output, offset, run->buffer, length);
		sample->d2h_ns += elapsed_ns(run, start);
		if (kmt_result(run, status, "回读输出失败"))
			return -1;
		sample->d2h_bytes += length;

		/* 2. reference I/O 与纯 CPU 比较分开计时。 */
		start = clock_ns(run);
		if (fread(run->reference_buffer, 1, length, reference->stream) != length)
			return eval_error(run, "读取 reference 失败或文件长度改变");
		sample->reference_read_ns += elapsed_ns(run, start);
		start = clock_ns(run);
		for (size_t i = 0; i < length; i += width) {
			if (run->job.type == EVAL_F32) {
				compare_floating(run, &sample->metrics, run->buffer + i,
						 run->reference_buffer + i);
			} else {
				int64_t actual = integer_value(run->buffer + i);
				int64_t expected = integer_value(run->reference_buffer + i);

				record_difference(&sample->metrics, actual, expected,
						  actual == expected);
			}
		}
		sample->compare_ns += elapsed_ns(run, start);
		offset += length;
	}

	/* 3. 只在完整比较后标记 checked。 */
	if (fgetc(reference->stream) != EOF || ferror(reference->stream))
		return eval_error(run, "reference 在比较期间改变长度");
	sample->checked = true;
	return 0;
}

/* 创建新 marker，拒绝悄悄复用上一次运行的同名文件。 */
static int write_marker(EvalRun *run, const char *path)
{
	FILE *stream = fopen(path, "wx");
	int result = 0;

	if (!stream)
		return eval_error(run, "创建文件 barrier 失败");
	if (fprintf(stream, "%ld\n", (long)getpid()) < 0)
		result = eval_error(run, "写入文件 barrier 失败");
	if (fclose(stream))
		result = eval_error(run, "发布文件 barrier 失败");
	return result;
}

/*
 * 等待由 Host 协调，timeout 只约束派发；
 * 另一进程的分配时间不受本进程的派发期限限制。
 */
static int wait_marker(EvalRun *run, const char *path)
{
	const struct timespec interval = { .tv_nsec = EVAL_BARRIER_POLL_NS };

	for (;;) {
		if (!access(path, F_OK))
			return 0;
		if (errno != ENOENT && errno != EINTR)
			return eval_error(run, "读取文件 barrier 失败");
		if (nanosleep(&interval, NULL) && errno != EINTR)
			return eval_error(run, "等待文件 barrier 失败");
	}
}

/* JSON 字符串保留 UTF-8，控制字符、引号和反斜线必须转义。 */
static void json_string(const char *text)
{
	if (!text) {
		fputs("null", stdout);
		return;
	}
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

/* JSON 不输出非标准 NaN/Infinity 字面量；额外布尔字段说明无穷指标。 */
static void json_number(long double value)
{
	if (isfinite(value))
		printf("%.17Lg", value);
	else
		fputs("null", stdout);
}

/* 全零 reference 的相对 L2 单独处理，不添加任意分母常量。 */
static long double relative_l2(const EvalMetrics *metrics)
{
	if (metrics->reference_squared)
		return sqrtl(metrics->error_squared / metrics->reference_squared);
	return metrics->error_squared ? INFINITY : 0;
}

/* 累加已完整检查的样本；非有限值不匹配仍保持失败计数。 */
static void merge_metrics(EvalMetrics *total, const EvalMetrics *sample)
{
	total->compared += sample->compared;
	total->failed += sample->failed;
	total->nonfinite_mismatches += sample->nonfinite_mismatches;
	if (sample->maxabs > total->maxabs)
		total->maxabs = sample->maxabs;
	if (sample->maxrel > total->maxrel)
		total->maxrel = sample->maxrel;
	total->error_squared += sample->error_squared;
	total->reference_squared += sample->reference_squared;
}

/* 不给样本数写死断言，未开始和中途失败的次数由状态明确区分。 */
static bool emit_report(EvalRun *run)
{
	EvalMetrics metrics = { 0 };
	uint64_t params = 0, params_bytes = 0, dispatch = 0, tiles = 0;
	uint64_t warmup_tiles = 0, d2h = 0, d2h_bytes = 0, compare = 0, reference_read = 0;
	uint64_t work_items = 0, warmup_work_items = 0;
	size_t completed = 0;
	bool first, success;

	for (size_t i = 0; i < run->sample_count; i++) {
		EvalSample *sample = &run->samples[i];

		if (sample->warmup) {
			warmup_tiles += sample->tile_dispatches;
			warmup_work_items += sample->work_items;
			continue;
		}
		work_items += sample->work_items;
		params += sample->params_ns;
		params_bytes += sample->params_bytes;
		dispatch += sample->dispatch_ns;
		tiles += sample->tile_dispatches;
		d2h += sample->d2h_ns;
		d2h_bytes += sample->d2h_bytes;
		compare += sample->compare_ns;
		reference_read += sample->reference_read_ns;
		merge_metrics(&metrics, &sample->metrics);
		completed += sample->complete && sample->checked;
	}
	success = !run->error && !metrics.failed && completed == run->options.repeats &&
		run->cleanup_ok && !run->stats_error && !run->clock_error;
	fputs("{\"case_id\":", stdout);
	json_string(run->job.case_id);
	printf(",\"pid\":%ld", (long)getpid());
	printf(",\"backend\":\"%s\",\"effective_backend\":\"%s\"",
	       run->options.backend == GPGPU_EVAL_BACKEND_BLOCK ? "block" : "scalar",
	       run->job.backend == GPGPU_EVAL_BACKEND_BLOCK ? "block" : "scalar");
	printf(",\"work_items\":%" PRIu64 ",\"warmup_work_items\":%" PRIu64,
	       work_items, warmup_work_items);
	printf(",\"op\":%" PRIu64 ",\"count\":%" PRIu64 ",\"output_dtype\":",
	       run->job.op, run->job.count);
	json_string(run->job.type == EVAL_I64 ? "int64" : "float32");
	printf(",\"uncached\":%s,\"staging_bytes\":%" PRIu64,
	       run->options.uncached ? "true" : "false", (uint64_t)EVAL_STAGING_BYTES);
	printf(",\"comparison_policy\":\"%s\"", run->job.type != EVAL_F32 ? "exact_integer" :
	       exact_compare_enabled(&run->job) ? "exact_bits" : "atol_rtol");
	printf(",\"tile\":%" PRIu64 ",\"tile_requested\":%" PRIu64,
	       run->tile, run->options.tile);
	printf(",\"repeats\":%zu,\"warmups\":%zu,\"completed_repeats\":%zu",
	       run->options.repeats, run->options.warmups, completed);
	printf(",\"tile_dispatches\":%" PRIu64 ",\"warmup_tile_dispatches\":%" PRIu64,
	       tiles, warmup_tiles);
	fputs(",\"operator_ns\":[", stdout);
	first = true;
	for (size_t i = 0; i < run->sample_count; i++) {
		if (run->samples[i].warmup)
			continue;
		printf("%s%" PRIu64, first ? "" : ",", run->samples[i].operator_ns);
		first = false;
	}
	fputs("],\"warmup_ns\":[", stdout);
	first = true;
	for (size_t i = 0; i < run->sample_count; i++) {
		if (!run->samples[i].warmup)
			continue;
		printf("%s%" PRIu64, first ? "" : ",", run->samples[i].operator_ns);
		first = false;
	}
	printf("],\"alloc_clear_ns\":%" PRIu64 ",\"runtime_setup_ns\":%" PRIu64
	       ",\"kernel_load_ns\":%" PRIu64 ",\"input_read_ns\":%" PRIu64
	       ",\"h2d_ns\":%" PRIu64 ",\"h2d_bytes\":%" PRIu64
	       ",\"queue_prepare_ns\":%" PRIu64 ",\"params_ns\":%" PRIu64
	       ",\"params_bytes\":%" PRIu64 ",\"dispatch_ns\":%" PRIu64
	       ",\"d2h_ns\":%" PRIu64 ",\"d2h_bytes\":%" PRIu64
	       ",\"reference_read_ns\":%" PRIu64 ",\"compare_ns\":%" PRIu64,
	       run->alloc_clear_ns, run->runtime_setup_ns, run->kernel_load_ns,
	       run->input_read_ns, run->h2d_ns, run->h2d_bytes, run->queue_prepare_ns,
	       params, params_bytes, dispatch, d2h, d2h_bytes, reference_read, compare);
	printf(",\"h2d_chunks\":%" PRIu64 ",\"buffer_setup_ns\":%" PRIu64,
	       run->h2d_chunks, run->buffer_setup_ns);
	printf(",\"atol\":%.17g,\"rtol\":%.17g,\"compared_values\":%" PRIu64
	       ",\"failed_values\":%" PRIu64 ",\"nonfinite_mismatches\":%" PRIu64,
	       run->options.atol, run->options.rtol, metrics.compared, metrics.failed,
	       metrics.nonfinite_mismatches);
	fputs(",\"maxabs\":", stdout);
	json_number(metrics.maxabs);
	fputs(",\"maxrel\":", stdout);
	json_number(metrics.maxrel);
	fputs(",\"relative_l2\":", stdout);
	json_number(relative_l2(&metrics));
	printf(",\"maxrel_infinite\":%s,\"relative_l2_infinite\":%s",
	       isinf(metrics.maxrel) ? "true" : "false",
	       isinf(relative_l2(&metrics)) ? "true" : "false");
	printf(",\"requested_bytes\":%" PRIu64 ",\"mapped_bytes\":%" PRIu64
	       ",\"rss_max_kib\":%ld,\"bos\":[",
	       run->requested_bytes, run->mapped_bytes, run->rss_max_kib);
	for (size_t i = 0; i < run->bo_count; i++) {
		EvalBoRecord *bo = &run->bos[i];

		printf("%s{\"role\":", i ? "," : "");
		json_string(bo->role);
		printf(",\"kind\":%u,\"requested_bytes\":%" PRIu64
		       ",\"mapped_bytes\":%" PRIu64 "}", bo->kind, bo->requested, bo->mapped);
	}
	fputs("],\"samples\":[", stdout);
	for (size_t i = 0; i < run->sample_count; i++) {
		EvalSample *sample = &run->samples[i];

		printf("%s{\"warmup\":%s,\"complete\":%s,\"checked\":%s",
		       i ? "," : "", sample->warmup ? "true" : "false",
		       sample->complete ? "true" : "false", sample->checked ? "true" : "false");
		printf(",\"operator_ns\":%" PRIu64 ",\"params_ns\":%" PRIu64
		       ",\"dispatch_ns\":%" PRIu64 ",\"tile_dispatches\":%" PRIu64
		       ",\"work_items\":%" PRIu64,
		       sample->operator_ns, sample->params_ns, sample->dispatch_ns,
		       sample->tile_dispatches, sample->work_items);
		printf(",\"d2h_ns\":%" PRIu64 ",\"d2h_bytes\":%" PRIu64
		       ",\"compare_ns\":%" PRIu64 ",\"failed_values\":%" PRIu64 "}",
		       sample->d2h_ns, sample->d2h_bytes, sample->compare_ns,
		       sample->metrics.failed);
	}
	fputs("],\"error\":", stdout);
	json_string(run->error);
	printf(",\"kmt_status\":%u,\"retained\":%s,\"success\":%s}\n",
	       run->kmt_status, run->retained ? "true" : "false", success ? "true" : "false");
	if (fflush(stdout))
		return false;
	return success;
}

/* 文件句柄与路径只在 JSON 输出后释放，BO 记录中的 role 仍需引用路径。 */
static void free_host_memory(EvalRun *run)
{
	for (unsigned int i = 0; i < EVAL_INPUTS; i++) {
		if (run->job.inputs[i].stream)
			fclose(run->job.inputs[i].stream);
		free(run->job.inputs[i].path);
	}
	if (run->job.reference.stream)
		fclose(run->job.reference.stream);
	free(run->job.reference.path);
	free(run->job.case_id);
	free(run->buffer);
	free(run->reference_buffer);
	free(run->samples);
	free(run->bos);
}

/* 只归还已确认不再被队列使用的额外 KMT 对象。 */
static int free_uncached_buffer(EvalRun *run, GpgpuKmtMemory **memory)
{
	if (!*memory)
		return 0;
	if (kmt_result(run, gpgpuKmtFreeMemory(*memory), "释放 UNCACHED 对象失败"))
		return -1;
	*memory = NULL;
	return 0;
}

/*
 * release_device() - 在两个进程均完成之后按寿命依赖回收
 *
 * 1. 完成未知时不再发任何回收请求；
 * 2. RT 先排空、卸载队列，释放其登记对象；
 * 3. 再释放额外 UNCACHED 数据对象，最后关闭 KMT。
 *
 * 任一步失败即保留剩余对象，CLI 不能报告成功。
 */
static void release_device(EvalRun *run)
{
	/* 1. 不能把超时解释为设备已经停止访问。 */
	if (run->uncertain) {
		run->retained = true;
		return;
	}
	/* 2. 队列及 RT 对象的既有收尾顺序保持不变。 */
	if (run->rt) {
		if (kmt_result(run, gpgpu_rt_destroy(run->rt), "回收 RT 失败，保留资源")) {
			run->retained = true;
			return;
		}
		run->rt = NULL;
	}

	/* 3. 队列已卸载，才释放未登记在 RT 中的评估对象。 */
	if (run->options.uncached) {
		for (unsigned int i = EVAL_SCRATCHES; i; i--)
			if (free_uncached_buffer(run, &run->scratch[i - 1]))
				goto retain;
		if (free_uncached_buffer(run, &run->command) ||
		    free_uncached_buffer(run, &run->output))
			goto retain;
		for (unsigned int i = EVAL_INPUTS; i; i--)
			if (free_uncached_buffer(run, &run->inputs[i - 1]))
				goto retain;
	}
	if (run->context && kmt_result(run, gpgpuKmtClose(run->context),
				       "关闭 KMT 失败，资源继续保留")) {
		run->retained = true;
		return;
	}
	run->context = NULL;
	run->cleanup_ok = true;
	return;

retain:
	run->retained = true;
}

/*
 * main() - 准备、同步启动、重复真实算子并完整比较
 *
 * 1. 校验文件与布局，准备固定小缓冲和所有设备对象；
 * 2. COUNT=0 预建队列，然后发布 ready，等待 start；
 * 3. 预热与测量都保存完整算子耗时，测量样本逐次完整比较；
 * 4. 发布 done 并等待 release，最后才卸载队列和回收对象。
 *
 * 失败也沿同一顺序收尾，未知完成时不复用或释放设备资源。
 *
 * Return: 只有全部测量比较和正常收尾成功才返回 EXIT_SUCCESS。
 */
int main(int argc, char **argv)
{
	EvalRun run = { 0 };
	struct rusage usage_info;
	size_t sample_capacity;
	int options_result;
	bool success;

	options_result = parse_options(argc, argv, &run.options);
	if (options_result) {
		if (options_result < 0)
			usage(stderr, argv[0]);
		return options_result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	eval_observer = &run;

	/* 1. 所有数组先按真实文件长度检查，不按文件名猜 shape。 */
	if (load_job(&run))
		goto out;
	if (!layout_valid(&run.job)) {
		eval_error(&run, "算子、dtype、维度或文件长度不符合契约");
		goto out;
	}
	if (select_backend(&run) || select_tile(&run))
		goto out;
	sample_capacity = run.options.warmups + run.options.repeats;
	if (sample_capacity > SIZE_MAX / sizeof(*run.samples)) {
		eval_error(&run, "样本数组长度溢出");
		goto out;
	}
	run.samples = calloc(sample_capacity, sizeof(*run.samples));
	run.buffer = malloc(EVAL_STAGING_BYTES);
	run.reference_buffer = malloc(EVAL_STAGING_BYTES);
	if (!run.samples || !run.buffer || !run.reference_buffer) {
		eval_error(&run, "分配固定读缓冲或样本数组失败");
		goto out;
	}
	if (prepare_memory(&run))
		goto out;

	/* 2. ready 之前队列和数组均已准备好，空派发不算真实吞吐。 */
	if (prepare_queue(&run))
		goto out;
	if (run.options.ready) {
		if (write_marker(&run, run.options.ready))
			goto out;
		run.ready = true;
		if (wait_marker(&run, run.options.start))
			goto out;
	}

	/* 3. 不在失败后改变 shape、tile 或数值容差。 */
	for (size_t i = 0; i < sample_capacity; i++) {
		EvalSample *sample = &run.samples[run.sample_count++];

		sample->warmup = i < run.options.warmups;
		if (run_operator(&run, sample))
			break;
		if (!sample->warmup && compare_output(&run, sample))
			break;
	}
out:
	/* 4. barrier 放行前不卸载队列，避免被另一个长派发阻塞。 */
	if (run.ready) {
		write_marker(&run, run.options.done);
		wait_marker(&run, run.options.release);
	}
	if (run.stats_error)
		eval_error(&run, "BO 统计未能完整保存");
	if (run.clock_error)
		eval_error(&run, "单调时钟读取失败，耗时无效");
	release_device(&run);
	if (getrusage(RUSAGE_SELF, &usage_info))
		eval_error(&run, "读取 RSS 统计失败");
	else
		run.rss_max_kib = usage_info.ru_maxrss;
	success = emit_report(&run);
	eval_observer = NULL;
	free_host_memory(&run);
	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
