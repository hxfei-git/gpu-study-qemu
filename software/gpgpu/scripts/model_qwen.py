# SPDX-License-Identifier: GPL-2.0-or-later
"""Qwen2.5-0.5B：完整 W8 设备图、本地文本编码与独立的连续 KV CPU 参考。

图中只固化真实 prompt、位置、因果 mask 和模型配置；生成 token 由客体逐轮
读取自己的 logits 后选择。CPU 参考另行保存，不作为设备的输入或 KV 初值。
"""

from functools import lru_cache
import gc
import hashlib
import json
from pathlib import Path
import resource
import sys
import time

from gpgpu_model_eval import EVAL_ROOT, QWEN_REVISION, prepare_environment


_CAPACITY_LIMIT = 2048
_LINEAR_ROWS = 2048
_CHECK_LAYERS = (0, 12, 23)
_DEFAULT_PROMPT = "请用一句话介绍你自己。"


def _material():
    """材料只从固定本地目录读取；依赖的配置和缓存沿用实验隔离入口。"""
    if not sys.flags.isolated:
        raise ValueError("读取 Qwen 材料必须使用 python -I")
    prepare_environment()
    path = EVAL_ROOT / "material" / ("qwen-" + QWEN_REVISION + "-complete")
    provenance = json.loads((path / "provenance.json").read_text(encoding="utf-8"))
    if provenance["revision"] != QWEN_REVISION:
        raise ValueError("Qwen 本地材料 revision 不符")
    return path, provenance


def _configuration(info=None):
    path, provenance = _material()
    config = json.loads((path / "config.json").read_text(encoding="utf-8"))
    if info is not None:
        if (info["revision"] != provenance["revision"] or
                info["repository"] != provenance["repository"]):
            raise ValueError("Qwen manifest 与本地材料版本不符")
        for key, value in info["config"].items():
            if config.get(key) != value:
                raise ValueError(f"Qwen manifest 与本地配置不符: {key}")
    if (config["use_sliding_window"] or config["hidden_act"] != "silu" or
            not config["tie_word_embeddings"]):
        raise ValueError("本适配器只支持固定的全注意力、SiLU、共享词表 Qwen 配置")
    return config


def _generation():
    path, _ = _material()
    config = json.loads((path / "generation_config.json").read_text(encoding="utf-8"))
    eos = config["eos_token_id"]
    eos = [eos] if isinstance(eos, int) else list(eos)
    return config, [int(value) for value in eos]


@lru_cache(maxsize=1)
def _tokenizer():
    path, _ = _material()
    from transformers import AutoTokenizer

    return AutoTokenizer.from_pretrained(str(path), local_files_only=True,
                                         trust_remote_code=False, token=False)


def tokenize(prompt=_DEFAULT_PROMPT, use_chat=True, prefix_length=None):
    """用官方本地 tokenizer 编码指令，或截取真实 Alice 正文的 token 前缀。"""
    tokenizer = _tokenizer()
    source = None
    if prefix_length is not None:
        if isinstance(prefix_length, bool) or not isinstance(prefix_length, int) or prefix_length <= 0:
            raise ValueError("Alice token 前缀长度必须是正整数")
        path = EVAL_ROOT / "material/qwen-text/alice.txt"
        text = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
        marker = "CHAPTER I.\nDown the Rabbit-Hole"
        if marker not in text:
            raise ValueError("Alice 材料缺少已约定的正文起点")
        text = text[text.index(marker):]
        all_ids = tokenizer(text, add_special_tokens=False)["input_ids"]
        if len(all_ids) < prefix_length:
            raise ValueError("真实 Alice token 不足，不允许循环文本或随机补齐")
        ids = all_ids[:prefix_length]
        original = text
        encoded_text = tokenizer.decode(ids, skip_special_tokens=False,
                                        clean_up_tokenization_spaces=False)
        policy = "alice-token-prefix"
        source = {"path": str(path), "selection": marker, "prefix_length": prefix_length}
        use_chat = False
    else:
        original = _DEFAULT_PROMPT if prompt is None else prompt
        if not isinstance(original, str) or not original:
            raise ValueError("Qwen prompt 必须是非空文本")
        encoded_text = (tokenizer.apply_chat_template(
            [{"role": "user", "content": original}], tokenize=False,
            add_generation_prompt=True) if use_chat else original)
        ids = tokenizer(encoded_text, add_special_tokens=False)["input_ids"]
        policy = "official-chat-template" if use_chat else "plain-text"
    ids = [int(value) for value in ids]
    if not ids:
        raise ValueError("Qwen prompt 编码后为空")
    generation, eos = _generation()
    _, provenance = _material()
    metadata = {
        "original_text": original, "encoded_text": encoded_text, "token_ids": ids,
        "template_policy": policy, "use_chat": bool(use_chat),
        "add_generation_prompt": bool(use_chat), "add_special_tokens": False,
        "chat_template": tokenizer.chat_template if use_chat else None,
        "source": source, "repository": provenance["repository"],
        "revision": provenance["revision"], "eos": eos,
        "official_generation_config": generation,
    }
    return ids, metadata


def decode_tokens(tokens):
    """仅做生成后的文本解码，不生成或改写设备输入。"""
    return _tokenizer().decode([int(value) for value in tokens], skip_special_tokens=True,
                               clean_up_tokenization_spaces=False)


def _valid_request(ids, max_new_tokens, capacity, prefill_chunk, vocabulary):
    values = list(ids)
    if (not values or any(isinstance(value, bool) or not isinstance(value, int) or
                          value < 0 or value >= vocabulary for value in values)):
        raise ValueError("Qwen 输入必须是非空且位于词表范围内的整数 token 列表")
    if (any(isinstance(value, bool) or not isinstance(value, int) or value <= 0
            for value in (max_new_tokens, capacity, prefill_chunk)) or
            capacity > _CAPACITY_LIMIT):
        raise ValueError(f"生成预算、容量和 prefill chunk 必须为正整数，容量最多 {_CAPACITY_LIMIT}")
    if len(values) + max_new_tokens - 1 > capacity:
        raise ValueError("prompt 加生成预算超过 KV 容量，不允许静默截断")
    return values


def _round_plan(prompt_tokens, max_new_tokens, prefill_chunk):
    result = []
    for past in range(0, prompt_tokens, prefill_chunk):
        length = min(prefill_chunk, prompt_tokens - past)
        last = past + length == prompt_tokens
        result.append({"name": f"prefill-{len(result):04d}", "past": past,
                       "length": length, "kind": "token" if last else "prefix",
                       "token_index": 0 if last else None})
    for index in range(1, max_new_tokens):
        result.append({"name": f"decode-{index:04d}", "past": prompt_tokens + index - 1,
                       "length": 1, "kind": "token", "token_index": index})
    return result


def _inverse_frequencies(config):
    """仅生成依赖固定配置的 inv_freq，保留官方 FP32 舍入；不计算位置或激活。"""
    from transformers import Qwen2Config
    from transformers.models.qwen2.modeling_qwen2 import Qwen2RotaryEmbedding

    rotary = Qwen2RotaryEmbedding(config=Qwen2Config.from_dict(config), device="cpu")
    return rotary.inv_freq.detach().numpy()


def _rope(graph, tensor, cos, sin, name):
    _, heads, tokens, width = tensor.shape
    return graph.op("rope", [tensor, cos, sin], tensor.shape,
                    [heads, tokens, width, *tensor.stride, 1], name=name)


def _attention(graph, hidden, prefix, name, key, value, past, cos, sin, mask, config):
    """每个 KV head 被一组 query head 共享；BMM 的右侧 batch stride 为零。"""
    tokens = hidden.shape[1]
    heads, kv_heads = config["num_attention_heads"], config["num_key_value_heads"]
    width = config["hidden_size"] // heads
    group_heads = heads // kv_heads
    end = past + tokens
    q = graph.linear(hidden, prefix + ".q_proj", name=name + ".q_proj")
    k = graph.linear(hidden, prefix + ".k_proj", name=name + ".k_proj")
    v = graph.linear(hidden, prefix + ".v_proj", name=name + ".v_proj")
    q = graph.permute(graph.reshape(q, (1, tokens, heads, width)), (0, 2, 1, 3))
    k = graph.permute(graph.reshape(k, (1, tokens, kv_heads, width)), (0, 2, 1, 3))
    q = _rope(graph, q, cos, sin, name + ".q_rope")
    k = _rope(graph, k, cos, sin, name + ".k_rope")

    # 只写本轮新增的 token-major 区间；旧前缀不回读 Host，也不重新上传。
    new_key = graph.reshape(graph.permute(k, (0, 2, 1, 3)), (tokens, kv_heads, width))
    new_value = graph.reshape(v, (tokens, kv_heads, width))
    graph.copy_to(new_key, graph.slice(key, 0, past, tokens), name=name + ".key_append")
    graph.copy_to(new_value, graph.slice(value, 0, past, tokens), name=name + ".value_append")
    key_prefix = graph.slice(key, 0, 0, end)
    value_prefix = graph.slice(value, 0, 0, end)
    attended = []
    for group in range(kv_heads):
        group_name = name + f".kv{group}"
        query = graph.reshape(graph.slice(q, 1, group * group_heads, group_heads),
                              (group_heads, tokens, width))
        # [token, KV head, channel] 的同一 head 在相邻 token 间跨 kv_heads*width。
        offset = group * width * key.itemsize
        shared_key = graph.view(key_prefix, (1, width, end),
                                (0, 1, kv_heads * width), offset=offset)
        shared_value = graph.view(value_prefix, (1, end, width),
                                  (0, kv_heads * width, 1), offset=offset)
        scores = graph.scale(graph.bmm(query, shared_key, name=group_name + ".qk"),
                             width ** -0.5, name=group_name + ".scale")
        scores = graph.binary("add", scores, mask, name=group_name + ".causal_mask")
        probabilities = graph.softmax(scores, name=group_name + ".softmax")
        attended.append(graph.bmm(probabilities, shared_value, name=group_name + ".av"))
    combined = graph.concat(attended, 0, name=name + ".heads")
    combined = graph.reshape(graph.permute(combined, (1, 0, 2)), hidden.shape)
    projected = graph.linear(combined, prefix + ".o_proj", name=name + ".o_proj")
    return projected, key_prefix, value_prefix


def build_qwen(graph, ids, max_new_tokens=16, capacity=2048, prefill_chunk=128,
               assets=None, round_range=None, generated_tokens=()):
    """展开全部前向或指定的全局轮次窗口，KV 与输入索引始终保持绝对位置。

    round_range 为非空的半开区间；generated_tokens 恰好包含窗口之前设备已产的
    token，按顺序填入原全局输入槽。调用者负责保持同一 capacity 的常驻 KV。
    """
    import numpy as np

    if graph.model != "qwen":
        raise ValueError("Qwen 适配器要求 GraphBuilder(manifest, 'qwen')")
    config = _configuration(graph.model_info) if assets is None else assets["config"]
    if assets is not None and any(config.get(key) != value for key, value in
                                  graph.model_info["config"].items()):
        raise ValueError("客体配置与固定权重清单不符")
    ids = _valid_request(ids, max_new_tokens, capacity, prefill_chunk, config["vocab_size"])
    layers, hidden = config["num_hidden_layers"], config["hidden_size"]
    heads, kv_heads = config["num_attention_heads"], config["num_key_value_heads"]
    width = hidden // heads
    if hidden % heads or heads % kv_heads or width % 2:
        raise ValueError("Qwen head 布局不符合分组注意力与 split-half RoPE 契约")
    plan = _round_plan(len(ids), max_new_tokens, prefill_chunk)
    total_rounds = len(plan)
    if round_range is None:
        start, stop = 0, total_rounds
    else:
        if (not isinstance(round_range, (tuple, list)) or len(round_range) != 2 or
                any(isinstance(value, bool) or not isinstance(value, int)
                    for value in round_range)):
            raise ValueError("Qwen 轮次窗口必须是两个整数构成的半开区间")
        start, stop = round_range
        if not 0 <= start < stop <= total_rounds:
            raise ValueError("Qwen 轮次窗口为空或超出完整计划")
    generated_tokens = list(generated_tokens)
    previous_tokens = sum(current["kind"] == "token" for current in plan[:start])
    if (len(generated_tokens) != previous_tokens or
            any(isinstance(value, bool) or not isinstance(value, int) or
                value < 0 or value >= config["vocab_size"] for value in generated_tokens)):
        raise ValueError("Qwen 已生成 token 必须恰好覆盖窗口之前的 token 轮且位于词表内")
    rounds = plan[start:stop]
    token_data = np.zeros(len(ids) + max_new_tokens - 1, dtype="<i8")
    token_data[:len(ids)] = ids
    token_data[len(ids):len(ids) + previous_tokens] = generated_tokens
    token_input = graph.const("qwen.tokens", token_data)
    graph.input_tensor = token_input
    keys = [graph.persistent(f"qwen.key.{index}", (capacity, kv_heads, width))
            for index in range(layers)]
    values = [graph.persistent(f"qwen.value.{index}", (capacity, kv_heads, width))
              for index in range(layers)]
    inverse_values = (_inverse_frequencies(config) if assets is None else
                      np.asarray(assets["inv_freq"], dtype="<f4"))
    if inverse_values.shape != (width // 2,) or not np.isfinite(inverse_values).all():
        raise ValueError("Qwen 固定旋转频率形状或数值非法")
    inverse = graph.const("qwen.inv_freq", inverse_values.reshape(1, 1, width // 2))
    embedding = graph.weight("model.embed_tokens.weight", "q")
    embedding_scales = graph.weight("model.embed_tokens.weight", "scales")
    for round_index, current in enumerate(rounds):
        name, past, tokens = current["name"], current["past"], current["length"]
        end = past + tokens
        kind = graph.program["GPGPU_MODEL_ROUND_" + current["kind"].upper()]
        graph.begin_round(name, kind)
        graph.stage = "position"
        positions = graph.const(name + ".positions",
                                np.arange(past, end, dtype=np.float32).reshape(1, tokens, 1))
        # 只有位置与 inv_freq 是输入；频率乘法、三角函数和旋转全部在设备执行。
        frequencies = graph.binary("mul", positions, inverse, name=name + ".frequencies")
        angles = graph.concat([frequencies, frequencies], -1, name=name + ".angles")
        cos = graph.activation("cos", angles, name=name + ".cos")
        sin = graph.activation("sin", angles, name=name + ".sin")
        future = np.arange(end)[None, :] > np.arange(past, end)[:, None]
        mask = graph.const(name + ".mask", np.where(future, np.finfo(np.float32).min,
                           np.float32(0)).astype("<f4").reshape(1, tokens, end))
        inputs = graph.slice(token_input, 0, past, tokens)
        graph.stage = "embedding"
        x = graph.op("embedding_w8", [inputs, embedding, embedding_scales],
                     (1, tokens, hidden), [hidden], name=name + ".embedding")
        checks = []
        for index in range(layers):
            prefix, node_name = f"model.layers.{index}", name + f".layer-{index}"
            graph.stage = f"layer-{index}.attention"
            normalized = graph.rmsnorm(x, prefix + ".input_layernorm", config["rms_norm_eps"],
                                       name=node_name + ".input_norm")
            projected, key_prefix, value_prefix = _attention(
                graph, normalized, prefix + ".self_attn", node_name + ".attention",
                keys[index], values[index], past, cos, sin, mask, config)
            x = graph.binary("add", x, projected, name=node_name + ".attention_residual")
            graph.stage = f"layer-{index}.mlp"
            normalized = graph.rmsnorm(x, prefix + ".post_attention_layernorm",
                                       config["rms_norm_eps"], name=node_name + ".post_norm")
            gate = graph.activation("silu", graph.linear(normalized, prefix + ".mlp.gate_proj",
                                    name=node_name + ".gate"), name=node_name + ".silu")
            up = graph.linear(normalized, prefix + ".mlp.up_proj", name=node_name + ".up")
            gated = graph.binary("mul", gate, up, name=node_name + ".gated")
            down = graph.linear(gated, prefix + ".mlp.down_proj", name=node_name + ".down")
            x = graph.binary("add", x, down, name=node_name + ".mlp_residual")
            if graph.capture_checks and index in _CHECK_LAYERS:
                checks.extend([(f"hidden-{index}", x), (f"key-{index}", key_prefix),
                               (f"value-{index}", value_prefix)])
        output, token_slot = None, None
        if current["kind"] == "token":
            graph.stage = "lm_head"
            last = graph.slice(x, 1, tokens - 1, 1)
            last = graph.rmsnorm(last, "model.norm", config["rms_norm_eps"], name=name + ".norm")
            output = graph.linear(last, "model.embed_tokens", name=name + ".lm_head")
            if current["token_index"] + 1 < max_new_tokens and round_index + 1 < len(rounds):
                token_slot = graph.slice(token_input, 0, end, 1)
        graph.end_round(output=output, token_slot=token_slot, checks=checks)
    if not any(current["kind"] == "token" for current in rounds):
        # 纯前缀窗口仍保留初始权重合同；末端 norm 在首轮各层权重之后登记。
        graph.weight("model.norm.weight")
    if assets is None:
        generation, eos = _generation()
    else:
        generation = assets["generation_config"]
        value = generation["eos_token_id"]
        eos = [int(item) for item in (value if isinstance(value, list) else [value])]
        if not eos or any(item < 0 or item >= config["vocab_size"] for item in eos):
            raise ValueError("Qwen EOS 不在固定词表内")
    return {
        "repository": graph.model_info["repository"], "revision": graph.model_info["revision"],
        "config": config, "head_dim": width, "queries_per_kv_head": heads // kv_heads,
        "precision": "w8-per-output-row-fp32-activation-kv", "capacity": capacity,
        "prefill_chunk": prefill_chunk, "max_new_tokens": max_new_tokens,
        "prompt_tokens": len(ids), "prompt_ids": ids, "vocabulary": config["vocab_size"],
        "eos": eos, "rounds": rounds, "round_range": [start, stop],
        "total_rounds": total_rounds, "tied_lm_head": "model.embed_tokens.weight",
        "generation_policy": "greedy_argmax_first_index_until_eos_or_budget",
        "official_generation_config": generation,
        "boundary": ("窗口输入补入此前设备生成的 token；KV 和网络中间值仍只由设备产生"
                     if generated_tokens else
                     "输入包只含真实 prompt 和零占位；生成 token、KV 和中间值均由设备产生"),
    }


class _RawWeights:
    """只映射具名 rawbin；W8 不读取原 FP32 大矩阵，也不重新量化。"""

    def __init__(self, manifest_path, info):
        self.root = Path(manifest_path).resolve().parent
        self.entries = info["weights"]
        self.maps = {}
        self.used = set()

    def mapped(self, name, role="weight"):
        import numpy as np

        entry = self.entries[name][role]
        path = (self.root / entry["path"]).resolve()
        if not path.is_relative_to(self.root) or path.stat().st_size != entry["bytes"]:
            raise ValueError(f"Qwen raw 权重路径或长度非法: {name}/{role}")
        key = (entry["path"], entry["dtype"])
        if key not in self.maps:
            dtype = {"float32": "<f4", "int8": "i1"}[entry["dtype"]]
            self.maps[key] = np.memmap(path, dtype=dtype, mode="c", shape=tuple(entry["shape"]))
        self.used.add((name, role))
        return self.maps[key]

    def small(self, name):
        import numpy as np
        import torch

        value = self.mapped(name)
        if value.ndim != 1:
            raise ValueError(f"仅 FP32 bias 和 norm 可作为小权重装载: {name}")
        return torch.from_numpy(np.array(value, dtype="<f4", copy=True))


def _reference_model(manifest_path, info, config_dict, original_fp32):
    """在 meta 建官方结构，逐个替换矩阵，避免先分配整套 FP32 模型。"""
    import numpy as np
    import torch
    import torch.nn.functional as functional
    from transformers import Qwen2Config
    from transformers.models.qwen2.modeling_qwen2 import Qwen2Model, Qwen2RotaryEmbedding

    raw = _RawWeights(manifest_path, info)

    class RowLinear(torch.nn.Module):
        """每次只反量化有限输出行；lm_head 同样只消费最后位置。"""

        def __init__(self, name, has_bias=False):
            super().__init__()
            self.matrix = raw.mapped(name, "weight" if original_fp32 else "q")
            self.scales = None if original_fp32 else raw.mapped(name, "scales")
            self.out_features, self.in_features = self.matrix.shape
            self.bias = raw.small(name[:-len(".weight")] + ".bias") if has_bias else None

        def forward(self, x):
            result = torch.empty((*x.shape[:-1], self.out_features), dtype=torch.float32,
                                 device="cpu")
            for first in range(0, self.out_features, _LINEAR_ROWS):
                last = min(first + _LINEAR_ROWS, self.out_features)
                if self.scales is None:
                    weight = torch.from_numpy(self.matrix[first:last])
                else:
                    weight = torch.from_numpy(np.array(self.matrix[first:last],
                                                        dtype=np.float32, copy=True))
                    weight.mul_(torch.from_numpy(self.scales[first:last])[:, None])
                bias = self.bias[first:last] if self.bias is not None else None
                result[..., first:last] = functional.linear(x, weight, bias)
            return result

    class RowEmbedding(torch.nn.Module):
        """词嵌入仅解量化本轮真正索引到的行。"""

        def __init__(self, name):
            super().__init__()
            self.matrix = raw.mapped(name, "weight" if original_fp32 else "q")
            self.scales = None if original_fp32 else raw.mapped(name, "scales")

        def forward(self, ids):
            indices = ids.numpy()
            result = torch.from_numpy(np.array(self.matrix[indices], dtype=np.float32, copy=True))
            if self.scales is not None:
                result.mul_(torch.from_numpy(np.array(self.scales[indices], copy=True))[..., None])
            return result

    config = Qwen2Config.from_dict({**config_dict, "torch_dtype": "float32"})
    config._attn_implementation = "eager"
    with torch.device("meta"):
        model = Qwen2Model(config).float()
    for name, module in list(model.named_modules()):
        if isinstance(module, torch.nn.Embedding):
            replacement = RowEmbedding("model." + name + ".weight")
        elif isinstance(module, torch.nn.Linear):
            replacement = RowLinear("model." + name + ".weight", module.bias is not None)
        else:
            continue
        parent, _, child = name.rpartition(".")
        setattr(model.get_submodule(parent), child, replacement)
    # 替换矩阵后只剩 norm 参数；rotary 的 meta buffer 必须在 CPU 重新构造。
    for name, parameter in list(model.named_parameters()):
        parent, _, child = name.rpartition(".")
        value = raw.small("model." + name)
        if tuple(value.shape) != tuple(parameter.shape):
            raise ValueError(f"Qwen 小权重形状不符: model.{name}")
        setattr(model.get_submodule(parent), child, torch.nn.Parameter(value, requires_grad=False))
    model.rotary_emb = Qwen2RotaryEmbedding(config=config, device="cpu")
    return model.eval(), RowLinear("model.embed_tokens.weight").eval(), raw


def _save_tensor(output_dir, name, tensor):
    import numpy as np

    array = np.ascontiguousarray(tensor.detach().cpu().numpy(), dtype="<f4")
    path = output_dir / name
    path.write_bytes(memoryview(array).cast("B"))
    return {"path": path.relative_to(output_dir).as_posix(), "shape": list(array.shape),
            "dtype": "float32", "bytes": array.nbytes}


def _reference_run(manifest_path, manifest, ids, output_dir, max_new_tokens,
                   prefill_chunk, config, original_fp32):
    import torch
    import transformers
    from transformers.cache_utils import DynamicCache

    started = time.perf_counter()
    model, lm_head, raw = _reference_model(manifest_path, manifest["models"]["qwen"],
                                           config, original_fp32)
    prepare_seconds = time.perf_counter() - started
    cache = DynamicCache()
    captured = {}
    handles = []
    for layer in _CHECK_LAYERS:
        def capture(module, args, output, index=layer):
            captured[index] = output[0].detach()
        handles.append(model.layers[layer].register_forward_hook(capture))
    generation, eos = _generation()
    tokens, rounds = [], []
    forward_seconds = head_seconds = save_seconds = 0.0
    prefill_seconds = decode_seconds = 0.0
    stop_reason = "max_new_tokens"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        with torch.inference_mode(), torch.autocast(device_type="cpu", enabled=False):
            for round_index, current in enumerate(_round_plan(len(ids), max_new_tokens, prefill_chunk)):
                past, length = current["past"], current["length"]
                end = past + length
                inputs = ids[past:end] if current["name"].startswith("prefill-") else [tokens[-1]]
                captured.clear()
                positions = torch.arange(past, end, dtype=torch.int64)
                tick = time.perf_counter()
                output = model(input_ids=torch.tensor([inputs], dtype=torch.int64),
                               attention_mask=torch.ones((1, end), dtype=torch.int64),
                               position_ids=positions[None, :], cache_position=positions,
                               past_key_values=cache, use_cache=True, return_dict=True,
                               output_attentions=False, output_hidden_states=False)
                duration = time.perf_counter() - tick
                forward_seconds += duration
                if current["name"].startswith("prefill-"):
                    prefill_seconds += duration
                else:
                    decode_seconds += duration
                tick = time.perf_counter()
                checks = []
                for layer in _CHECK_LAYERS:
                    values = {f"hidden-{layer}": captured[layer],
                              f"key-{layer}": cache.key_cache[layer][0].permute(1, 0, 2),
                              f"value-{layer}": cache.value_cache[layer][0].permute(1, 0, 2)}
                    for name, tensor in values.items():
                        desc = _save_tensor(output_dir, f"round-{round_index:04d}.{name}.bin", tensor)
                        checks.append({"name": name, **desc})
                save_seconds += time.perf_counter() - tick
                record = {"round": round_index, "name": current["name"], "kind": current["kind"],
                          "output": None, "checks": checks, "token": None}
                if current["kind"] == "token":
                    tick = time.perf_counter()
                    logits = lm_head(output.last_hidden_state[:, -1:, :])
                    if not torch.isfinite(logits).all().item():
                        raise ValueError("Qwen CPU logits 含非有限值")
                    token = int(torch.argmax(logits.reshape(-1)).item())
                    head_seconds += time.perf_counter() - tick
                    tick = time.perf_counter()
                    record["output"] = _save_tensor(output_dir,
                                                     f"round-{round_index:04d}.output.bin", logits)
                    save_seconds += time.perf_counter() - tick
                    record["token"] = token
                    tokens.append(token)
                rounds.append(record)
                if record["token"] in eos:
                    stop_reason = "eos"
                    break
    finally:
        for handle in handles:
            handle.remove()
    role_counts = {role: sum(kind == role for _, kind in raw.used)
                   for role in sorted({kind for _, kind in raw.used})}
    result = {
        "schema_version": 1, "model": "qwen", "repository": manifest["models"]["qwen"]["repository"],
        "revision": manifest["models"]["qwen"]["revision"], "config": config,
        "precision": "original-fp32" if original_fp32 else "full-w8-fp32-activation-kv",
        "manifest_path": str(Path(manifest_path).resolve()), "prompt_ids": ids,
        "prompt_tokens": len(ids), "max_new_tokens": max_new_tokens, "prefill_chunk": prefill_chunk,
        "eos": eos, "rounds": rounds, "tokens": tokens, "text": decode_tokens(tokens),
        "stop_reason": stop_reason, "generation_policy": "greedy_argmax_first_index",
        "official_generation_config": generation,
        "weights": {"named_tensors": len({name for name, _ in raw.used}), "roles": role_counts,
                    "tied_lm_head": "model.embed_tokens.weight", "linear_rows_per_chunk": _LINEAR_ROWS},
        "implementation": {"torch": torch.__version__, "transformers": transformers.__version__,
                           "attention": "Qwen2Model/eager", "autocast": False},
        "timings": {"model_prepare_seconds": prepare_seconds, "forward_seconds": forward_seconds,
                    "prefill_seconds": prefill_seconds, "decode_seconds": decode_seconds,
                    "lm_head_seconds": head_seconds, "save_seconds": save_seconds,
                    "total_seconds": time.perf_counter() - started},
        "resources": {"peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                      "torch_threads": torch.get_num_threads(), "kv_tokens": int(cache.get_seq_length())},
        "boundary": "独立 CPU 全网络与连续 KV；输出只用于对照，不注入设备输入或 KV",
    }
    del model, lm_head, cache, raw
    return result


def cpu_reference(manifest_path, ids, output_dir, max_new_tokens=16, prefill_chunk=128,
                  original_fp32=False):
    """从完整 W8 prefill 连续生成；可串行另算原 FP32，不复用两者的 KV。"""
    _material()
    import torch

    manifest_path = Path(manifest_path).resolve()
    # W8 与原 FP32 使用同一份已解析清单的摘要，不按后来的路径内容重新签名。
    manifest_data = manifest_path.read_bytes()
    manifest_sha256 = hashlib.sha256(manifest_data).hexdigest()
    manifest = json.loads(manifest_data)
    config = _configuration(manifest["models"]["qwen"])
    ids = _valid_request(ids, max_new_tokens, _CAPACITY_LIMIT, prefill_chunk, config["vocab_size"])
    output_dir = Path(output_dir).resolve()
    previous_threads = torch.get_num_threads()
    threads = manifest.get("environment", {}).get("threads", previous_threads)
    torch.set_num_threads(threads)
    try:
        result = _reference_run(manifest_path, manifest, ids, output_dir, max_new_tokens,
                                prefill_chunk, config, False)
        result["manifest_sha256"] = manifest_sha256
        (output_dir / "metadata.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        if original_fp32:
            # W8 实例及映射先释放；原 FP32 参考使用独立模型、独立 KV 和独立目录。
            gc.collect()
            original_dir = output_dir / "original-fp32"
            reference = _reference_run(manifest_path, manifest, ids, original_dir, max_new_tokens,
                                       prefill_chunk, config, True)
            reference["manifest_sha256"] = manifest_sha256
            (original_dir / "metadata.json").write_text(
                json.dumps(reference, ensure_ascii=False, indent=2) + "\n")
            result["original_fp32"] = {"metadata_path": "original-fp32/metadata.json",
                                       "tokens": reference["tokens"], "text": reference["text"],
                                       "stop_reason": reference["stop_reason"],
                                       "timings": reference["timings"]}
            (output_dir / "metadata.json").write_text(
                json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        return result
    finally:
        torch.set_num_threads(previous_threads)
