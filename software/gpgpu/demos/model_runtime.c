// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 模型运行时：共用的常驻 BO、设备算子程序与连续生成
 *
 * 只读取模型包中的权重、真实输入和命令，不读取 CPU 参考：
 *   文件地址 -> BO + 子区间 -> 共用算子执行器 -> RT/AQL/CP/RV64
 *   token 轮回读 logits 并在 CPU argmax，再写下一轮 token 输入。
 * 本文件不在 CPU 上计算网络前向；检测后处理由调用者完成。
 */
#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "model_ops.h"
#include "model_program.h"
#include "model_runtime.h"
#include "model_eval.h"

/* 文件装载、回读及 RT 暂存的固定字节数。 */
#define MODEL_IO_BYTES (64u * 1024u)
/* 单调时钟每秒的纳秒数。 */
#define MODEL_NS_PER_SECOND UINT64_C(1000000000)
/* 宿主请求屏障的检查间隔，单位纳秒。 */
#define MODEL_CONTROL_POLL_NS 10000000L
/* 命令中需要重定位的六输入、输出与三个 scratch。 */
#define MODEL_REFERENCES (GPGPU_MODEL_INPUTS + 1 + GPGPU_MODEL_SCRATCHES)

/**
 * struct ModelCursor - 已映射文件的剩余只读范围
 * @data: 下一个待解析字节。
 * @left: 尚未读取的字节数。
 */
typedef struct ModelCursor {
	const uint8_t *data;
	size_t left;
} ModelCursor;

/**
 * struct ModelRange - 文件编码的设备子区间
 * @address: 高字节选择 BO，低位为 BO 内偏移。
 * @bytes: 该区间可访问的字节数。
 * @name: 相对数据文件名或输出名称；节点引用不使用此成员。
 */
typedef struct ModelRange {
	uint64_t address;
	uint64_t bytes;
	char *name;
} ModelRange;

/**
 * struct ModelNode - 一个完整基本或复合算子
 * @words: 本机字节序命令，装载 BO 后只重定位地址槽。
 * @spans: 六输入、输出和三 scratch 的可访问跨度，字节。
 * @token_address: embedding 输入的文件编码地址，重定位后仍用于实际 token 值域检查。
 * @rows: 复合归约行数。
 * @work_count: 复合中间结果的元素数。
 * @name: 用于层与算子归属的 ASCII 名称。
 */
typedef struct ModelNode {
	uint64_t words[GPGPU_MODEL_WORDS];
	uint64_t spans[MODEL_REFERENCES];
	uint64_t token_address;
	uint64_t rows;
	uint64_t work_count;
	char *name;
} ModelNode;

/**
 * struct ModelRound - 一次 prefill 块、decode 或检测前向
 * @kind: GPGPU_MODEL_ROUND_*。
 * @count: 节点数。
 * @output: logits 或最终检测张量。
 * @elements: output 的 FP32 元素数。
 * @token_slot: 本轮所选 token 的下一轮输入位置，末轮可为零。
 * @checks: 仅诊断运行回读的中间检查输出。
 * @check_count: checks 的条数。
 * @name: 轮名称。
 * @nodes: 按顺序同步执行的节点。
 */
typedef struct ModelRound {
	uint64_t kind;
	size_t count;
	uint64_t output;
	uint64_t elements;
	uint64_t token_slot;
	ModelRange *checks;
	size_t check_count;
	char *name;
	ModelNode *nodes;
} ModelRound;

/**
 * struct ModelProgram - 已核对结构的完整模型程序
 * @model: Qwen 或 YOLO 标识。
 * @sizes: 各类 BO 的请求字节数；零长度类别不分配。
 * @loads: 装载到 BO 的权重及输入文件。
 * @load_count: loads 条数。
 * @rounds: 所有已准备前向轮。
 * @round_count: rounds 条数。
 * @eos: 允许停止生成的 token ID。
 * @eos_count: eos 条数。
 * @vocabulary: Qwen 词表大小，YOLO 为零。
 * @prompt_tokens: 完整 prompt token 数，YOLO 为零。
 */
typedef struct ModelProgram {
	uint64_t model;
	uint64_t sizes[GPGPU_MODEL_ARENA_COUNT];
	ModelRange *loads;
	size_t load_count;
	ModelRound *rounds;
	size_t round_count;
	uint64_t *eos;
	size_t eos_count;
	uint64_t vocabulary;
	uint64_t prompt_tokens;
} ModelProgram;

/**
 * struct ModelRun - 一个串行应用会话
 * @program: 当前已重定位的程序；装载表与结果范围仍保留文件编码地址。
 * @weights: 初始程序的只读权重合同副本，不随请求更换。
 * @weight_count: weights 的条数。
 * @weight_bytes: 初始权重 arena 的逻辑字节数。
 * @context: 当前 KMT 连接。
 * @rt: 当前小运行时。
 * @kernel: 已装载的手写 Kernel。
 * @arenas: 五类常驻数据 BO。
 * @command: 串行复用的命令 BO。
 * @executor: 借用上述对象的算子执行核心。
 * @directory: 当前程序所在的绝对目录，只为请求 INPUT 提供路径根。
 * @weights_directory: 初始程序所在的固定绝对目录，只为 WEIGHTS 提供路径根。
 * @output: 当前请求借用的结果目录，调用返回前清空。
 * @log: 当前请求的遥测流；CLI 借用 stdout，API 只借用本次 model.log。
 * @setup_record: API 会话的初始准备日志快照，避免后续累计计数混入准备成本。
 * @control: 可选宿主请求屏障目录；不提供时自动顺序执行。
 * @control_timeout_ms: 独立协调期限，毫秒；零表示由宿主超时和 abort 控制。
 * @buffer: 小型流式 I/O 缓冲。
 * @setup_ns: 连接、分配、Kernel 与权重装载总耗时。
 * @allocation_ns: BO 分配与映射的累计耗时。
 * @file_read_ns: 权重及输入文件读取耗时。
 * @h2d_ns: 权重及已执行请求输入的上传累计耗时。
 * @h2d_bytes: 权重及已执行请求输入的上传累计字节数。
 * @program_read_ns: 程序解析耗时。
 * @clock_error: 单调时钟失败，不能报告成功计时。
 * @failed: 设备访问或程序执行失败，禁止继续运行请求。
 * @checkpoints: 是否回读中间结果；仅诊断使用。
 */
typedef struct ModelRun {
	ModelProgram program;
	ModelRange *weights;
	size_t weight_count;
	uint64_t weight_bytes;
	GpgpuKmtContext *context;
	GpgpuRt *rt;
	GpgpuRtKernel kernel;
	GpgpuKmtMemory *arenas[GPGPU_MODEL_ARENA_COUNT];
	GpgpuKmtMemory *command;
	GpgpuModelExecutor executor;
	char *directory;
	char *weights_directory;
	const char *output;
	FILE *log;
	char *setup_record;
	const char *control;
	uint64_t control_timeout_ms;
	uint8_t buffer[MODEL_IO_BYTES];
	uint64_t setup_ns;
	uint64_t allocation_ns;
	uint64_t file_read_ns;
	uint64_t h2d_ns;
	uint64_t h2d_bytes;
	uint64_t program_read_ns;
	bool clock_error;
	bool failed;
	bool checkpoints;
} ModelRun;

/**
 * struct ModelRoundStats - 完整前向轮及必要 CPU 控制耗时
 * @nodes: 本轮逐节点统计，前向结束后才输出，避免日志混入计算。
 * @forward_ns: 全部节点的墙钟耗时，纳秒。
 * @output_ns: 必要输出回读及 argmax 耗时，纳秒。
 * @token_upload_ns: 给下一轮写入所选 token 的耗时，纳秒。
 * @checks_ns: 可选检查输出的回读与文件写入耗时，纳秒。
 * @ready_ns: 从请求开始到本轮 token/检测结果可用的耗时，纳秒。
 * @token: 实际选出的 token，检测或纯前缀轮为零。
 * @complete: 本轮是否完整成功。
 */
typedef struct ModelRoundStats {
	GpgpuModelOpStats *nodes;
	uint64_t forward_ns;
	uint64_t output_ns;
	uint64_t token_upload_ns;
	uint64_t checks_ns;
	uint64_t ready_ns;
	uint64_t token;
	bool complete;
} ModelRoundStats;

/* 时钟错误由最终状态统一拒绝，不能伪造成功耗时。 */
static uint64_t model_clock(ModelRun *run)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value)) {
		run->clock_error = true;
		return 0;
	}
	return (uint64_t)value.tv_sec * MODEL_NS_PER_SECOND + value.tv_nsec;
}

/* 只使用同一客体单调时钟计算区间。 */
static uint64_t model_elapsed(ModelRun *run, uint64_t begin)
{
	uint64_t end = model_clock(run);

	if (end < begin) {
		run->clock_error = true;
		return 0;
	}
	return end - begin;
}

/* 临时标记文件关闭后才 rename，宿主不会读到半条记录。 */
static bool control_publish(const ModelRun *run, const char *name)
{
	char *path = NULL, *temporary = NULL;
	FILE *stream;
	bool valid = false;

	if (!run->control)
		return true;
	if (asprintf(&path, "%s/%s", run->control, name) < 0 ||
	    asprintf(&temporary, "%s.tmp", path) < 0)
		goto out;
	stream = fopen(temporary, "wx");
	if (!stream)
		goto out;
	valid = fputs("0\n", stream) >= 0;
	if (fclose(stream))
		valid = false;
	if (valid && rename(temporary, path))
		valid = false;

out:
	free(path);
	free(temporary);
	return valid;
}

/* 屏障只在设备空闲时等待；宿主失败发布 abort 后正常走资源收尾。 */
static bool control_wait(ModelRun *run, const char *name)
{
	char *path = NULL, *abort_path = NULL;
	uint64_t start = model_clock(run);
	uint64_t limit = run->control_timeout_ms * (MODEL_NS_PER_SECOND / 1000);
	bool valid = false;

	if (!run->control)
		return true;
	if (asprintf(&path, "%s/%s", run->control, name) < 0 ||
	    asprintf(&abort_path, "%s/abort", run->control) < 0)
		goto out;
	while ((!run->control_timeout_ms || model_elapsed(run, start) < limit) &&
	       !run->clock_error) {
		struct timespec interval = { .tv_nsec = MODEL_CONTROL_POLL_NS };

		if (!access(abort_path, F_OK))
			goto out;
		if (!access(path, F_OK)) {
			valid = true;
			goto out;
		}
		nanosleep(&interval, NULL);
	}
	fprintf(stderr, "宿主请求屏障超时：%s\n", name);

out:
	free(path);
	free(abort_path);
	return valid;
}

/* 失败后停止整个请求，不尝试复用部分更新的 KV。 */
static bool model_status_ok(ModelRun *run, GpgpuKmtStatus status, const char *action)
{
	if (status == GPGPU_KMT_SUCCESS)
		return true;
	fprintf(stderr, "%s：KMT=%u\n", action, status);
	run->failed = true;
	return false;
}

/* 从完整剩余区间读一个小端 u64，不直接解引用未对齐指针。 */
static bool cursor_u64(ModelCursor *cursor, uint64_t *value)
{
	uint64_t wire;

	if (cursor->left < sizeof(wire))
		return false;
	memcpy(&wire, cursor->data, sizeof(wire));
	*value = le64toh(wire);
	cursor->data += sizeof(wire);
	cursor->left -= sizeof(wire);
	return true;
}

/* 名称不含控制字符；数据文件路径另做受控子目录检查。 */
static bool cursor_name(ModelCursor *cursor, char **result)
{
	uint64_t length;

	if (!cursor_u64(cursor, &length) || !length || length > cursor->left || length > PATH_MAX)
		return false;
	for (uint64_t i = 0; i < length; i++)
		if (!cursor->data[i] ||
		    (!isalnum(cursor->data[i]) && !strchr("_.-/", cursor->data[i])))
			return false;
	*result = strndup((const char *)cursor->data, length);
	if (!*result)
		return false;
	cursor->data += length;
	cursor->left -= length;
	return true;
}

/* 文件计数必须能由剩余 u64 字段和 size_t 分配表示。 */
static bool cursor_array(ModelCursor *cursor, uint64_t count, size_t size, void **result)
{
	if (count > cursor->left / sizeof(uint64_t) || count > SIZE_MAX / size)
		return false;
	*result = calloc(count ? count : 1, size);
	return *result != NULL;
}

/* 解析失败也只释放 CPU 侧描述，不涉及任何设备资源。 */
static void program_free(ModelProgram *program)
{
	for (size_t i = 0; i < program->load_count; i++)
		free(program->loads[i].name);
	for (size_t r = 0; r < program->round_count; r++) {
		ModelRound *round = &program->rounds[r];

		for (size_t i = 0; i < round->check_count; i++)
			free(round->checks[i].name);
		for (size_t i = 0; i < round->count; i++)
			free(round->nodes[i].name);
		free(round->checks);
		free(round->nodes);
		free(round->name);
	}
	free(program->loads);
	free(program->rounds);
	free(program->eos);
	memset(program, 0, sizeof(*program));
}

/* 按真实 COUNT/dtype/stride 推导访问量，不把文件自报跨度当成算子边界。 */
static bool node_layout_valid(const ModelNode *node, const ModelProgram *program)
{
	GpgpuModelLayout layout;

	if (!gpgpu_model_layout_valid(node->words, node->spans, &layout) ||
	    node->spans[GPGPU_MODEL_INPUTS] < layout.output_bytes ||
	    node->rows != layout.rows || node->work_count != layout.work_count)
		return false;
	for (unsigned int i = 0; i < GPGPU_MODEL_SCRATCHES; i++)
		if (node->spans[GPGPU_MODEL_INPUTS + 1 + i] < layout.scratch_bytes[i])
			return false;
	return !layout.vocabulary || program->model != GPGPU_MODEL_QWEN ||
	       layout.vocabulary == program->vocabulary;
}

/*
 * program_parse() - 解析完整模型程序，但不建立任何设备对象
 *
 * program_load() 在只读文件映射存活时调用。先复制全部描述，再解除映射，
 * 防止前向运行时依赖可变化的文件页或悬空名称。
 *
 * 1. 核对格式、模型标识和计数，读取 BO 请求长度及 EOS；
 * 2. 保存受控 data/ 子目录中的权重和输入装载表；
 * 3. 保存逐轮检查输出和完整节点；拒绝缺少字段或尾部多余数据。
 *
 * 失败不局部回滚，program_free() 统一释放已分配的 CPU 描述。
 * 地址范围由 program_valid() 在建立或替换 BO 前核对。
 *
 * Return: 整个文件结构有效时为 true。
 */
static bool program_parse(ModelCursor *cursor, ModelProgram *program)
{
	uint64_t arenas, rounds, loads, eos;

	/* 1. 所有计数都先受剩余文件和 size_t 可表示范围约束。 */
	if (cursor->left < sizeof(GPGPU_MODEL_PROGRAM_MAGIC) - 1 ||
	    memcmp(cursor->data, GPGPU_MODEL_PROGRAM_MAGIC, sizeof(GPGPU_MODEL_PROGRAM_MAGIC) - 1))
		return false;
	cursor->data += sizeof(GPGPU_MODEL_PROGRAM_MAGIC) - 1;
	cursor->left -= sizeof(GPGPU_MODEL_PROGRAM_MAGIC) - 1;
	if (!cursor_u64(cursor, &program->model) ||
	    (program->model != GPGPU_MODEL_QWEN && program->model != GPGPU_MODEL_YOLO) ||
	    !cursor_u64(cursor, &arenas) || arenas != GPGPU_MODEL_ARENA_COUNT ||
	    !cursor_u64(cursor, &rounds) || !rounds ||
	    !cursor_u64(cursor, &loads) || !loads || !cursor_u64(cursor, &eos) ||
	    !cursor_u64(cursor, &program->vocabulary) ||
	    !cursor_u64(cursor, &program->prompt_tokens))
		return false;
	for (unsigned int i = 0; i < GPGPU_MODEL_ARENA_COUNT; i++)
		if (!cursor_u64(cursor, &program->sizes[i]) || program->sizes[i] > SIZE_MAX)
			return false;
	if (!cursor_array(cursor, eos, sizeof(*program->eos), (void **)&program->eos))
		return false;
	program->eos_count = eos;
	for (size_t i = 0; i < program->eos_count; i++)
		if (!cursor_u64(cursor, &program->eos[i]) || program->eos[i] >= program->vocabulary)
			return false;
	/* 2. 不允许装载路径离开运行包，也不从参考目录取中间激活。 */
	if (!cursor_array(cursor, loads, sizeof(*program->loads), (void **)&program->loads))
		return false;
	program->load_count = loads;
	for (size_t i = 0; i < program->load_count; i++) {
		ModelRange *range = &program->loads[i];

		if (!cursor_u64(cursor, &range->address) || !cursor_u64(cursor, &range->bytes) ||
		    !range->bytes || !cursor_name(cursor, &range->name) ||
		    strncmp(range->name, "data/", strlen("data/")) || strstr(range->name, "..") ||
		    strchr(range->name + strlen("data/"), '/'))
			return false;
	}
	/* 3. 记录全部算子，执行阶段不再临时读取图文件。 */
	if (!cursor_array(cursor, rounds, sizeof(*program->rounds), (void **)&program->rounds))
		return false;
	program->round_count = rounds;
	for (size_t r = 0; r < program->round_count; r++) {
		ModelRound *round = &program->rounds[r];
		uint64_t nodes, checks;

		if (!cursor_u64(cursor, &round->kind) || round->kind > GPGPU_MODEL_ROUND_DETECT ||
		    !cursor_u64(cursor, &nodes) || !nodes ||
		    !cursor_u64(cursor, &round->output) || !cursor_u64(cursor, &round->elements) ||
		    round->elements > SIZE_MAX / sizeof(float) ||
		    !cursor_u64(cursor, &round->token_slot) || !cursor_u64(cursor, &checks) ||
		    !cursor_name(cursor, &round->name) ||
		    !cursor_array(cursor, checks, sizeof(*round->checks), (void **)&round->checks))
			return false;
		round->check_count = checks;
		for (size_t i = 0; i < round->check_count; i++) {
			ModelRange *range = &round->checks[i];

			if (!cursor_u64(cursor, &range->address) ||
			    !cursor_u64(cursor, &range->bytes) || !range->bytes ||
			    !cursor_name(cursor, &range->name) || strchr(range->name, '/'))
				return false;
		}
		if (!cursor_array(cursor, nodes, sizeof(*round->nodes), (void **)&round->nodes))
			return false;
		round->count = nodes;
		for (size_t i = 0; i < round->count; i++) {
			ModelNode *node = &round->nodes[i];

			if (!cursor_u64(cursor, &node->rows) ||
			    !cursor_u64(cursor, &node->work_count) ||
			    !cursor_name(cursor, &node->name))
				return false;
			for (unsigned int w = 0; w < GPGPU_MODEL_WORDS; w++)
				if (!cursor_u64(cursor, &node->words[w]))
					return false;
			for (unsigned int w = 0; w < MODEL_REFERENCES; w++)
				if (!cursor_u64(cursor, &node->spans[w]))
					return false;
			if (!node_layout_valid(node, program)) {
				fprintf(stderr, "模型节点范围或几何不合法：%s\n",
					node->name);
				return false;
			}
		}
	}
	return !cursor->left;
}

/* 地址槽顺序与文件中的六输入、输出、三 scratch 跨度一一对应。 */
static unsigned int reference_slot(unsigned int index)
{
	if (index < GPGPU_MODEL_INPUTS)
		return GPGPU_EVAL_IN0 / GPGPU_EVAL_SLOT_BYTES + index;
	if (index == GPGPU_MODEL_INPUTS)
		return GPGPU_EVAL_OUT / GPGPU_EVAL_SLOT_BYTES;
	return GPGPU_EVAL_AUX0 / GPGPU_EVAL_SLOT_BYTES + index - GPGPU_MODEL_INPUTS - 1;
}

/*
 * program_range_valid() - 解码文件地址并核对本次程序的逻辑范围
 *
 * 解析、换图和运行时访问共用此检查，不借用旧图或较大 BO 的容量：
 *
 *   文件地址位  63..56          55..0
 *   含义        BO 类别加一     BO 内字节偏移
 *
 * 例：0x0200_0000_0000_0040 选择 INPUT，偏移 0x40；长度 8 要求 sizes[1] >= 0x48。
 *
 * 1. 拒绝零类别、未知类别和零长度；
 * 2. 用减法核对完整逻辑范围，避免 offset + bytes 回绕。
 *
 * 本函数不检查物理 BO；绑定时由调用者另核对容量。
 *
 * Return: 编码范围完整落在本次逻辑 arena 内时为 true。
 */
static bool program_range_valid(const ModelProgram *program, uint64_t address, uint64_t bytes,
				unsigned int *arena, uint64_t *offset)
{
	uint64_t id = address >> GPGPU_MODEL_ADDRESS_SHIFT;
	uint64_t low = address & (UINT64_MAX >> (64 - GPGPU_MODEL_ADDRESS_SHIFT));

	/* 1. 零地址不表示实际可访问区间。 */
	if (!id || id > GPGPU_MODEL_ARENA_COUNT || !bytes)
		return false;

	/* 2. 逻辑长度限制独立于常驻 BO 的已有容量。 */
	*arena = id - 1;
	*offset = low;
	return bytes <= program->sizes[*arena] && low <= program->sizes[*arena] - bytes;
}

/* 编码地址还必须绑定到容量足够的现有 BO。 */
static bool range_valid(const ModelRun *run, uint64_t address, uint64_t bytes,
			unsigned int *arena, uint64_t *offset)
{
	return program_range_valid(&run->program, address, bytes, arena, offset) &&
		run->arenas[*arena] && run->arenas[*arena]->size >= run->program.sizes[*arena];
}

/*
 * load_ranges_valid() - 核对文件装载区间的角色和互斥范围
 *
 * program_valid() 在设备分配前调用，防止其他 INPUT 文件覆盖借用输入。
 *
 * 1. 每项必须完整落在 WEIGHTS 或 INPUT 的逻辑范围内；
 * 2. 同类 BO 内按两起点之差判断重叠，拒绝重复或部分覆盖。
 *
 * 本函数不读取数据文件，也不修改装载表。
 *
 * Return: 全部装载区间合法且互不重叠时为 true。
 */
static bool load_ranges_valid(const ModelProgram *program)
{
	for (size_t i = 0; i < program->load_count; i++) {
		const ModelRange *range = &program->loads[i];
		unsigned int arena;
		uint64_t offset;

		/* 1. KV 和 scratch 不能用包内数据预填。 */
		if (!program_range_valid(program, range->address, range->bytes, &arena, &offset) ||
		    (arena != GPGPU_MODEL_ARENA_WEIGHTS && arena != GPGPU_MODEL_ARENA_INPUT))
			return false;

		/* 2. 不计算两个区间的末尾相加值，避免地址加法回绕。 */
		for (size_t j = 0; j < i; j++) {
			const ModelRange *other = &program->loads[j];

			if ((other->address >> GPGPU_MODEL_ADDRESS_SHIFT) != arena + 1)
				continue;
			if (range->address <= other->address) {
				if (other->address - range->address < range->bytes)
					return false;
			} else if (range->address - other->address < other->bytes) {
				return false;
			}
		}
	}
	return true;
}

/*
 * program_valid() - 在替换描述或设备存储前核对完整程序
 *
 * 初次准备和请求换图共用此检查，不要求已分配 BO。
 *
 * 1. 核对装载角色、逻辑范围及互不覆盖的输入；
 * 2. 核对所有轮输出、下一轮 token 槽和可选检查范围；
 * 3. 核对每个节点的全部引用、只读权重约束与派发几何。
 *
 * 此处不证明任意外部图的数学依赖；每请求完整重建 KV 仍由导出图保证。
 * 本函数不改描述、不分配或提交任何设备工作。
 *
 * Return: 完整逻辑描述可安全绑定时为 true。
 */
static bool program_valid(const ModelProgram *program)
{
	/* 1. 只允许从文件预装权重与输入，KV 必须来自设备前向。 */
	if (!load_ranges_valid(program))
		return false;
	for (size_t r = 0; r < program->round_count; r++) {
		const ModelRound *round = &program->rounds[r];
		unsigned int arena;
		uint64_t offset;

		/* 2. 非前缀轮必须产生完整结果，token 只能写入 INPUT。 */
		if (round->kind != GPGPU_MODEL_ROUND_PREFIX &&
		    (!round->elements ||
		     (round->kind == GPGPU_MODEL_ROUND_TOKEN &&
		      round->elements != program->vocabulary) ||
		     !program_range_valid(program, round->output, round->elements * sizeof(float),
					  &arena, &offset)))
			return false;
		if (round->token_slot &&
		    (!program_range_valid(program, round->token_slot, sizeof(uint64_t),
					  &arena, &offset) || arena != GPGPU_MODEL_ARENA_INPUT))
			return false;
		for (size_t i = 0; i < round->check_count; i++)
			if (!program_range_valid(program, round->checks[i].address,
						 round->checks[i].bytes, &arena, &offset))
				return false;
		for (size_t n = 0; n < round->count; n++) {
			const ModelNode *node = &round->nodes[n];
			uint64_t launch, backend;

			/* 3. 权重只作输入，不允许节点输出或 scratch 改写常驻权重。 */
			for (unsigned int i = 0; i < MODEL_REFERENCES; i++) {
				unsigned int slot = reference_slot(i);

				if (!node->words[slot] && !node->spans[i])
					continue;
				if (!program_range_valid(program, node->words[slot], node->spans[i],
							 &arena, &offset) ||
				    (i >= GPGPU_MODEL_INPUTS && arena == GPGPU_MODEL_ARENA_WEIGHTS))
					return false;
			}
			if (!gpgpu_model_launch_valid(node->words, GPGPU_EVAL_BACKEND_BLOCK,
						     &launch, &backend))
				return false;
		}
	}
	return true;
}

/* 映射仅用于解析和逻辑检查，返回前已复制所需描述，原文件不被修改。 */
static bool program_load(ModelRun *run, const char *path, ModelProgram *program)
{
	FILE *stream = fopen(path, "rb");
	struct stat status;
	void *mapping;
	ModelCursor cursor;
	bool valid;
	uint64_t start = model_clock(run);

	if (!stream)
		return false;
	if (fstat(fileno(stream), &status) || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
	    (uint64_t)status.st_size > SIZE_MAX) {
		fclose(stream);
		return false;
	}
	mapping = mmap(NULL, status.st_size, PROT_READ, MAP_PRIVATE, fileno(stream), 0);
	fclose(stream);
	if (mapping == MAP_FAILED)
		return false;
	cursor = (ModelCursor) { .data = mapping, .left = status.st_size };
	valid = program_parse(&cursor, program) && program_valid(program);
	munmap(mapping, status.st_size);
	run->program_read_ns = model_elapsed(run, start);
	return valid && !run->clock_error;
}

/*
 * program_relocate() - 将文件子区间换成真实的完整 64 位 GPU 地址
 *
 * 初次准备或请求换图在 BO 建立后调用，执行期间不修改代码或这些绑定：
 *
 *   文件地址位  63..56             55..0
 *   含义        BO 类别加一        类内字节偏移
 *
 * 例：0x0300_0000_0000_0040 选择激活 BO，地址为 arena[2].gpu_address + 0x40。
 *
 * 1. 依六输入、输出、三 scratch 的顺序核对完整跨度；
 * 2. 只替换地址槽，保留完整高位，不把维度或浮点位当成地址；
 * 3. 填唯一数学常量，检查操作及派发规模。
 *
 * 不提交计算，也不更改模型数据。失败由调用者停止准备并统一回收。
 *
 * Return: 所有节点均可由现有执行器派发时为 true。
 */
static bool program_relocate(ModelRun *run, ModelProgram *program)
{
	for (size_t r = 0; r < program->round_count; r++) {
		ModelRound *round = &program->rounds[r];

		for (size_t n = 0; n < round->count; n++) {
			ModelNode *node = &round->nodes[n];
			uint64_t launch, backend;

			/* 1. 保留编码范围，派发前才检查，不预检未来 decode 占位槽。 */
			if (node->words[GPGPU_EVAL_OP / GPGPU_EVAL_SLOT_BYTES] ==
			    GPGPU_EVAL_EMBEDDING_W8)
				node->token_address =
					node->words[GPGPU_EVAL_IN0 / GPGPU_EVAL_SLOT_BYTES];
			/* 跨度不跨 BO；缺省引用的地址、长度均为零。 */
			for (unsigned int i = 0; i < MODEL_REFERENCES; i++) {
				unsigned int slot = reference_slot(i), arena;
				uint64_t offset;

				if (!node->words[slot] && !node->spans[i])
					continue;
				if (!program_range_valid(program, node->words[slot], node->spans[i],
							 &arena, &offset) || !run->arenas[arena] ||
				    run->arenas[arena]->size < program->sizes[arena])
					return false;
				/* 2. 保留 GPUVA 高位，只有 BO 内偏移参与加法。 */
				node->words[slot] = run->arenas[arena]->gpu_address + offset;
			}
			/* 3. 不在调用者复制另一套近似常量或分块上限。 */
			gpgpu_model_constants(node->words);
			if (!gpgpu_model_launch_valid(node->words, GPGPU_EVAL_BACKEND_BLOCK,
						     &launch, &backend))
				return false;
		}
	}
	return true;
}

/* 分配统计不与装载统计相加，setup_ns 已包含它们。 */
static bool arena_allocate(ModelRun *run, uint64_t bytes, GpgpuKmtMemory **memory)
{
	uint64_t start = model_clock(run);
	GpgpuKmtStatus status = gpgpu_rt_buffer_alloc(run->rt, bytes, memory);

	run->allocation_ns += model_elapsed(run, start);
	return model_status_ok(run, status, "分配模型 BO 失败");
}

/*
 * load_range() - 把一个权重或输入子区间流式装入常驻 BO
 *
 * 准备阶段装权重，每次请求另装输入；固定窗口避免大权重占满客体 RAM。
 *
 * 1. 核对目的范围，经目录 fd 拒绝 data/ 与文件符号链接，再核对普通文件长度；
 * 2. 读取一小块，等待对应 upload_range 完成后才推进偏移并复用窗口；
 * 3. 确认文件没有追加或读取错误。
 *
 * 失败前已完成的上传不回退；设备结果未知时由调用者保留在途对象。
 * 本函数不计算、反量化或改写任何网络数值。
 *
 * Return: 文件全部读取且 COPY 均确认成功时为 true。
 */
static bool load_range(ModelRun *run, const ModelRange *range)
{
	FILE *stream = NULL;
	struct stat status;
	unsigned int arena;
	uint64_t offset;
	int root_fd = -1, data_fd = -1, file_fd = -1;
	bool valid = false;

	/* 1. 名称已限定为 data/叶子；实际打开也不跟随目录或叶子的符号链接。 */
	if (!range_valid(run, range->address, range->bytes, &arena, &offset))
		return false;
	root_fd = open(arena == GPGPU_MODEL_ARENA_WEIGHTS ? run->weights_directory : run->directory,
		       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root_fd < 0)
		goto out;
	data_fd = openat(root_fd, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (data_fd < 0)
		goto out;
	/* 非阻塞只为避免在 fstat 拒绝 FIFO 前被 open 卡住，普通文件不受影响。 */
	file_fd = openat(data_fd, range->name + strlen("data/"),
			 O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (file_fd < 0)
		goto out;
	stream = fdopen(file_fd, "rb");
	if (!stream)
		goto out;
	file_fd = -1;
	if (fstat(fileno(stream), &status) || !S_ISREG(status.st_mode) ||
	    status.st_size < 0 || (uint64_t)status.st_size != range->bytes)
		goto out;
	for (uint64_t done = 0; done < range->bytes;) {
		size_t size = range->bytes - done < sizeof(run->buffer) ?
			      range->bytes - done : sizeof(run->buffer);
		uint64_t start = model_clock(run);

		/* 2. 该块的设备 COPY 确认结束后，下一次 fread 才覆盖缓冲。 */
		if (fread(run->buffer, 1, size, stream) != size)
			goto out;
		run->file_read_ns += model_elapsed(run, start);
		start = model_clock(run);
		if (!model_status_ok(run, gpgpu_rt_upload_range(run->rt, run->arenas[arena],
							 offset + done, run->buffer, size),
				     "上传模型数据失败"))
			goto out;
		run->h2d_ns += model_elapsed(run, start);
		run->h2d_bytes += size;
		done += size;
	}
	/* 3. 长度被追加时也不能当作稳定材料装载成功。 */
	valid = fgetc(stream) == EOF && !ferror(stream);

out:
	if (stream && fclose(stream))
		valid = false;
	if (file_fd >= 0)
		close(file_fd);
	if (data_fd >= 0)
		close(data_fd);
	if (root_fd >= 0)
		close(root_fd);
	return valid;
}

/*
 * model_setup() - 建立一个模型会话的常驻资源
 *
 * 初次准备在完整解析文件后调用，后续请求共用权重、Kernel 与 RT。
 *
 * 1. 打开连接，建立原有 RT 并装载同一个 RV64 Kernel；
 * 2. 建立各类 BO 和命令区，按共享能力选择 tile；
 * 3. 仅装载权重并重定位节点；输入延后到每次请求，KV 只由设备产生。
 *
 * 不实现第二条提交或缓存维护路径。失败不局部释放，调用者统一调用
 * model_cleanup()，避免在设备访问尚未确认结束时提前复用存储。
 *
 * Return: 所有准备和计时均成功时为 true。
 */
static bool model_setup(ModelRun *run, uint64_t tile, uint32_t timeout)
{
	uint64_t start = model_clock(run);

	/* 1. 使用既有连接、装载和同步语义，不在应用重新敲 Doorbell。 */
	if (!model_status_ok(run, gpgpuKmtOpen(NULL, &run->context), "打开设备失败") ||
	    !model_status_ok(run, gpgpu_rt_create(run->context, MODEL_IO_BYTES, &run->rt),
			     "建立运行时失败") ||
	    !model_status_ok(run, gpgpu_rt_kernel_load(run->rt, model_eval_code,
						      sizeof(model_eval_code), GPGPU_MODEL_RT_ARGS,
						      &run->kernel), "装载 Kernel 失败"))
		return false;
	/* 2. 每类 BO 只有一份，不按张量重复登记 RT 对象。 */
	for (unsigned int i = 0; i < GPGPU_MODEL_ARENA_COUNT; i++)
		if (run->program.sizes[i] &&
		    !arena_allocate(run, run->program.sizes[i], &run->arenas[i]))
			return false;
	if (!arena_allocate(run, GPGPU_EVAL_PARAM_BYTES, &run->command))
		return false;
	run->executor = (GpgpuModelExecutor) {
		.rt = run->rt,
		.kernel = &run->kernel,
		.command = run->command,
		.timeout_ms = timeout,
	};
	if (!gpgpu_model_tile_valid(tile ? tile : GPGPU_MAX_TOTAL_THREADS, &run->executor.tile))
		return false;
	/* 3. 不允许通过运行包预填 CPU 计算的 KV。 */
	for (size_t i = 0; i < run->program.load_count; i++) {
		const ModelRange *range = &run->program.loads[i];
		uint64_t arena = range->address >> GPGPU_MODEL_ADDRESS_SHIFT;

		/* 输入每请求重新上传；KV 只允许由本次设备前向产生。 */
		if (arena == GPGPU_MODEL_ARENA_INPUT + 1)
			continue;
		if (arena != GPGPU_MODEL_ARENA_WEIGHTS + 1 || !load_range(run, range))
			return false;
	}
	if (!program_relocate(run, &run->program))
		return false;
	run->setup_ns = model_elapsed(run, start);
	return !run->clock_error;
}

/* 小端 FP32 解码；所有 logits 都须有限，不能靠 NaN 改变 argmax。 */
static bool logit_value(const uint8_t *bytes, float *value)
{
	uint32_t bits;

	memcpy(&bits, bytes, sizeof(bits));
	bits = le32toh(bits);
	memcpy(value, &bits, sizeof(*value));
	return isfinite(*value);
}

/* 完整回读一个子区间；诊断文件写入与必要 D2H 可分别计时。 */
static bool download_range(ModelRun *run, uint64_t address, uint64_t bytes, const char *path,
			   bool argmax, uint64_t *selected)
{
	unsigned int arena;
	uint64_t offset, best = 0;
	float maximum = -FLT_MAX;
	FILE *stream = path ? fopen(path, "wb") : NULL;
	bool valid = false;

	if ((path && !stream) || !range_valid(run, address, bytes, &arena, &offset))
		goto out;
	for (uint64_t done = 0; done < bytes;) {
		size_t size = bytes - done < sizeof(run->buffer) ?
			      bytes - done : sizeof(run->buffer);

		if (!model_status_ok(run, gpgpu_rt_download_range(run->rt, run->arenas[arena],
							   offset + done, run->buffer, size),
				     "回读模型结果失败"))
			goto out;
		if (stream && fwrite(run->buffer, 1, size, stream) != size)
			goto out;
		if (argmax)
			for (size_t i = 0; i < size; i += sizeof(float)) {
				float value;

				if (!logit_value(run->buffer + i, &value))
					goto out;
				if (value > maximum) {
					maximum = value;
					best = (done + i) / sizeof(float);
				}
			}
		done += size;
	}
	if (argmax)
		*selected = best;
	valid = true;

out:
	if (stream && fclose(stream))
		valid = false;
	return valid;
}

/* token 是 CPU 后处理结果，不是 CPU 前向参考；只写下一轮的输入槽。 */
static bool upload_token(ModelRun *run, uint64_t address, uint64_t token)
{
	unsigned int arena;
	uint64_t offset, wire = htole64(token);

	return range_valid(run, address, sizeof(wire), &arena, &offset) &&
	       arena == GPGPU_MODEL_ARENA_INPUT &&
	       model_status_ok(run, gpgpu_rt_upload_range(run->rt, run->arenas[arena], offset,
						     &wire, sizeof(wire)),
				       "写入 decode token 失败");
}

/* EOS 配置来自官方本地生成配置，而不是 C 内硬编码的 token 数值。 */
static bool token_eos(const ModelProgram *program, uint64_t token)
{
	for (size_t i = 0; i < program->eos_count; i++)
		if (program->eos[i] == token)
			return true;
	return false;
}

/* 可选中间回读只在诊断模式执行，正式计时不开启。 */
static bool round_checks(ModelRun *run, const ModelRound *round, size_t index,
			 const char *directory)
{
	for (size_t i = 0; i < round->check_count; i++) {
		const ModelRange *range = &round->checks[i];
		char *path;
		bool valid;

		if (asprintf(&path, "%s/round-%04zu.%s.bin", directory, index, range->name) < 0)
			return false;
		valid = download_range(run, range->address, range->bytes, path, false, NULL);
		free(path);
		if (!valid)
			return false;
	}
	return true;
}

/* 小结果与完整日志共用编码，必要后处理不解析历史节点日志。 */
static void sample_json(FILE *stream, const ModelRun *run, const ModelRoundStats *stats,
			size_t completed, size_t sample, bool warmup, uint64_t request_ns,
			uint64_t input_ns, uint64_t input_h2d_ns, uint64_t input_bytes,
			const char *stop)
{
	fprintf(stream, "{\"sample\":%zu,\"warmup\":%s,\"request_ns\":%" PRIu64
		",\"input_ns\":%" PRIu64 ",\"input_h2d_ns\":%" PRIu64
		",\"input_h2d_bytes\":%" PRIu64
		",\"rounds\":%zu,\"stop_reason\":\"%s\",\"tokens\":[",
		sample, warmup ? "true" : "false", request_ns, input_ns, input_h2d_ns,
		input_bytes, completed, stop);
	for (size_t r = 0, count = 0; r < completed; r++)
		if (run->program.rounds[r].kind == GPGPU_MODEL_ROUND_TOKEN)
			fprintf(stream, "%s%" PRIu64, count++ ? "," : "", stats[r].token);
	fputs("]}\n", stream);
}

/* 必要结果文件关闭成功前，调用者不得发布 done 标记。 */
static bool sample_save(const char *directory, const ModelRun *run, const ModelRoundStats *stats,
			size_t completed, size_t sample, bool warmup, uint64_t request_ns,
			uint64_t input_ns, uint64_t input_h2d_ns, uint64_t input_bytes,
			const char *stop)
{
	char *path;
	FILE *stream;
	bool valid;

	if (asprintf(&path, "%s/sample.json", directory) < 0)
		return false;
	stream = fopen(path, "wx");
	free(path);
	if (!stream)
		return false;
	sample_json(stream, run, stats, completed, sample, warmup, request_ns, input_ns,
		    input_h2d_ns, input_bytes, stop);
	valid = !ferror(stream);
	if (fclose(stream))
		valid = false;
	return valid;
}

/* CLI 等待 release 后输出；API 直接写请求专属 model.log，不改进程 stdout。 */
static void sample_report(const ModelRun *run, const ModelRoundStats *stats, size_t completed,
			  size_t sample, bool warmup, uint64_t request_ns, uint64_t input_ns,
			  uint64_t input_h2d_ns, uint64_t input_bytes, const char *stop)
{
	FILE *stream = run->log;

	fputs("GPGPU_MODEL_SAMPLE ", stream);
	sample_json(stream, run, stats, completed, sample, warmup, request_ns, input_ns,
		    input_h2d_ns, input_bytes, stop);
	for (size_t r = 0; r < completed; r++) {
		const ModelRound *round = &run->program.rounds[r];
		const ModelRoundStats *value = &stats[r];

		fprintf(stream, "GPGPU_MODEL_ROUND {\"sample\":%zu,\"round\":%zu,\"name\":\"%s\",",
			sample, r, round->name);
		fprintf(stream, "\"kind\":%" PRIu64 ",\"forward_ns\":%" PRIu64
			",\"output_ns\":%" PRIu64 ",\"token_upload_ns\":%" PRIu64
			",\"checks_ns\":%" PRIu64 ",\"ready_ns\":%" PRIu64
			",\"token\":%" PRIu64 "}\n", round->kind, value->forward_ns,
			value->output_ns, value->token_upload_ns, value->checks_ns,
			value->ready_ns, value->token);
		for (size_t n = 0; n < round->count; n++) {
			const GpgpuModelOpStats *op = &value->nodes[n];

			fprintf(stream,
				"GPGPU_MODEL_NODE {\"sample\":%zu,\"round\":%zu,\"node\":%zu,",
				sample, r, n);
			fprintf(stream, "\"name\":\"%s\",\"operator_ns\":%" PRIu64
				",\"params_ns\":%" PRIu64 ",\"dispatch_ns\":%" PRIu64
				",\"dispatches\":%" PRIu64 ",\"work_items\":%" PRIu64 "}\n",
				round->nodes[n].name, op->operator_ns, op->params_ns,
				op->dispatch_ns, op->tile_dispatches, op->work_items);
		}
	}
}

/*
 * upload_input() - 经原 RT 同步上传一个已核对的 CPU 预处理输入
 *
 * model_sample() 以此替代该 load 的文件装载，借用指针只用于本次同步 COPY。
 *
 * 1. 按当前逻辑长度重新核对完整 INPUT 范围及物理绑定；
 * 2. 等待全部上传结束后累计 H2D，不保留 input 指针。
 *
 * 失败前已上传字节不回退，设备状态由原 RT 记录；本函数不继续计算。
 *
 * Return: 上传和计时均成功时为 true。
 */
static bool upload_input(ModelRun *run, const ModelRange *range, const void *input)
{
	unsigned int arena;
	uint64_t offset, start;

	/* 1. 匹配 load 的检查不能代替当前 BO 绑定检查。 */
	if (!range_valid(run, range->address, range->bytes, &arena, &offset) ||
	    arena != GPGPU_MODEL_ARENA_INPUT)
		return false;

	/* 2. 同步返回后，调用者才可以释放或改写借用输入。 */
	start = model_clock(run);
	if (!model_status_ok(run, gpgpu_rt_upload_range(run->rt, run->arenas[arena], offset,
						      input, range->bytes), "上传请求输入失败"))
		return false;
	run->h2d_ns += model_elapsed(run, start);
	run->h2d_bytes += range->bytes;
	return !run->clock_error;
}

/*
 * embedding_input_valid() - 在 embedding 派发前核对实际 INT64 token
 *
 * model_sample() 在 INPUT 上传完成、且上游节点或上一轮 token 写入结束后调用。
 * 不提前扫描未来 decode 占位槽，避免把尚未被生成 token 覆盖的旧值当作输入。
 *
 * 1. 用共享布局检查推导词表长度，只检查 COUNT / D0 个实际 token；
 * 2. 按保留的文件编码地址核对逻辑范围，经原 RT 分块回读当前 token；
 * 3. 按小端 INT64 解码，拒绝负数或大于等于该节点词表长度的值。
 *
 * 例：COUNT=6、D0=2、INT8 权重跨度为 8 字节，则词表有 4 行，读取 3 个 token。
 * 上限不依赖 program.vocabulary，非生成图中的合法 embedding 也可使用。
 * 这里只检查索引，不在 CPU 计算 embedding 输出；失败后不继续派发。
 *
 * Return: 非 embedding 节点或全部实际 token 合法时为 true。
 */
static bool embedding_input_valid(ModelRun *run, const ModelNode *node)
{
	const uint64_t *words = node->words;
	GpgpuModelLayout layout;
	unsigned int arena;
	uint64_t offset, bytes;

	/* 1. 布局检查保证 D0 非零，且 token 字节数在声明跨度内可表示。 */
	if (words[GPGPU_EVAL_OP / GPGPU_EVAL_SLOT_BYTES] != GPGPU_EVAL_EMBEDDING_W8)
		return true;
	if (!gpgpu_model_layout_valid(words, node->spans, &layout))
		return false;
	bytes = words[GPGPU_EVAL_COUNT / GPGPU_EVAL_SLOT_BYTES] /
		words[GPGPU_EVAL_D0 / GPGPU_EVAL_SLOT_BYTES] * sizeof(int64_t);

	/* 2. 重定位只改命令地址；范围检查仍使用该节点原来的编码地址。 */
	if (!range_valid(run, node->token_address, bytes, &arena, &offset))
		return false;
	for (uint64_t done = 0; done < bytes;) {
		size_t size = bytes - done < sizeof(run->buffer) ?
			      bytes - done : sizeof(run->buffer);

		if (!model_status_ok(run, gpgpu_rt_download_range(run->rt, run->arenas[arena],
							       offset + done, run->buffer, size),
				     "回读 embedding token 失败"))
			return false;
		/* 3. 包内输入和 API 借用输入都检查已上传后的同一份实际字节。 */
		for (size_t i = 0; i < size; i += sizeof(int64_t)) {
			uint64_t wire;
			int64_t value;

			memcpy(&wire, run->buffer + i, sizeof(wire));
			value = (int64_t)le64toh(wire);
			if (value < 0 || (uint64_t)value >= layout.vocabulary) {
				fprintf(stderr, "embedding token 越界：%s：index=%" PRIu64
					"，token=%" PRId64 "，vocabulary=%" PRIu64 "\n",
					node->name, (done + i) / sizeof(int64_t), value,
					layout.vocabulary);
				return false;
			}
		}
		done += size;
	}
	return true;
}

/*
 * model_sample() - 从真实输入执行一个完整检测或连续生成请求
 *
 * CLI 在宿主发布 start 后调用，API 不经过屏障；权重保持常驻。首批前缀节点
 * 重建本次全部可读 KV，所以重复请求无需清零整个缓存容量。
 *
 * 1. 预分配 CPU 统计，重新装载输入，计入每请求的真实读取和 H2D；
 * 2. 每个 embedding 派发前检查实际 token，按轮同步执行，不在 CPU 计算网络中间值；
 * 3. 回读必要结果；token 轮 CPU argmax，非 EOS 时写下一轮输入；
 * 4. 仅诊断模式回读检查；保存必要结果并发布 done，
 *    宿主 release 后才输出遥测。
 *
 * 失败立即结束请求，部分写入和 KV 不回退，也不得继续下一轮。
 * 本函数不释放 BO，由调用者按 RT 的完成/保留协议收尾。
 *
 * Return: 全请求计算、输出和计时成功时为 true。
 */
static bool model_sample(ModelRun *run, size_t sample, bool warmup, size_t input_index,
			 const void *input)
{
	ModelRoundStats *stats = calloc(run->program.round_count, sizeof(*stats));
	char *directory = NULL;
	char name[sizeof("release-") + sizeof(sample) * 3];
	uint64_t begin, elapsed, input_ns, input_h2d_ns, input_bytes;
	size_t completed = 0;
	const char *stop = run->program.model == GPGPU_MODEL_QWEN ? "budget" : "completed";
	bool valid = false;

	/* 1. 统计分配在计时前完成，输入文件与 H2D 留在真实请求区间内。 */
	if (!stats || asprintf(&directory, "%s/sample-%04zu", run->output, sample) < 0)
		goto out;
	if (mkdir(directory, 0755)) {
		fprintf(stderr, "创建请求输出目录失败：%s：%s\n",
			directory, strerror(errno));
		goto out;
	}
	for (size_t r = 0; r < run->program.round_count; r++) {
		stats[r].nodes = calloc(run->program.rounds[r].count, sizeof(*stats[r].nodes));
		if (!stats[r].nodes)
			goto out;
	}
	begin = model_clock(run);
	input_h2d_ns = run->h2d_ns;
	input_bytes = run->h2d_bytes;
	for (size_t i = 0; i < run->program.load_count; i++) {
		const ModelRange *range = &run->program.loads[i];

		if ((range->address >> GPGPU_MODEL_ADDRESS_SHIFT) != GPGPU_MODEL_ARENA_INPUT + 1)
			continue;
		/* 精确覆盖项只上传借用数据，不再读取包内占位文件。 */
		if (i == input_index) {
			if (!upload_input(run, range, input))
				goto out;
		} else if (!load_range(run, range)) {
			goto out;
		}
	}
	input_ns = model_elapsed(run, begin);
	input_h2d_ns = run->h2d_ns - input_h2d_ns;
	input_bytes = run->h2d_bytes - input_bytes;
	for (size_t r = 0; r < run->program.round_count; r++) {
		const ModelRound *round = &run->program.rounds[r];
		ModelRoundStats *value = &stats[r];
		uint64_t start = model_clock(run);
		char *path = NULL;

		/* 2. 上游成功后才运行下游；中间结果不经 CPU。 */
		for (size_t n = 0; n < round->count; n++) {
			const ModelNode *node = &round->nodes[n];

			if (!embedding_input_valid(run, node) ||
			    !model_status_ok(run, gpgpu_model_execute(&run->executor, node->words,
								     node->rows, node->work_count,
								     GPGPU_EVAL_BACKEND_BLOCK,
								     &value->nodes[n]), node->name))
				goto out;
		}
		value->forward_ns = model_elapsed(run, start);
		start = model_clock(run);
		/* 3. 只有 logits/检测张量交给 CPU 后处理。 */
		if (round->kind != GPGPU_MODEL_ROUND_PREFIX) {
			if (!round->elements || (round->kind == GPGPU_MODEL_ROUND_TOKEN &&
						 round->elements != run->program.vocabulary))
				goto out;
			if ((run->checkpoints || round->kind == GPGPU_MODEL_ROUND_DETECT) &&
			    asprintf(&path, "%s/round-%04zu.output.bin", directory, r) < 0)
				goto out;
			if (!download_range(run, round->output, round->elements * sizeof(float),
					    path, round->kind == GPGPU_MODEL_ROUND_TOKEN,
					    &value->token)) {
				free(path);
				goto out;
			}
			free(path);
		}
		value->output_ns = model_elapsed(run, start);
		value->ready_ns = model_elapsed(run, begin);
		if (round->kind == GPGPU_MODEL_ROUND_TOKEN) {
			if (token_eos(&run->program, value->token))
				stop = "eos";
			else if (round->token_slot) {
				start = model_clock(run);
				if (!upload_token(run, round->token_slot, value->token))
					goto out;
				value->token_upload_ns = model_elapsed(run, start);
			}
		}
		/* 4. 检查回读单列，正式请求关闭；日志在请求结束后输出。 */
		if (run->checkpoints) {
			start = model_clock(run);
			if (!round_checks(run, round, r, directory))
				goto out;
			value->checks_ns = model_elapsed(run, start);
		}
		value->complete = true;
		completed++;
		if (!strcmp(stop, "eos"))
			break;
	}
	elapsed = model_elapsed(run, begin);
	if (run->clock_error || !sample_save(directory, run, stats, completed, sample, warmup,
					    elapsed, input_ns, input_h2d_ns, input_bytes, stop))
		goto out;
	/* 宿主只读本次小结果做必要后处理；大日志等 release 后再输出。 */
	snprintf(name, sizeof(name), "done-%04zu", sample);
	if (!control_publish(run, name))
		goto out;
	snprintf(name, sizeof(name), "release-%04zu", sample);
	if (!control_wait(run, name))
		goto out;
	sample_report(run, stats, completed, sample, warmup, elapsed, input_ns,
		      input_h2d_ns, input_bytes, stop);
	valid = !ferror(run->log) && !run->clock_error;

out:
	if (stats)
		for (size_t r = 0; r < run->program.round_count; r++)
			free(stats[r].nodes);
	free(stats);
	free(directory);
	return valid;
}

/* 不把卸载失败当成已确认停止；只有 RT 成功销毁后才关闭 KMT。 */
static bool model_cleanup(ModelRun *run)
{
	if (run->rt) {
		if (!model_status_ok(run, gpgpu_rt_destroy(run->rt),
				     "卸载模型队列失败，保留资源"))
			return false;
		run->rt = NULL;
	}
	if (run->context) {
		if (!model_status_ok(run, gpgpuKmtClose(run->context), "关闭模型连接失败"))
			return false;
		run->context = NULL;
	}
	return true;
}

/* CPU 描述不接管任何 RT/KMT 对象；调用者先按设备寿命规则完成收尾。 */
static void model_cpu_free(ModelRun *run)
{
	program_free(&run->program);
	for (size_t i = 0; i < run->weight_count; i++)
		free(run->weights[i].name);
	free(run->weights);
	free(run->directory);
	free(run->weights_directory);
	free(run->setup_record);
}

/* 保存绝对目录，使后续 Python 改变工作目录时也不会改变权重或 INPUT 的路径根。 */
static char *program_directory(const char *path)
{
	char *copy = strdup(path);
	char *slash, *directory;

	if (!copy)
		return NULL;
	slash = strrchr(copy, '/');
	if (slash) {
		if (slash == copy)
			slash[1] = 0;
		else
			*slash = 0;
	}
	directory = realpath(slash ? copy : ".", NULL);
	free(copy);
	return directory;
}

/*
 * weights_save() - 为会话保存不随换图失效的初始权重合同
 *
 * 初次准备在任何换图前调用；装载区间已拒绝重叠，权重地址因而唯一。
 *
 * 1. 保存权重 arena 逻辑长度并统计 WEIGHTS 装载项；
 * 2. 分配清零的合同数组，深复制名称，避免旧程序回收后悬空。
 *
 * 本函数不上传权重；失败前的 CPU 分配由 model_cpu_free() 统一释放。
 *
 * Return: 所有合同字段均已独立保存时为 true。
 */
static bool weights_save(ModelRun *run)
{
	size_t next = 0;

	/* 1. INPUT 不属于不可变合同。 */
	run->weight_bytes = run->program.sizes[GPGPU_MODEL_ARENA_WEIGHTS];
	for (size_t i = 0; i < run->program.load_count; i++)
		if ((run->program.loads[i].address >> GPGPU_MODEL_ADDRESS_SHIFT) ==
		    GPGPU_MODEL_ARENA_WEIGHTS + 1)
			run->weight_count++;
	if (!run->weight_count)
		return true;

	/* 2. 部分复制失败时，未填充的 name 保持 NULL，统一收尾仍安全。 */
	run->weights = calloc(run->weight_count, sizeof(*run->weights));
	if (!run->weights) {
		run->weight_count = 0;
		return false;
	}
	for (size_t i = 0; i < run->program.load_count; i++) {
		const ModelRange *range = &run->program.loads[i];

		if ((range->address >> GPGPU_MODEL_ADDRESS_SHIFT) != GPGPU_MODEL_ARENA_WEIGHTS + 1)
			continue;
		run->weights[next] = *range;
		run->weights[next].name = strdup(range->name);
		if (!run->weights[next].name)
			return false;
		next++;
	}
	return true;
}

/*
 * weights_contract_valid() - 在换图前核对初始权重集合
 *
 * program_replace() 在设备扩容前调用；装载顺序可以改变，内容合同不能改变。
 *
 * 1. 核对模型标识和权重 arena 的逻辑长度；
 * 2. 按唯一地址匹配每项的长度与名称，再核对集合条数。
 *
 * 初始表和候选表都已拒绝重叠，故不会用重复项冒充遗漏项。
 * 本函数不重新打开任何权重文件。
 *
 * Return: 候选包含且仅包含原权重合同时为 true。
 */
static bool weights_contract_valid(const ModelRun *run, const ModelProgram *program)
{
	size_t count = 0;

	/* 1. 物理容量更大也不能放宽权重的逻辑合同。 */
	if (program->model != run->program.model ||
	    program->sizes[GPGPU_MODEL_ARENA_WEIGHTS] != run->weight_bytes)
		return false;

	/* 2. 只比较合同字段，INPUT 文件允许随请求变化。 */
	for (size_t i = 0; i < program->load_count; i++) {
		const ModelRange *range = &program->loads[i];
		bool found = false;

		if ((range->address >> GPGPU_MODEL_ADDRESS_SHIFT) != GPGPU_MODEL_ARENA_WEIGHTS + 1)
			continue;
		count++;
		for (size_t j = 0; j < run->weight_count; j++) {
			const ModelRange *weight = &run->weights[j];

			if (range->address == weight->address && range->bytes == weight->bytes &&
			    !strcmp(range->name, weight->name)) {
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return count == run->weight_count;
}

/*
 * input_range_valid() - 找到可以由 CPU 预处理输入完整覆盖的装载项
 *
 * run 或换图在设备工作前调用，不保存 input 指针。
 *
 * 1. 全零参数选择原装载表；否则核对指针范围和 INPUT 逻辑区间；
 * 2. 精确匹配完整 load 的地址与长度，拒绝子区间或错误 BO 角色。
 *
 * 装载表已拒绝重叠，因此返回的项不会再被另一包内文件覆盖。
 * 本函数不上传数据；SIZE_MAX 表示没有覆盖项。
 *
 * Return: 参数全零或覆盖项合法时为 true。
 */
static bool input_range_valid(const ModelProgram *program, uint64_t address, const void *input,
			      size_t bytes, size_t *index)
{
	unsigned int arena;
	uint64_t offset;

	/* 1. 不能把部分提供的参数误当作缺省输入。 */
	*index = SIZE_MAX;
	if (!address && !input && !bytes)
		return true;
	if (!address || !input || !bytes || bytes > UINTPTR_MAX - (uintptr_t)input ||
	    !program_range_valid(program, address, bytes, &arena, &offset) ||
	    arena != GPGPU_MODEL_ARENA_INPUT)
		return false;

	/* 2. 长度相同但地址不同也不能覆盖。 */
	for (size_t i = 0; i < program->load_count; i++)
		if (program->loads[i].address == address && program->loads[i].bytes == bytes) {
			*index = i;
			return true;
		}
	return false;
}

/*
 * arenas_resize() - 在两次同步请求之间按需扩大非权重 BO
 *
 * 换图先完成全部 CPU 校验，再调用本函数；此前同步请求已确认结束。
 * 保留能容纳新逻辑长度的 BO，不收缩，也不重传权重或旧 KV。
 *
 * 1. 跳过权重、零长度及容量已经足够的类别；
 * 2. 经原 RT 释放旧数据 BO，成功后才清空旧指针；
 * 3. 分配新容量；失败后禁止继续使用旧图的已失效绑定。
 *
 * 失败前已释放的对象不恢复；未释放对象仍登记在 RT 中，交由 close 收尾。
 * 本函数不重定位或提交节点。
 *
 * Return: 全部容量和计时均有效时为 true。
 */
static bool arenas_resize(ModelRun *run, const ModelProgram *program)
{
	for (unsigned int i = 0; i < GPGPU_MODEL_ARENA_COUNT; i++) {
		uint64_t bytes = program->sizes[i];

		/* 1. 容量与逻辑长度分开，缩小请求不缩小物理 BO。 */
		if (i == GPGPU_MODEL_ARENA_WEIGHTS || !bytes ||
		    (run->arenas[i] && bytes <= run->arenas[i]->size))
			continue;
		/* 2. 只有前一个请求同步结束后，才允许回收其绑定对象。 */
		if (run->arenas[i]) {
			uint64_t start = model_clock(run);
			GpgpuKmtStatus status = gpgpu_rt_buffer_free(run->rt, run->arenas[i]);

			run->allocation_ns += model_elapsed(run, start);
			if (!model_status_ok(run, status, "扩容时释放模型 BO 失败"))
				return false;
			run->arenas[i] = NULL;
		}
		/* 3. 不在小显存设备上同时持有两份大 BO。 */
		if (!arena_allocate(run, bytes, &run->arenas[i]))
			return false;
	}
	return !run->clock_error;
}

/*
 * program_replace() - 校验新请求后，在 idle 状态替换程序描述与绑定
 *
 * 同步 run 在开始设备前向前调用；纯文件或合同错误不改当前描述和 BO。
 *
 * 1. 独立解析完整程序，核对初始权重合同及可选 INPUT 覆盖；
 * 2. 按新逻辑长度扩容非权重 BO，并只重定位候选描述；
 * 3. 所有绑定有效后一次替换当前描述和 INPUT 目录。
 *
 * 扩容后的失败不回滚物理资源；标记 failed，后续只能 close。
 * 权重目录固定，不从请求目录重新装载或读取权重文件。
 *
 * Return: 完整候选已成为当前程序时为 true。
 */
static bool program_replace(ModelRun *run, const char *path, uint64_t input_address,
			    const void *input, size_t input_bytes, size_t *input_index)
{
	ModelProgram program = { 0 };
	char *directory = program_directory(path);
	bool valid = false;

	/* 1. 所有 CPU 检查先完成，坏请求不能破坏上一个可运行程序。 */
	if (!directory || !program_load(run, path, &program) ||
	    !weights_contract_valid(run, &program) ||
	    !input_range_valid(&program, input_address, input, input_bytes, input_index))
		goto out;
	/* 2. 已释放旧 BO 后不能回到旧程序继续执行。 */
	if (!arenas_resize(run, &program) || !program_relocate(run, &program)) {
		run->failed = true;
		goto out;
	}
	/* 3. 节点绑定、结果范围及输入目录属于同一份程序。 */
	program_free(&run->program);
	run->program = program;
	memset(&program, 0, sizeof(program));
	free(run->directory);
	run->directory = directory;
	directory = NULL;
	valid = true;

out:
	program_free(&program);
	free(directory);
	return valid;
}

/*
 * model_initial_prepare() - 为 CLI 或 API 建立同一套初始资源
 *
 * 调用者已核对基本参数，后续失败统一交给 model_cleanup() 收尾。
 *
 * 1. 保存绝对 INPUT 目录和独立的固定权重目录；
 * 2. 完整解析初始程序，复制权重合同后才建立设备资源。
 *
 * 不借用调用者路径，不打印成功准备日志，也不装载任何请求 INPUT。
 *
 * Return: 初始描述、合同和设备准备均成功时为 true。
 */
static bool model_initial_prepare(ModelRun *run, const char *path, uint64_t tile,
				  uint32_t timeout_ms)
{
	/* 1. 换图只能替换 directory，不能替换 weights_directory。 */
	run->directory = program_directory(path);
	if (!run->directory)
		return false;
	run->weights_directory = strdup(run->directory);
	if (!run->weights_directory)
		return false;

	/* 2. 坏文件不能先造成设备分配或部分权重上传。 */
	if (!program_load(run, path, &run->program)) {
		fprintf(stderr, "模型程序结构不合法\n");
		return false;
	}
	if (!weights_save(run) || !model_setup(run, tile, timeout_ms)) {
		fprintf(stderr, "模型准备失败\n");
		return false;
	}
	return true;
}

/* CLI 原样输出；API 只在 open 后生成一次快照，后续日志引用初始准备成本。 */
static void setup_report(FILE *stream, const ModelRun *run)
{
	fprintf(stream, "GPGPU_MODEL_SETUP {\"setup_ns\":%" PRIu64
		",\"program_read_ns\":%" PRIu64 ",\"allocation_ns\":%" PRIu64
		",\"file_read_ns\":%" PRIu64 ",\"h2d_ns\":%" PRIu64
		",\"h2d_bytes\":%" PRIu64 ",\"tile\":%" PRIu64 ",\"arena_bytes\":[",
		run->setup_ns, run->program_read_ns, run->allocation_ns, run->file_read_ns,
		run->h2d_ns, run->h2d_bytes, run->executor.tile);
	for (unsigned int i = 0; i < GPGPU_MODEL_ARENA_COUNT; i++)
		fprintf(stream, "%s%" PRIu64, i ? "," : "", run->program.sizes[i]);
	fputs("]}\n", stream);
}

/* 进程 RSS 与旧日志保持一致，不把它误写成单个请求独占的内存。 */
static void resources_report(FILE *stream)
{
	struct rusage usage;

	if (!getrusage(RUSAGE_SELF, &usage))
		fprintf(stream, "GPGPU_MODEL_RESOURCES {\"rss_max_kib\":%ld}\n", usage.ru_maxrss);
}

GpgpuModelSession *gpgpu_model_session_open(const char *program_path, uint64_t tile,
					 uint32_t timeout_ms)
{
	ModelRun *run;
	uint64_t actual_tile;
	size_t size;
	FILE *stream;
	bool valid;

	if (!program_path || !*program_path || !timeout_ms ||
	    !gpgpu_model_tile_valid(tile ? tile : GPGPU_MAX_TOTAL_THREADS, &actual_tile))
		return NULL;
	run = calloc(1, sizeof(*run));
	if (!run)
		return NULL;
	if (!model_initial_prepare(run, program_path, actual_tile, timeout_ms))
		goto fail;
	stream = open_memstream(&run->setup_record, &size);
	if (!stream)
		goto fail;
	setup_report(stream, run);
	valid = !ferror(stream);
	if (fclose(stream))
		valid = false;
	if (!valid)
		goto fail;
	return run;

fail:
	if (model_cleanup(run)) {
		model_cpu_free(run);
		free(run);
	} else {
		/* open 无法返回失败句柄，但未知设备访问也不能因此提前释放。 */
		fprintf(stderr, "模型会话打开失败且收尾未确认，保留资源\n");
	}
	return NULL;
}

bool gpgpu_model_session_run(GpgpuModelSession *session, const char *program_path,
			     const char *output, uint64_t input_address, const void *input,
			     size_t input_bytes, bool checkpoints)
{
	ModelRun *run = session;
	char *path;
	FILE *stream;
	struct stat status;
	size_t input_index;
	uint64_t allocation_ns;
	bool valid = false;

	if (!run || !output || !*output || run->failed || run->clock_error ||
	    !run->rt || !run->context || (program_path && !*program_path) ||
	    stat(output, &status) || !S_ISDIR(status.st_mode))
		return false;
	if (asprintf(&path, "%s/model.log", output) < 0)
		return false;
	stream = fopen(path, "wx");
	free(path);
	if (!stream)
		return false;
	run->log = stream;
	run->output = output;
	run->checkpoints = checkpoints;
	allocation_ns = run->allocation_ns;
	run->program_read_ns = 0;
	fputs(run->setup_record, stream);
	if (program_path) {
		if (!program_replace(run, program_path, input_address, input, input_bytes,
				     &input_index)) {
			fprintf(stderr, "请求程序、权重合同或输入范围不合法\n");
			goto out;
		}
	} else if (!input_range_valid(&run->program, input_address, input, input_bytes,
				      &input_index)) {
		fprintf(stderr, "请求输入必须精确覆盖一个完整 INPUT 装载区间\n");
		goto out;
	}
	fprintf(stream, "GPGPU_MODEL_REQUEST_SETUP {\"program_read_ns\":%" PRIu64
		",\"allocation_ns\":%" PRIu64 ",\"arena_bytes\":[",
		run->program_read_ns, run->allocation_ns - allocation_ns);
	for (unsigned int i = 0; i < GPGPU_MODEL_ARENA_COUNT; i++)
		fprintf(stream, "%s%" PRIu64, i ? "," : "", run->program.sizes[i]);
	fputs("]}\n", stream);
	if (fflush(stream))
		goto out;
	valid = model_sample(run, 0, false, input_index, input);
	if (!valid)
		run->failed = true;

out:
	resources_report(stream);
	valid = valid && !run->failed && !run->clock_error && !ferror(stream);
	fprintf(stream, "GPGPU_MODEL_REQUEST_DONE {\"success\":%s,\"clock_error\":%s}\n",
		valid ? "true" : "false", run->clock_error ? "true" : "false");
	if (fclose(stream)) {
		run->failed = true;
		valid = false;
	}
	run->log = NULL;
	run->output = NULL;
	return valid;
}

bool gpgpu_model_session_close(GpgpuModelSession *session)
{
	ModelRun *run = session;

	if (!run)
		return false;
	/* 即使只完成部分销毁，也不允许再次使用旧 Kernel、BO 或队列。 */
	run->failed = true;
	if (!model_cleanup(run))
		return false;
	model_cpu_free(run);
	free(run);
	return true;
}

int gpgpu_model_run_cli(const GpgpuModelOptions *options)
{
	ModelRun run = { .log = stdout };
	uint64_t actual_tile;
	bool valid = false, cleanup;

	if (!options || !options->program_path || !options->output ||
	    !options->repeats || !options->timeout_ms || options->warmups > SIZE_MAX ||
	    options->repeats > SIZE_MAX - options->warmups ||
	    options->control_timeout_ms > UINT64_MAX / (MODEL_NS_PER_SECOND / 1000) ||
	    !gpgpu_model_tile_valid(options->tile ? options->tile : GPGPU_MAX_TOTAL_THREADS,
				    &actual_tile))
		return 2;
	run.output = options->output;
	run.control = options->control;
	run.control_timeout_ms = options->control_timeout_ms;
	run.checkpoints = options->checkpoints;
	if (!model_initial_prepare(&run, options->program_path, actual_tile, options->timeout_ms))
		goto out;
	setup_report(stdout, &run);
	if (fflush(stdout) || !control_publish(&run, "ready"))
		goto out;
	for (uint64_t sample = 0; sample < options->repeats + options->warmups; sample++) {
		char name[sizeof("release-") + sizeof(sample) * 3];

		snprintf(name, sizeof(name), "start-%04" PRIu64, sample);
		if (!control_wait(&run, name) ||
		    !model_sample(&run, sample, sample < options->warmups, SIZE_MAX, NULL)) {
			fprintf(stderr, "完整模型请求失败：sample=%" PRIu64 "\n", sample);
			goto out;
		}
		/* 遥测完成才开始下一请求，避免转嫁上次日志 I/O。 */
		snprintf(name, sizeof(name), "idle-%04" PRIu64, sample);
		if (fflush(stdout) || !control_publish(&run, name))
			goto out;
	}
	valid = true;

out:
	cleanup = model_cleanup(&run);
	resources_report(stdout);
	model_cpu_free(&run);
	printf("GPGPU_MODEL_DONE {\"success\":%s,\"cleanup_ok\":%s,\"clock_error\":%s}\n",
	       valid && cleanup && !run.failed && !run.clock_error ? "true" : "false",
	       cleanup ? "true" : "false", run.clock_error ? "true" : "false");
	return valid && cleanup && !run.failed && !run.clock_error ? 0 : 1;
}
