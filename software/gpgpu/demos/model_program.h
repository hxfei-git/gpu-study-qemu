/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 模型程序：宿主适配器与客体运行器之间的小端文件约定
 *
 * 宿主只编排已支持的模型，不在文件中保存 CPU 前向结果：
 *
 *   具名权重/真实输入 -> 五个常驻 BO -> 逐轮算子 -> logits 或检测张量
 *                            ^
 *   文件内地址的高字节选择 BO，低位保存偏移；运行器装载后换成 GPUVA。
 *
 * 所有整数均为小端 u64；字符串为长度加原始 UTF-8 字节，不含终止符。
 * 文件头依次是 magic、model、arena_count、round_count、load_count、
 * eos_count、vocabulary、prompt_tokens，再跟 BO 长度、EOS 与装载表。
 * 每轮保存 kind、node_count、output、elements、token_slot、check_count、
 * 名称，随后是检查输出和节点；节点保存 rows、work_count、名称、命令槽、
 * 六输入/输出/三暂存的跨度。只重定位地址槽，维度与浮点位保持原样。
 */
#ifndef GPGPU_MODEL_PROGRAM_H
#define GPGPU_MODEL_PROGRAM_H

/* 文件格式标识，恰好八字节；不是设备协议版本。 */
#define GPGPU_MODEL_PROGRAM_MAGIC "GPGMDL01"
/* Qwen 的逐轮生成程序。 */
#define GPGPU_MODEL_QWEN 1
/* YOLO 的单图检测程序。 */
#define GPGPU_MODEL_YOLO 2
/* 常驻只读推理权重所在 BO。 */
#define GPGPU_MODEL_ARENA_WEIGHTS 0
/* 真实输入、位置与掩码等输入元数据所在 BO。 */
#define GPGPU_MODEL_ARENA_INPUT 1
/* 按最后消费者复用的激活 BO。 */
#define GPGPU_MODEL_ARENA_ACTIVATION 2
/* 跨轮保留的 KV BO；YOLO 不使用。 */
#define GPGPU_MODEL_ARENA_KV 3
/* 串行复合算子共用的 scratch BO。 */
#define GPGPU_MODEL_ARENA_SCRATCH 4
/* 文件格式中固定的 BO 类别数。 */
#define GPGPU_MODEL_ARENA_COUNT 5
/* 文件地址的高字节为 BO 编号加一，零地址表示未提供。 */
#define GPGPU_MODEL_ADDRESS_SHIFT 56
/* 只更新前缀 KV，不产生生成 token 的 prefill 轮。 */
#define GPGPU_MODEL_ROUND_PREFIX 0
/* 回读 logits、选 token 并按需写入下一轮输入的轮。 */
#define GPGPU_MODEL_ROUND_TOKEN 1
/* 回读完整检测张量的轮。 */
#define GPGPU_MODEL_ROUND_DETECT 2
/* 打包子区间的字节对齐；不改变 BO 的页对齐。 */
#define GPGPU_MODEL_ALIGNMENT 64

#endif /* GPGPU_MODEL_PROGRAM_H */
