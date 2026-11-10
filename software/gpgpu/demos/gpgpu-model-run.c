// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 完整模型 CLI：解析旧命令行选项，控制循环与设备资源由共用运行时处理
 */
#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include "model_runtime.h"

/* 数值参数须完整且可表示，不接受尾随字符或负号。 */
static bool number_valid(const char *text, uint64_t *value)
{
	char *end;

	if (!text || !*text || *text == '-')
		return false;
	errno = 0;
	*value = strtoull(text, &end, 10);
	return !errno && !*end;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "program", required_argument, NULL, 'p' },
		{ "output", required_argument, NULL, 'o' },
		{ "repeats", required_argument, NULL, 'r' },
		{ "warmups", required_argument, NULL, 'w' },
		{ "tile", required_argument, NULL, 't' },
		{ "timeout-ms", required_argument, NULL, 'm' },
		{ "checkpoints", no_argument, NULL, 'c' },
		{ "control-dir", required_argument, NULL, 'd' },
		{ "control-timeout-ms", required_argument, NULL, 'u' },
		{ NULL, 0, NULL, 0 },
	};
	GpgpuModelOptions run = { .repeats = 1, .timeout_ms = 300000 };
	uint64_t timeout;
	int option;

	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (option) {
		case 'p':
			run.program_path = optarg;
			break;
		case 'o':
			run.output = optarg;
			break;
		case 'r':
			if (!number_valid(optarg, &run.repeats) || !run.repeats)
				return 2;
			break;
		case 'w':
			if (!number_valid(optarg, &run.warmups))
				return 2;
			break;
		case 't':
			if (!number_valid(optarg, &run.tile) || !run.tile)
				return 2;
			break;
		case 'm':
			if (!number_valid(optarg, &timeout) || !timeout || timeout > UINT32_MAX)
				return 2;
			run.timeout_ms = timeout;
			break;
		case 'c':
			run.checkpoints = true;
			break;
		case 'd':
			run.control = optarg;
			break;
		case 'u':
			if (!number_valid(optarg, &run.control_timeout_ms) ||
			    !run.control_timeout_ms)
				return 2;
			break;
		default:
			return 2;
		}
	}
	if (optind != argc)
		return 2;
	return gpgpu_model_run_cli(&run);
}
