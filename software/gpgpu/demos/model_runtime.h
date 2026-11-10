/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 模型运行时：CLI 与客体 Python 共用的常驻会话接口
 *
 * 调用者串行使用同一会话，负责保证请求输入指针在同步 run 返回前有效：
 *
 *   open：初始程序 -> 固定权重目录 -> RT、Kernel、五类 BO
 *   run ：请求程序/INPUT -> 同一执行器 -> output/sample-0000 与 model.log
 *   close：确认 RT 卸载 -> 关闭 KMT -> 释放会话
 *
 * 网络前向全部由 model_ops 经 RT 提交；本层只做装载、argmax 和结果保存。
 */
#ifndef GPGPU_MODEL_RUNTIME_H
#define GPGPU_MODEL_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 不透明会话；调用者不得并发执行同一会话的 run 或 close。 */
typedef struct ModelRun GpgpuModelSession;

/**
 * struct GpgpuModelOptions - 旧 CLI 的控制循环参数
 * @program_path: 初始程序文件路径，非空。
 * @output: 已创建的输出目录，非空；样本目录由运行时创建。
 * @control: 可选宿主屏障目录；NULL 表示不等待宿主标记。
 * @control_timeout_ms: 屏障期限，毫秒；零表示无独立期限。
 * @repeats: 正式请求数，非零且与 warmups 之和可由 size_t 表示。
 * @warmups: 预热请求数。
 * @tile: 每次派发的工作项上限；零使用共享上限。
 * @timeout_ms: 每次设备派发的等待期限，非零，单位毫秒。
 * @checkpoints: 是否回读并保存中间检查输出。
 *
 * 字符串只在 gpgpu_model_run_cli() 调用期间借用。
 */
typedef struct GpgpuModelOptions {
	const char *program_path;
	const char *output;
	const char *control;
	uint64_t control_timeout_ms;
	uint64_t repeats;
	uint64_t warmups;
	uint64_t tile;
	uint32_t timeout_ms;
	bool checkpoints;
} GpgpuModelOptions;

/**
 * gpgpu_model_session_open() - 解析初始程序并建立常驻模型资源
 * @program_path: 非空程序文件路径；权重目录固定为此文件所在目录。
 * @tile: 工作项上限；零使用共享上限，非零须能容纳一个完整工作组。
 * @timeout_ms: 每次设备派发的等待期限，非零，单位毫秒。
 *
 * 只上传一次初始 WEIGHTS，保存其地址、长度、名称与 arena 长度合同。
 * INPUT 在 run 时装载。路径在返回前复制，调用者不必继续保留字符串。
 * 失败时尝试按 RT/KMT 原顺序收尾；无法确认停止的资源保留，不提前释放。
 *
 * Return: 完整准备成功时返回会话；参数、程序或设备准备失败时返回 NULL。
 */
GpgpuModelSession *gpgpu_model_session_open(const char *program_path, uint64_t tile,
					 uint32_t timeout_ms);

/**
 * gpgpu_model_session_run() - 在常驻会话中同步执行本次程序的前向轮次
 * @session: open 返回的会话，调用者串行使用；失败关闭后不可再运行。
 * @program_path: NULL 复用当前已重定位程序；否则完整解析并校验新程序。
 * @output: 已创建的请求目录；写入 model.log 和 sample-0000/，不得已有同名结果。
 * @input_address: 文件编码的 INPUT 地址；与 input、input_bytes 全零时使用装载表。
 * @input: 可选 CPU 预处理输入，只借用到本次调用返回；非空时须覆盖一个完整 INPUT load。
 * @input_bytes: input 的精确字节数，须与匹配 load 一致。
 * @checkpoints: 是否保存中间检查输出和 token 轮 logits。
 *
 * 新程序须保持初始权重合同，权重不重传；INPUT 从新程序所在目录读取。
 * 非权重 BO 只在上次同步请求结束后按需扩容，逻辑范围始终受新程序 sizes 约束。
 * 可选输入上传后跳过其占位文件；本接口不会清空 KV。
 * 同一次生成的连续窗口由调用者保持 KV 容量、布局及绝对位置，禁止在延续时扩容 KV。
 * 新对话请求仍由应用从完整前缀重建可读 KV，不能把旧请求的有效长度带入新请求。
 * 本接口没有宿主屏障，不在 stdout 输出逐节点日志，也不读取 CPU 前向参考。
 * 运行失败不回滚部分写入，禁止继续复用失败会话；close 仍按原资源保留协议处理。
 * 纯参数或新程序校验失败不更换当前程序，也不提交设备工作。
 *
 * Return: 本次程序、结果文件、遥测与计时均成功时为 true。
 */
bool gpgpu_model_session_run(GpgpuModelSession *session, const char *program_path,
			     const char *output, uint64_t input_address, const void *input,
			     size_t input_bytes, bool checkpoints);

/**
 * gpgpu_model_session_close() - 确认设备停止访问后释放常驻会话
 * @session: 非空会话，调用者已停止对此会话的一切并发访问。
 *
 * 先销毁 RT，再关闭 KMT；失败不提前释放剩余资源或会话，保留句柄供重试 close。
 * close 一旦开始，无论是否成功，都不得再调用 run。成功后句柄失效。
 *
 * Return: RT/KMT 已确认回收且会话已释放时为 true；NULL 或收尾失败为 false。
 */
bool gpgpu_model_session_close(GpgpuModelSession *session);

/**
 * gpgpu_model_run_cli() - 保留旧 CLI 日志与宿主屏障的控制循环
 * @options: 已解析的选项，调用期间保持有效。
 *
 * 不走每请求换程序或 model.log 路径，仍在 stdout 发布原日志协议。
 *
 * Return: 全部请求和正常收尾成功时为 0，执行失败为 1，参数非法为 2。
 */
int gpgpu_model_run_cli(const GpgpuModelOptions *options);

#endif /* GPGPU_MODEL_RUNTIME_H */
