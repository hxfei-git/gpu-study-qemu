# SPDX-License-Identifier: GPL-2.0-or-later
"""模型材料：以真实 CPU 前向导出逐算子回放数据和完整调用清单。

本入口不下载、不安装依赖，也不执行下载目录中的 Python。export 只接受固定材料，
用独立 CPU 环境运行；inventory 只读取已经导出的清单，不重新生成参考数据。
"""

import argparse
import contextlib
import gc
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import pickletools
import platform
import resource
import math
import re
import shlex
import shutil
import statistics
import struct
import subprocess
import sys
import time
import traceback
import types
import zipfile


ROOT = Path(__file__).resolve().parents[3]
EVAL_ROOT = ROOT / "build/gpgpu-model-eval"
QWEN_REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
QWEN_WEIGHT_SHA256 = "fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"
QWEN_FILES = {"LICENSE", "config.json", "generation_config.json", "merges.txt",
              "model.safetensors", "tokenizer.json", "tokenizer_config.json", "vocab.json"}
YOLO_SHA256 = "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1"
DEPENDENCIES = {
    "torch": "2.6.0+cpu",
    "torchvision": "0.21.0+cpu",
    "transformers": "4.48.3",
    "tokenizers": "0.21.0",
    "safetensors": "0.5.3",
    "ultralytics": "8.3.9",
    "numpy": "1.26.4",
    "Pillow": "11.1.0",
}
# 固定 checkpoint 的静态 GLOBAL 清单；不允许根据反序列化报错自动扩大范围。
YOLO_OBJECT_GLOBALS = {
    "torch.nn.modules.activation.SiLU",
    "torch.nn.modules.batchnorm.BatchNorm2d",
    "torch.nn.modules.container.ModuleList",
    "torch.nn.modules.container.Sequential",
    "torch.nn.modules.conv.Conv2d",
    "torch.nn.modules.linear.Identity",
    "torch.nn.modules.pooling.MaxPool2d",
    "torch.nn.modules.upsampling.Upsample",
    "ultralytics.nn.modules.block.Attention",
    "ultralytics.nn.modules.block.Bottleneck",
    "ultralytics.nn.modules.block.C2PSA",
    "ultralytics.nn.modules.block.C3k",
    "ultralytics.nn.modules.block.C3k2",
    "ultralytics.nn.modules.block.DFL",
    "ultralytics.nn.modules.block.PSABlock",
    "ultralytics.nn.modules.block.SPPF",
    "ultralytics.nn.modules.conv.Concat",
    "ultralytics.nn.modules.conv.Conv",
    "ultralytics.nn.modules.conv.DWConv",
    "ultralytics.nn.modules.head.Detect",
    "ultralytics.nn.tasks.DetectionModel",
}
YOLO_TENSOR_GLOBALS = {
    "__builtin__.set", "collections.OrderedDict", "torch.FloatStorage",
    "torch.HalfStorage", "torch.LongStorage", "torch.Size",
    "torch._utils._rebuild_parameter", "torch._utils._rebuild_tensor_v2",
}
METADATA_OPS = {
    "view", "_unsafe_view", "reshape", "transpose", "t", "permute", "expand",
    "unsqueeze", "squeeze", "slice", "select", "detach", "alias", "as_strided",
    "split", "split_with_sizes", "unbind", "lift_fresh",
    "empty", "empty_like", "empty_strided", "new_empty", "new_empty_strided",
    "_local_scalar_dense", "is_nonzero",
}


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def json_value(value):
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, (tuple, list)):
        return [json_value(x) for x in value]
    if isinstance(value, dict):
        return {str(k): json_value(v) for k, v in value.items()}
    return str(value)


def tensor_meta(tensor):
    return {
        "shape": list(tensor.shape), "dtype": str(tensor.dtype).removeprefix("torch."),
        "source_stride": list(tensor.stride()), "layout": "C",
    }


def tensor_items(value, name):
    if isinstance(value, torch.Tensor):
        return [(name, value)]
    if isinstance(value, (tuple, list)):
        return [item for i, v in enumerate(value) for item in tensor_items(v, f"{name}.{i}")]
    if isinstance(value, dict):
        return [item for k, v in value.items() for item in tensor_items(v, f"{name}.{k}")]
    return []


def dtype_code(tensor):
    codes = {torch.float32: "<f4", torch.float64: "<f8", torch.int64: "<i8",
             torch.int32: "<i4", torch.int16: "<i2", torch.int8: "i1",
             torch.uint8: "u1", torch.bool: "?"}
    if tensor.dtype not in codes:
        raise ValueError(f"不能导出未约定的 dtype: {tensor.dtype}")
    return codes[tensor.dtype]


class RawWriter:
    """按内容去重；文件始终是紧凑 C 顺序、小端，无文件头。"""

    def __init__(self, output):
        self.output = output
        (output / "raw").mkdir()
        self.saved = {}

    def tensor(self, tensor, name=None):
        value = tensor.detach().cpu().contiguous()
        array = value.numpy().astype(np.dtype(dtype_code(value)), copy=False)
        data = memoryview(array).cast("B")
        digest = hashlib.sha256(data).hexdigest()
        path = self.output / "raw" / (digest + ".bin")
        if not path.exists():
            with path.open("xb") as stream:
                stream.write(data)
        result = {"path": str(path.relative_to(self.output)), **tensor_meta(tensor),
                  "byte_order": "little", "sha256": digest, "bytes": len(data)}
        if name is not None:
            result["name"] = name
        self.saved[digest] = len(data)
        return result

    def finish_stream(self, temporary, shape, dtype):
        digest = sha256(temporary)
        path = self.output / "raw" / (digest + ".bin")
        if path.exists():
            temporary.unlink()
        else:
            temporary.rename(path)
        size = path.stat().st_size
        self.saved[digest] = size
        stride = []
        step = 1
        for dim in reversed(shape):
            stride.insert(0, step)
            step *= dim
        return {"path": str(path.relative_to(self.output)), "shape": list(shape),
                "dtype": dtype, "source_stride": stride, "layout": "C",
                "byte_order": "little", "sha256": digest, "bytes": size}


class WeightStore:
    """只保存描述符，不持有各层 tensor；逐行块量化，避免复制整个大矩阵。"""

    def __init__(self, writer):
        self.writer = writer
        self.entries = {}

    def fp32(self, name, tensor):
        if name not in self.entries:
            self.entries[name] = {"weight": self.writer.tensor(tensor.float(), "weight")}
        return self.entries[name]["weight"]

    def quantized(self, name, tensor):
        if name in self.entries and "q" in self.entries[name]:
            return self.entries[name]
        if tensor.ndim != 2 or tensor.dtype != torch.float32:
            raise ValueError("INT8 权重必须是 FP32 的 [N,K] 矩阵")
        original = self.writer.tensor(tensor, "weight")
        temporary = self.writer.output / "raw" / f"q-{len(self.entries)}.part"
        scales = torch.empty(tensor.shape[0], dtype=torch.float32)
        zero_rows = 0
        with temporary.open("xb") as stream:
            for start in range(0, tensor.shape[0], 2048):
                block = tensor[start:start + 2048]
                maximum = block.abs().amax(dim=1)
                # 零行的 scale 固定为 1，q 全 0，解量化后仍精确为 0。
                scale = torch.where(maximum == 0, torch.ones_like(maximum), maximum / 127)
                quantized = torch.round(block / scale[:, None]).clamp(-127, 127).to(torch.int8)
                stream.write(memoryview(quantized.numpy()).cast("B"))
                scales[start:start + block.shape[0]] = scale
                zero_rows += int((maximum == 0).sum())
        entry = {"weight": original,
                 "q": self.writer.finish_stream(temporary, tensor.shape, "int8"),
                 "scales": self.writer.tensor(scales, "scales"),
                 "zero_rows": zero_rows, "weight_layout": "NK"}
        entry["q"]["name"] = "q"
        self.entries[name] = entry
        return entry

    def q_reference(self, entry, value, bias=None, embedding=False):
        q = np.memmap(self.writer.output / entry["q"]["path"], dtype=np.int8,
                      mode="r", shape=tuple(entry["q"]["shape"]))
        scale = np.memmap(self.writer.output / entry["scales"]["path"], dtype="<f4",
                          mode="r", shape=tuple(entry["scales"]["shape"]))
        if embedding:
            indices = value.numpy()
            result = torch.from_numpy(np.array(q[indices], dtype=np.float32))
            result *= torch.from_numpy(np.array(scale[indices]))[..., None]
            return result
        output = torch.empty((*value.shape[:-1], q.shape[0]), dtype=torch.float32)
        for start in range(0, q.shape[0], 2048):
            weight = torch.from_numpy(np.array(q[start:start + 2048], dtype=np.float32))
            weight *= torch.from_numpy(np.array(scale[start:start + 2048]))[:, None]
            b = bias[start:start + weight.shape[0]] if bias is not None else None
            output[..., start:start + weight.shape[0]] = F.linear(value, weight, b)
        return output


class Collector:
    """语义算子用于回放；ATen 明细独立覆盖 functional 调用，防止只看 module 漏项。"""

    def __init__(self, writer, weights):
        self.writer = writer
        self.weights = weights
        self.model = ""
        self.scenario = ""
        self.enabled = False
        self.pause_depth = 0
        self.scope = []
        self.atomic = []
        self.cases = {}
        self.invocations = []
        self.aten = {}
        self.aten_invocations = []

    @property
    def active(self):
        return self.enabled and not self.pause_depth

    @contextlib.contextmanager
    def paused(self):
        self.pause_depth += 1
        try:
            yield
        finally:
            self.pause_depth -= 1

    @contextlib.contextmanager
    def scenario_region(self, model, scenario):
        self.model, self.scenario, self.enabled = model, scenario, True
        try:
            yield
        finally:
            self.enabled = False

    @property
    def source(self):
        return self.scope[-1] if self.scope else self.model

    def key(self, op, inputs, output, attrs, weights=None):
        signature = {"model": self.model, "scenario": self.scenario, "op": op,
                     "inputs": [{"name": n, **tensor_meta(t)} for n, t in inputs],
                     "outputs": [tensor_meta(t) for _, t in tensor_items(output, "output")],
                     "attrs": json_value(attrs),
                     "weights": {n: {k: d[k] for k in ("shape", "dtype")}
                                 for n, d in (weights or {}).items()}}
        digest = hashlib.sha256(json.dumps(signature, sort_keys=True).encode()).hexdigest()[:16]
        return f"{self.model}-{self.scenario}-{op}-{digest}", signature

    def record(self, op, inputs, output, attrs, weights=None, quantized=None,
               metadata_only=False, pre_saved=None):
        if not self.active:
            return None
        case_id, signature = self.key(op, inputs, output, attrs, weights)
        source = self.source
        with self.paused():
            if case_id not in self.cases:
                outputs = tensor_items(output, "output")
                case = {"id": case_id, **signature, "count": 0, "source_names": [],
                        "metadata_only": metadata_only, "reference_only": False}
                if not metadata_only:
                    case["inputs"] = pre_saved or [self.writer.tensor(t, n) for n, t in inputs]
                    actual = outputs[0][1] if outputs else None
                    if quantized is not None:
                        entry, bias, embedding = quantized
                        reference = self.weights.q_reference(entry, inputs[0][1], bias, embedding)
                        case["output"] = self.writer.tensor(reference)
                        case["fp32_reference"] = self.writer.tensor(actual)
                        error = reference - actual
                        norm = float(torch.linalg.vector_norm(actual))
                        case["quantization_error"] = {
                            "max_abs": float(error.abs().max()),
                            "rmse": float(torch.sqrt(torch.mean(error.square()))),
                            "relative_l2": float(torch.linalg.vector_norm(error)) / norm if norm else 0.0,
                            "scope": "相同真实 FP32 输入上的独立权重量化误差，不代表全量化模型质量",
                        }
                        if op == "linear_w8":
                            original_id = case_id.replace("-linear_w8-", "-linear_f32-")
                            original = {**case, "id": original_id, "op": "linear_f32",
                                        "output": case["fp32_reference"], "reference_only": True,
                                        "weights": {"weight": entry["weight"]},
                                        "source_names": [source]}
                            if bias is not None:
                                original["weights"]["bias"] = weights["bias"]
                            self.cases[original_id] = original
                    elif actual is not None:
                        case["output"] = self.writer.tensor(actual)
                    if len(outputs) > 1:
                        case["extra_outputs"] = [self.writer.tensor(t, n) for n, t in outputs[1:]]
                    case["weights"] = weights or {}
                else:
                    case["output"] = tensor_meta(outputs[0][1]) if outputs else {"value": json_value(output)}
                self.cases[case_id] = case
            case = self.cases[case_id]
            case["count"] += 1
            if source not in case["source_names"]:
                case["source_names"].append(source)
            self.invocations.append({"index": len(self.invocations), "case_id": case_id,
                                     "model": self.model, "scenario": self.scenario,
                                     "source_name": source, "op": op,
                                     "metadata_only": metadata_only})
        return case_id

    def dispatch(self, func, args, kwargs):
        if not self.active:
            return func(*args, **kwargs)
        inputs, attrs = [], {}
        for i, argument in enumerate(func._schema.arguments):
            value = args[i] if i < len(args) else kwargs.get(argument.name, argument.default_value)
            found = tensor_items(value, argument.name)
            if found:
                inputs.extend(found)
            else:
                attrs[argument.name] = json_value(value)
        base = func._schema.name.split("::")[-1]
        metadata = base in METADATA_OPS
        # 原地算子必须在执行前保存输入；ATen handler 内部不会再次进入本 mode。
        before = None
        if not metadata and not self.atomic and base.endswith("_"):
            before = [self.writer.tensor(t, n) for n, t in inputs]
        output = func(*args, **kwargs)
        raw = {"aten": str(func), "inputs": [{"name": n, **tensor_meta(t)} for n, t in inputs],
               "outputs": [{"name": n, **tensor_meta(t)} for n, t in tensor_items(output, "output")],
               "attrs": attrs, "contained_by": self.atomic[0] if self.atomic else None}
        raw_id = hashlib.sha256(json.dumps(raw, sort_keys=True).encode()).hexdigest()[:16]
        if raw_id not in self.aten:
            self.aten[raw_id] = {"id": raw_id, **raw, "count": 0, "source_names": [],
                                 "case_ids": [], "metadata_only": metadata}
        detail = self.aten[raw_id]
        detail["count"] += 1
        if self.source not in detail["source_names"]:
            detail["source_names"].append(self.source)
        self.aten_invocations.append({"index": len(self.aten_invocations), "signature_id": raw_id,
                                      "model": self.model, "scenario": self.scenario,
                                      "source_name": self.source})
        if not self.atomic:
            op = {"cat": "concat", "clone": "copy", "copy_": "copy", "_to_copy": "copy",
                  "_softmax": "softmax", "upsample_nearest2d": "nearest"}.get(base, base)
            case_id = self.record(op, inputs, output, {"aten": str(func), **attrs},
                                  metadata_only=metadata or not tensor_items(output, "output"),
                                  pre_saved=before)
            if case_id not in detail["case_ids"]:
                detail["case_ids"].append(case_id)
        return output


def dispatch_mode(collector):
    from torch.utils._python_dispatch import TorchDispatchMode

    class ExportMode(TorchDispatchMode):
        def __torch_dispatch__(self, func, types, args=(), kwargs=None):
            return collector.dispatch(func, args, kwargs or {})

    return ExportMode()


class ModuleHooks:
    """叶子数值模块合并为设备算子；其内部 ATen 仍进入完整性清单。"""

    def __init__(self, collector, module, prefix):
        self.collector = collector
        self.handles = []
        self.states = {}
        self.paths = {}
        for name, child in module.named_modules(remove_duplicate=False):
            source = prefix + ("." + name if name else "")
            self.paths.setdefault(id(child), []).append(source)
        for _, child in module.named_modules():
            self.handles.append(child.register_forward_pre_hook(self.pre(self.paths[id(child)])))
            self.handles.append(child.register_forward_hook(self.post()))

    def kind(self, module):
        if isinstance(module, torch.nn.Linear):
            return "linear_w8" if self.collector.model == "qwen" else "linear_f32"
        if isinstance(module, torch.nn.Embedding):
            return "embedding"
        if isinstance(module, torch.nn.Conv2d):
            return "dwconv" if module.groups == module.in_channels and module.groups > 1 else "conv2d"
        if isinstance(module, torch.nn.BatchNorm2d):
            return "batchnorm"
        if isinstance(module, torch.nn.SiLU):
            return "silu"
        if isinstance(module, torch.nn.MaxPool2d):
            return "maxpool"
        if isinstance(module, torch.nn.Upsample):
            return "nearest"
        if type(module).__name__ == "Qwen2RMSNorm":
            return "rmsnorm"
        if type(module).__name__ == "DFL":
            return "dfl"
        return None

    def pre(self, paths):
        def hook(module, args):
            c = self.collector
            parent = c.scope[-1] if c.scope else ""
            source = next((p for p in paths if p.rpartition(".")[0] == parent), paths[0])
            c.scope.append(source)
            op = self.kind(module)
            root = op is not None and not c.atomic and c.active
            saved = args
            if root and getattr(module, "inplace", False):
                with c.paused():
                    saved = tuple(a.clone() if isinstance(a, torch.Tensor) else a for a in args)
            self.states.setdefault(id(module), []).append((op, root, saved))
            if op is not None:
                c.atomic.append(op)
        return hook

    def post(self):
        def hook(module, args, output):
            c = self.collector
            source = c.source
            op, root, saved = self.states[id(module)].pop()
            if op is not None:
                c.atomic.pop()
            if root:
                with c.paused():
                    attrs, weights, quantized = {}, {}, None
                    value = saved[0]
                    if op in {"linear_w8", "embedding"} and c.model == "qwen":
                        weight_name = "model.embed_tokens.weight" if source == "lm_head" else source + ".weight"
                        entry = c.weights.quantized(weight_name, module.weight)
                        weights = {"q": entry["q"], "scales": entry["scales"]}
                        bias = getattr(module, "bias", None)
                        if bias is not None:
                            weights["bias"] = c.weights.fp32(source + ".bias", bias)
                        attrs = {"weight_layout": "NK", "quantization": "symmetric_per_output_row",
                                 "rounding": "ties_to_even", "qmin": -127, "qmax": 127,
                                 "zero_row_scale": 1.0, "accumulation": "float32"}
                        quantized = (entry, bias, op == "embedding")
                    elif op == "linear_f32":
                        weights["weight"] = c.weights.fp32(source + ".weight", module.weight)
                        if module.bias is not None:
                            weights["bias"] = c.weights.fp32(source + ".bias", module.bias)
                        attrs["weight_layout"] = "NK"
                    elif op in {"conv2d", "dwconv"}:
                        weights["weight"] = c.weights.fp32(source + ".weight", module.weight)
                        if module.bias is not None:
                            weights["bias"] = c.weights.fp32(source + ".bias", module.bias)
                        attrs = {"stride": list(module.stride), "padding": list(module.padding),
                                 "dilation": list(module.dilation), "groups": module.groups,
                                 "input_layout": "NCHW", "weight_layout": "OIHW"}
                    elif op == "batchnorm":
                        for key, tensor in (("weight", module.weight), ("bias", module.bias),
                                            ("running_mean", module.running_mean),
                                            ("running_var", module.running_var)):
                            weights[key] = c.weights.fp32(source + "." + key, tensor)
                        attrs = {"eps": module.eps, "training": False, "axis": 1}
                    elif op == "rmsnorm":
                        weights["weight"] = c.weights.fp32(source + ".weight", module.weight)
                        attrs = {"eps": module.variance_epsilon, "axis": -1}
                    elif op == "maxpool":
                        attrs = {k: json_value(getattr(module, k)) for k in
                                 ("kernel_size", "stride", "padding", "dilation", "ceil_mode")}
                    elif op == "nearest":
                        attrs = {"mode": module.mode, "scale_factor": json_value(module.scale_factor),
                                 "size": json_value(module.size), "output_size": list(output.shape[-2:])}
                    elif op == "dfl":
                        attrs = {"reg_max": module.c1, "distribution_axis": 1,
                                 "input_layout": "B(4*reg_max)A", "output_layout": "B4A"}
                        weights["projection"] = c.weights.fp32(source + ".conv.weight", module.conv.weight)
                    elif op == "silu":
                        attrs = {"inplace": getattr(module, "inplace", False)}
                c.record(op, [("x", value)], output, attrs, weights, quantized)
            c.scope.pop()
        return hook

    def close(self):
        for handle in self.handles:
            handle.remove()


@contextlib.contextmanager
def qwen_rope_hook(collector, modeling):
    original = modeling.apply_rotary_pos_emb

    def apply(q, k, cos, sin, position_ids=None, unsqueeze_dim=1):
        active = collector.active and not collector.atomic
        collector.atomic.append("rope")
        try:
            result = original(q, k, cos, sin, position_ids, unsqueeze_dim)
        finally:
            collector.atomic.pop()
        if active:
            for value, output in zip((q, k), result):
                collector.record("rope", [("x", value), ("cos", cos), ("sin", sin)], output,
                                 {"unsqueeze_dim": unsqueeze_dim, "rotary_dim": value.shape[-1],
                                  "rotation": "split_half", "theta": 1000000.0})
        return result

    modeling.apply_rotary_pos_emb = apply
    try:
        yield
    finally:
        modeling.apply_rotary_pos_emb = original


def package_record(name, files):
    dist = importlib.metadata.distribution(name)
    return {"version": dist.version, "files": [
        {"path": str(dist.locate_file(f)), "sha256": sha256(dist.locate_file(f))} for f in files]}


def export_qwen(args, writer, collector, manifest):
    from safetensors import safe_open
    from transformers import AutoTokenizer, Qwen2Config
    from transformers.cache_utils import DynamicCache
    from transformers.models.qwen2 import modeling_qwen2 as modeling

    material = args.qwen_dir.resolve()
    provenance = json.loads((material / "provenance.json").read_text())
    if provenance["repository"] != "Qwen/Qwen2.5-0.5B-Instruct" or provenance["revision"] != QWEN_REVISION:
        raise ValueError("Qwen 材料并非授权的官方固定提交")
    if {record["file"] for record in provenance["files"]} != QWEN_FILES:
        raise ValueError("Qwen 材料文件清单不符；不读取清单以外的路径")
    for record in provenance["files"]:
        digest = sha256(material / record["file"])
        if digest != record["sha256"] or (record["file"] == "model.safetensors" and digest != QWEN_WEIGHT_SHA256):
            raise ValueError(f"Qwen 材料哈希不符: {record['file']}")
    config = Qwen2Config.from_dict(json.loads((material / "config.json").read_text()))
    expected = {"num_hidden_layers": 24, "hidden_size": 896, "intermediate_size": 4864,
                "num_attention_heads": 14, "num_key_value_heads": 2, "vocab_size": 151936,
                "rope_theta": 1000000.0, "rms_norm_eps": 1e-6, "tie_word_embeddings": True}
    for key, value in expected.items():
        if getattr(config, key) != value:
            raise ValueError(f"Qwen 配置不符: {key}")
    config._attn_implementation = "eager"
    tokenizer = AutoTokenizer.from_pretrained(str(material), local_files_only=True,
                                             trust_remote_code=False, token=False)
    text_bytes = args.text.read_bytes()
    text = text_bytes.decode("utf-8-sig").replace("\r\n", "\n")
    marker = "CHAPTER I.\nDown the Rabbit-Hole"
    if marker in text:
        text = text[text.index(marker):]
    all_ids = tokenizer(text, add_special_tokens=False, return_tensors="pt")["input_ids"]
    required = max([args.prefill_length, *(v + 1 for v in args.decode_contexts)])
    if all_ids.shape[1] < required:
        raise ValueError("真实文本 token 不足；不允许随机补齐或循环输入")
    selected_text = tokenizer.decode(all_ids[0, :required], skip_special_tokens=False)
    (writer.output / "qwen-input.txt").write_text(selected_text)
    info = {"repository": provenance["repository"], "revision": QWEN_REVISION,
            "materials": provenance["files"], "config": expected,
            "implementation": package_record("transformers", ["transformers/models/qwen2/modeling_qwen2.py",
                                                               "transformers/cache_utils.py"]),
            "text": {"path": str(args.text), "sha256": hashlib.sha256(text_bytes).hexdigest(),
                     "provenance": json.loads((args.text.parent / "provenance.json").read_text()),
                     "selected_path": "qwen-input.txt", "add_special_tokens": False,
                     "tokenizer": "官方本地 tokenizer，未使用 chat template"},
            "precision": "官方 BF16 权重逐层转 FP32；eval/no_grad，无 autocast，eager attention",
            "reference_boundary": "完整原权重 FP32 网络产生真实输入；W8 为各算子独立量化参考，未运行全量化模型",
            "host_setup": "文本编码、causal mask 与 cache_position 的构造不列入设备调用；RoPE 因子计算列入",
            "scenarios": {}, "weights": {}}
    manifest["models"]["qwen"] = info
    scenarios = [(f"prefill{args.prefill_length}", args.prefill_length, False)] + [
        (f"decode{v}", v, True) for v in args.decode_contexts]
    with safe_open(material / "model.safetensors", framework="pt", device="cpu") as source:
        embedding_weight = source.get_tensor("model.embed_tokens.weight").float()
        collector.weights.quantized("model.embed_tokens.weight", embedding_weight)
        with torch.device("meta"):
            embedding = torch.nn.Embedding(config.vocab_size, config.hidden_size)
        embedding.weight = torch.nn.Parameter(embedding_weight, requires_grad=False)
        embedding.eval()
        rotary = modeling.Qwen2RotaryEmbedding(config=config, device="cpu").eval()
        for scenario, context, decode in scenarios:
            started = time.monotonic()
            count = context + int(decode)
            ids = all_ids[:, :count].contiguous()
            prefix_ids = ids[:, :context]
            query_ids = ids[:, context:] if decode else ids
            collector.model = "qwen"
            emb_hooks = ModuleHooks(collector, embedding, "model.embed_tokens")
            rotary_hooks = ModuleHooks(collector, rotary, "model.rotary_emb")
            with dispatch_mode(collector), collector.scenario_region("qwen", scenario), qwen_rope_hook(collector, modeling):
                if decode:
                    with collector.paused():
                        prefix_hidden = embedding(prefix_ids)
                    query_hidden = embedding(query_ids)
                else:
                    prefix_hidden = None
                    query_hidden = embedding(query_ids)
                with collector.paused():
                    positions = torch.arange(count, dtype=torch.int64)
                    query_positions = positions[context:] if decode else positions
                    query_mask = modeling.Qwen2Model._prepare_4d_causal_attention_mask_with_cache_position(
                        None, sequence_length=query_ids.shape[1], target_length=count + 1,
                        dtype=torch.float32, device="cpu", cache_position=query_positions, batch_size=1)
                    prefix_positions = positions[:context]
                    prefix_mask = modeling.Qwen2Model._prepare_4d_causal_attention_mask_with_cache_position(
                        None, sequence_length=context, target_length=context + 1, dtype=torch.float32,
                        device="cpu", cache_position=prefix_positions, batch_size=1)
                    prefix_rope = rotary(prefix_hidden, prefix_positions[None]) if decode else None
                query_rope = rotary(query_hidden, query_positions[None])
                with collector.paused():
                    scenario_info = {"input_ids": writer.tensor(ids, "input_ids"),
                                     "query_ids": writer.tensor(query_ids, "query_ids"),
                                     "context_length": context if decode else 0,
                                     "query_length": query_ids.shape[1],
                                     "attention_mask": writer.tensor(query_mask),
                                     "layers": [], "kv": []}
                    info["scenarios"][scenario] = scenario_info
                for layer_index in range(config.num_hidden_layers):
                    prefix = f"model.layers.{layer_index}."
                    with collector.paused():
                        with torch.device("meta"):
                            layer = modeling.Qwen2DecoderLayer(config, layer_index)
                        state = {k.removeprefix(prefix): source.get_tensor(k).float()
                                 for k in source.keys() if k.startswith(prefix)}
                        layer.load_state_dict(state, strict=True, assign=True)
                        layer.eval()
                        for name, child in layer.named_modules():
                            if isinstance(child, torch.nn.Linear):
                                collector.weights.quantized(prefix + name + ".weight", child.weight)
                        del state
                        cache = DynamicCache()
                    hooks = ModuleHooks(collector, layer, prefix[:-1])
                    if decode:
                        with collector.paused():
                            prefix_hidden = layer(prefix_hidden, attention_mask=prefix_mask,
                                                  past_key_value=cache, use_cache=True,
                                                  cache_position=prefix_positions,
                                                  position_embeddings=prefix_rope)[0]
                            kv = {"layer": layer_index,
                                  "key_before": writer.tensor(cache.key_cache[layer_index]),
                                  "value_before": writer.tensor(cache.value_cache[layer_index])}
                    else:
                        kv = {"layer": layer_index}
                    query_hidden = layer(query_hidden, attention_mask=query_mask,
                                         past_key_value=cache, use_cache=True,
                                         cache_position=query_positions, position_embeddings=query_rope)[0]
                    with collector.paused():
                        kv["key_after"] = writer.tensor(cache.key_cache[layer_index])
                        kv["value_after"] = writer.tensor(cache.value_cache[layer_index])
                        scenario_info["kv"].append(kv)
                        if layer_index in (0, config.num_hidden_layers // 2, config.num_hidden_layers - 1):
                            scenario_info["layers"].append({"layer": layer_index,
                                                           "hidden": writer.tensor(query_hidden)})
                    hooks.close()
                    del layer, cache
                    gc.collect()
                    print(f"qwen {scenario} layer {layer_index + 1}/{config.num_hidden_layers}", flush=True)
                with collector.paused():
                    with torch.device("meta"):
                        norm = modeling.Qwen2RMSNorm(config.hidden_size, config.rms_norm_eps)
                    norm.load_state_dict({"weight": source.get_tensor("model.norm.weight").float()},
                                         strict=True, assign=True)
                    norm.eval()
                norm_hooks = ModuleHooks(collector, norm, "model.norm")
                query_hidden = norm(query_hidden)
                norm_hooks.close()
                # 只计算最后一个位置，避免 prefill 的 [T,vocab] logits。
                with collector.paused():
                    with torch.device("meta"):
                        head = torch.nn.Linear(config.hidden_size, config.vocab_size, bias=False)
                    head.weight = embedding.weight
                    head.eval()
                head_hooks = ModuleHooks(collector, head, "lm_head")
                logits = head(query_hidden[:, -1:, :])
                head_hooks.close()
                with collector.paused():
                    scenario_info["last_hidden"] = writer.tensor(query_hidden[:, -1:, :])
                    scenario_info["last_logits_fp32"] = writer.tensor(logits)
                    scenario_info["argmax_token"] = int(logits[0, 0].argmax())
                    scenario_info["seconds"] = time.monotonic() - started
                del head, norm, query_hidden, prefix_hidden, logits
            emb_hooks.close()
            rotary_hooks.close()
            gc.collect()
    info["weights"] = {name: entry for name, entry in collector.weights.entries.items()
                       if name.startswith("model.")}
    info["tied_weights"] = {"lm_head.weight": "model.embed_tokens.weight"}


class TensorShell:
    """旧 checkpoint 的惰性容器；不调用任何 nn/Ultralytics 类或 forward。"""

    def __setstate__(self, state):
        if not isinstance(state, dict):
            raise ValueError("checkpoint 对象状态不是字典")
        self.__dict__.update(state)


def yolo_state(path):
    if sha256(path) != YOLO_SHA256:
        raise ValueError("YOLO checkpoint 与授权的 SHA256 不符")
    with zipfile.ZipFile(path) as archive:
        pickles = [n for n in archive.namelist() if n.endswith("/data.pkl")]
        if len(pickles) != 1:
            raise ValueError("checkpoint 缺少唯一 data.pkl")
        globals_found = set()
        for opcode, argument, _ in pickletools.genops(archive.read(pickles[0])):
            if opcode.name == "STACK_GLOBAL":
                raise ValueError("固定 checkpoint 不应包含 STACK_GLOBAL")
            if opcode.name == "GLOBAL":
                globals_found.add(argument.replace(" ", "."))
        if globals_found != YOLO_OBJECT_GLOBALS | YOLO_TENSOR_GLOBALS:
            raise ValueError(f"checkpoint 全局符号发生变化: {sorted(globals_found)}")
    allowlist = [(TensorShell, name) for name in sorted(YOLO_OBJECT_GLOBALS)] + [set, torch.Size]
    # 不回退 weights_only=False，不让 torch 导入 checkpoint 中给出的模块。
    with torch.serialization.safe_globals(allowlist):
        checkpoint = torch.load(path, map_location="cpu", weights_only=True)
    shell = checkpoint.get("ema") or checkpoint["model"]
    if not isinstance(shell, TensorShell):
        raise ValueError("checkpoint model 不是审查过的惰性容器")
    state = {}

    def walk(node, prefix):
        if not isinstance(node, TensorShell):
            raise ValueError("checkpoint 模块树出现未知对象")
        for group in ("_parameters", "_buffers"):
            for name, tensor in node.__dict__.get(group, {}).items():
                if tensor is not None:
                    if not isinstance(tensor, torch.Tensor):
                        raise ValueError("checkpoint 参数不是 tensor")
                    state[prefix + name] = tensor.detach().float() if tensor.is_floating_point() else tensor.detach()
        for name, child in node.__dict__.get("_modules", {}).items():
            if child is not None:
                walk(child, prefix + name + ".")

    walk(shell, "")
    metadata = {"checkpoint_version": checkpoint.get("version"),
                "checkpoint_sha256": YOLO_SHA256, "tensor_count": len(state),
                "loading": "静态 GLOBAL 审查 + weights_only=True + TensorShell；只提取 tensor，官方新网络 strict load",
                "allowed_object_globals": sorted(YOLO_OBJECT_GLOBALS)}
    return state, metadata


def export_yolo(args, writer, collector, manifest):
    import cv2
    from PIL import Image
    import ultralytics
    from ultralytics.data.augment import LetterBox
    from ultralytics.nn.tasks import DetectionModel
    from ultralytics.utils import yaml_load

    state, checkpoint = yolo_state(args.yolo_checkpoint)
    package_root = Path(ultralytics.__file__).parent
    config_path = package_root / "cfg/models/11/yolo11.yaml"
    config = yaml_load(config_path)
    config["scale"] = "n"
    model = DetectionModel(config, ch=3, nc=80, verbose=False).float().eval()
    result = model.load_state_dict(state, strict=True)
    if result.missing_keys or result.unexpected_keys:
        raise ValueError("YOLO 官方网络 strict load 不完整")
    del state
    image = np.asarray(Image.open(args.image).convert("RGB"))
    original_shape = list(image.shape)
    letterbox = LetterBox(new_shape=(320, 320), auto=False, scaleFill=False,
                         scaleup=True, center=True, stride=32)
    resized = letterbox(image=image)
    value = torch.from_numpy(resized.copy()).permute(2, 0, 1).contiguous()[None].float() / 255.0
    if tuple(value.shape) != (1, 3, 320, 320):
        raise ValueError("letterbox 输入尺寸不符")
    info = {**checkpoint, "implementation": package_record("ultralytics", [
                "ultralytics/nn/tasks.py", "ultralytics/nn/modules/conv.py",
                "ultralytics/nn/modules/block.py", "ultralytics/nn/modules/head.py",
                "ultralytics/utils/tal.py", "ultralytics/data/augment.py",
                "ultralytics/cfg/models/11/yolo11.yaml"]),
            "source_revision": "50497218c25682458ea35b02dcc5d8a364f34591",
            "external_modified_source_imported": False, "fused_conv_bn": False,
            "input": {"image_path": str(args.image), "sha256": sha256(args.image),
                      "provenance": json.loads((args.image.parent / "provenance.json").read_text()),
                      "original_shape": original_shape, "tensor": writer.tensor(value),
                      "color": "RGB", "normalization": "float32 / 255",
                      "letterbox": {"auto": False, "shape": [320, 320], "center": True,
                                    "scale_fill": False, "scaleup": True,
                                    "resize": "OpenCV INTER_LINEAR", "border_value": 114}},
            "precision": "FP32 eval/no_grad，无 autocast、无 Conv/BN 融合",
            "host_setup": "照片解码、letterbox 与 RGB/255 在 CPU；网络记录从 1x3x320x320 开始",
            "nc": 80, "reg_max": 16, "attention": [], "scenarios": {}}
    manifest["models"]["yolo"] = info
    for name, module in model.named_modules():
        if type(module).__name__ == "Attention":
            info["attention"].append({"source_name": name, "num_heads": module.num_heads,
                                      "head_dim": module.head_dim, "key_dim": module.key_dim,
                                      "scale": module.scale})
    detect = model.model[-1]
    detect_name = f"model.{len(model.model) - 1}"
    original_decode = detect.decode_bboxes

    def decode(_self, bboxes, anchors):
        collector.atomic.append("boxdecode")
        try:
            output = original_decode(bboxes, anchors)
        finally:
            collector.atomic.pop()
        collector.record("boxdecode", [("distances", bboxes), ("anchors", anchors)], output,
                         {"axis": 1, "xywh": True, "format": "center_x,center_y,width,height"})
        return output

    detect.decode_bboxes = types.MethodType(decode, detect)
    started = time.monotonic()
    collector.model = "yolo"
    hooks = ModuleHooks(collector, model, "yolo")
    with dispatch_mode(collector), collector.scenario_region("yolo", "image320"):
        predictions, raw = model(value)
    hooks.close()
    expected = [(1, 144, 40, 40), (1, 144, 20, 20), (1, 144, 10, 10)]
    if [tuple(t.shape) for t in raw] != expected or tuple(predictions.shape) != (1, 84, 2100):
        raise ValueError(f"Detect 实际输出不符: {[tuple(t.shape) for t in raw]}, {tuple(predictions.shape)}")
    info["scenarios"]["image320"] = {"raw_heads": [writer.tensor(t) for t in raw],
                                             "pre_nms": writer.tensor(predictions),
                                             "seconds": time.monotonic() - started,
                                             "nms_executed": False}
    info["detect_source"] = "yolo." + detect_name
    info["weights"] = {name: entry for name, entry in collector.weights.entries.items()
                       if name.startswith("yolo.")}
    del model, predictions, raw
    gc.collect()


def prepare_environment():
    # 所有配置/cache 均限定在本实验目录；不查用户 token、netrc 或外部 YOLO 工程。
    cache = EVAL_ROOT / "cache"
    for name in ("huggingface", "ultralytics", "matplotlib", "xdg"):
        (cache / name).mkdir(parents=True, exist_ok=True)
    os.environ.update({"HOME": str(EVAL_ROOT / "isolated-home"),
                       "HF_HOME": str(cache / "huggingface"), "HF_HUB_OFFLINE": "1",
                       "TRANSFORMERS_OFFLINE": "1", "HF_HUB_DISABLE_IMPLICIT_TOKEN": "1",
                       "HF_HUB_DISABLE_TELEMETRY": "1", "DO_NOT_TRACK": "1",
                       "YOLO_CONFIG_DIR": str(cache / "ultralytics"), "YOLO_OFFLINE": "true",
                       "YOLO_AUTOINSTALL": "false", "YOLO_VERBOSE": "false",
                       "MPLCONFIGDIR": str(cache / "matplotlib"),
                       "XDG_CACHE_HOME": str(cache / "xdg"), "TOKENIZERS_PARALLELISM": "false"})
    for key in ("HF_TOKEN", "HUGGING_FACE_HUB_TOKEN", "HUGGINGFACE_TOKEN",
                "ULTRALYTICS_API_KEY", "WANDB_API_KEY"):
        os.environ.pop(key, None)
    for name, version in DEPENDENCIES.items():
        if importlib.metadata.version(name) != version:
            raise ValueError(f"独立环境依赖不符: {name} 应为 {version}")


def export_command(args):
    global torch, np, F
    if not sys.flags.isolated:
        raise ValueError("读取模型材料必须使用独立模式: python -I gpgpu_model_eval.py ...")
    output = args.output.resolve()
    if not output.is_relative_to(EVAL_ROOT):
        raise ValueError(f"输出只能位于 {EVAL_ROOT}")
    if args.prefill_length <= 0 or any(v <= 0 for v in args.decode_contexts):
        raise ValueError("token 长度必须为正数")
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    manifest = {"schema_version": 1, "status": "running", "models": {},
                "tensor_format": {"encoding": "rawbin", "byte_order": "little", "layout": "C",
                                  "paths": "相对 manifest.json；每个 .bin 无头、按逻辑索引紧凑排列",
                                  "source_stride": "前向中原 tensor 的 stride；不是 rawbin 的物理 stride"},
                "quantization": {"matrix_layout": "NK", "axis": 0, "q_dtype": "int8",
                                 "scale_dtype": "float32", "qmin": -127, "qmax": 127,
                                 "scale": "max(abs(weight[row])) / 127",
                                 "rounding": "ties_to_even", "zero_row": "scale=1,q=0",
                                 "activation_bias_norm_kv": "float32"},
                "coverage_policy": "invocations 为不重复计费的设备算子；aten_inventory/aten_invocations 保留全部底层实际调用；contained_by 表示已包含于语义算子，metadata_only 不搬运数据",
                "environment": {"python": platform.python_version(), "platform": platform.platform(),
                                "dependencies": DEPENDENCIES, "threads": args.threads,
                                "export_script": str(Path(__file__).resolve()),
                                "export_script_sha256": sha256(__file__)}}
    write_json(output / "manifest.json", manifest)
    collector = None
    try:
        prepare_environment()
        import torch
        import numpy as np
        import torch.nn.functional as F
        if torch.version.cuda is not None:
            raise ValueError("导出环境必须为 CPU-only torch")
        torch.set_num_threads(args.threads)
        torch.set_num_interop_threads(1)
        torch.set_grad_enabled(False)
        torch.manual_seed(0)
        writer = RawWriter(output)
        collector = Collector(writer, WeightStore(writer))
        with torch.no_grad(), torch.autocast(device_type="cpu", enabled=False):
            if args.model in ("qwen", "all"):
                export_qwen(args, writer, collector, manifest)
            if args.model in ("yolo", "all"):
                export_yolo(args, writer, collector, manifest)
        manifest.update({"status": "complete", "cases": list(collector.cases.values()),
                         "invocations": collector.invocations,
                         "aten_inventory": list(collector.aten.values()),
                         "aten_invocations": collector.aten_invocations,
                         "resources": {"seconds": time.monotonic() - started,
                                       "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                                       "raw_bytes_unique": sum(writer.saved.values())}})
        dependency_sources = EVAL_ROOT / "material/dependency-sources.json"
        if dependency_sources.exists():
            manifest["environment"]["dependency_sources"] = json.loads(dependency_sources.read_text())
        for name in ("dependencies.json", "requirements.lock", "torch-install.json", "model-install.json", "dependency-sources.json"):
            source = EVAL_ROOT / "material" / name
            if source.exists():
                manifest["environment"].setdefault("dependency_records", []).append(
                    {"path": str(source), "sha256": sha256(source)})
        write_json(output / "manifest.json", manifest)
        inventory_command(argparse.Namespace(manifest=output / "manifest.json", json=False))
    except BaseException as error:
        if collector is not None:
            manifest.update({"cases": list(collector.cases.values()),
                             "invocations": collector.invocations,
                             "aten_inventory": list(collector.aten.values()),
                             "aten_invocations": collector.aten_invocations})
        manifest["status"] = "failed"
        manifest["error"] = f"{type(error).__name__}: {error}"
        manifest["resources"] = {"seconds": time.monotonic() - started,
                                 "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
        write_json(output / "manifest.json", manifest)
        (output / "failure.log").write_text(traceback.format_exc())
        raise


def inventory_command(args):
    manifest = json.loads(args.manifest.read_text())
    if args.json:
        print(json.dumps({key: manifest.get(key) for key in
                          ("schema_version", "status", "models", "cases", "aten_inventory", "resources")},
                         ensure_ascii=False, indent=2))
        return
    print(f"status={manifest['status']} manifest={args.manifest}")
    for model in manifest.get("models", {}):
        scenarios = sorted({c["scenario"] for c in manifest.get("cases", []) if c["model"] == model})
        for scenario in scenarios:
            cases = [c for c in manifest["cases"] if c["model"] == model and c["scenario"] == scenario
                     and not c["reference_only"] and not c["metadata_only"]]
            counts = {}
            for case in cases:
                counts[case["op"]] = counts.get(case["op"], 0) + case["count"]
            print(f"{model}/{scenario}: signatures={len(cases)} calls={sum(counts.values())} "
                  + json.dumps(counts, sort_keys=True))
    if "resources" in manifest:
        print(json.dumps(manifest["resources"], ensure_ascii=False))


def add_export_subcommands(subparsers):
    parser = subparsers.add_parser("export", help="以真实 CPU 前向一次导出固定参考数据")
    parser.add_argument("--output", type=Path, required=True, help="新的空输出目录（必须在 build/gpgpu-model-eval 下）")
    parser.add_argument("--model", choices=("qwen", "yolo", "all"), default="all")
    material = EVAL_ROOT / "material"
    parser.add_argument("--qwen-dir", type=Path, default=material / ("qwen-" + QWEN_REVISION + "-complete"))
    parser.add_argument("--text", type=Path, default=material / "qwen-text/alice.txt")
    parser.add_argument("--yolo-checkpoint", type=Path, default=material / "yolo-checkpoint/yolo11n.pt")
    parser.add_argument("--image", type=Path, default=material / "yolo-input-complete/bus.jpg")
    parser.add_argument("--prefill-length", type=int, default=128)
    parser.add_argument("--decode-contexts", type=int, nargs="+", default=[128, 512, 2048])
    parser.add_argument("--threads", type=int, default=2)
    parser.set_defaults(function=export_command)
    parser = subparsers.add_parser("inventory", help="读取导出清单，不运行模型、不改参考数据")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.set_defaults(function=inventory_command)


def eval_abi():
    """按依赖顺序读取共用头的数值及别名，不执行 C 表达式。"""
    include = ROOT / "software/gpgpu/include/gpgpu"
    values = {}
    for header in ("gpgpu_tensor_abi.h", "gpgpu_eval_abi.h"):
        for name, value in re.findall(r"^#define[ \t]+(GPGPU_(?:TENSOR|EVAL)_\w+)[ \t]+(\S+)[ \t]*$",
                                      (include / header).read_text(), re.M):
            try:
                values[name] = int(value, 0)
            except ValueError:
                if value in values:
                    values[name] = values[value]
    return values


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", float(value)))[0]


def contiguous_stride(shape):
    stride, result = 1, []
    for dim in reversed(shape):
        result.append(stride)
        stride *= dim
    return list(reversed(result))


def pair(value):
    return list(value) if isinstance(value, (list, tuple)) else [value, value]


class JobWriter:
    """把冻结的逻辑 tensor 装入评估用参数；原参考数据保持只读。"""

    def __init__(self, manifest, output):
        self.root = manifest.resolve().parent
        self.output = output.resolve()
        self.abi = eval_abi()
        self.prepared = {}

    def tensor_path(self, tensor, physical=False):
        if tensor is None:
            return "-", None
        path = (self.root / tensor["path"]).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError("tensor 路径离开参考数据目录")
        shape = tensor["shape"]
        strides = tensor.get("source_stride", contiguous_stride(shape))
        contiguous = all(d <= 1 or a == b for d, a, b in
                         zip(shape, strides, contiguous_stride(shape)))
        if not physical or contiguous:
            return "/mnt/gpgpu-eval-data/" + str(path.relative_to(self.root)), None
        if any(s < 0 for s in strides):
            raise ValueError("回放器尚不支持负 stride")
        import numpy as np
        dtype = np.dtype(tensor["dtype"]).newbyteorder("<")
        span = 1 + sum((d - 1) * s for d, s in zip(shape, strides))
        key = hashlib.sha256(json.dumps([tensor["sha256"], shape, strides]).encode()).hexdigest()
        packed = self.output / "packed" / (key + ".bin")
        if not packed.exists():
            storage = np.zeros(span, dtype=dtype)
            view = np.ndarray(shape, dtype=dtype, buffer=storage,
                              strides=tuple(s * dtype.itemsize for s in strides))
            view[...] = np.fromfile(path, dtype=dtype).reshape(shape)
            packed.parent.mkdir(parents=True, exist_ok=True)
            storage.tofile(packed)
        self.prepared[key] = {"path": str(packed.relative_to(self.output)),
                              "source": tensor["path"], "source_stride": strides,
                              "shape": shape, "bytes": packed.stat().st_size,
                              "sha256": sha256(packed)}
        return "/mnt/gpgpu-eval-jobs/" + str(packed.relative_to(self.output)), strides

    def write_case(self, case):
        """只准备输入存储和维度；不计算设备算子的最终输出。"""
        abi = self.abi
        op, attrs = case["op"], case["attrs"]
        tensors, weights = case["inputs"], case.get("weights", {})
        out = case["output"]
        dims = [0] * 16
        sources = list(tensors)
        physical = set()
        name = {"linear_w8": "LINEAR_W8", "linear_f32": "LINEAR_F32",
                "embedding": "EMBEDDING_W8", "rmsnorm": "RMSNORM",
                "softmax": "SOFTMAX", "dfl": "DFL", "boxdecode": "BOX_DECODE",
                "conv2d": "CONV2D", "dwconv": "CONV2D", "maxpool": "MAXPOOL2D",
                "nearest": "NEAREST2D", "stack": "CONCAT"}.get(op, op.upper())
        if op.startswith("linear_"):
            weight = weights["q" if op == "linear_w8" else "weight"]
            n, k = weight["shape"]
            dims[:3] = [math.prod(tensors[0]["shape"]) // k, n, k]
            sources = [tensors[0], weight, weights.get("scales"), weights.get("bias")]
        elif op == "embedding":
            sources = [tensors[0], weights["q"], weights["scales"]]
            dims[0] = weights["q"]["shape"][-1]
        elif op == "bmm":
            b, m, k = tensors[0]["shape"]
            dims[:4] = [b, m, tensors[1]["shape"][-1], k]
            dims[4:7] = tensors[0]["source_stride"]
            dims[7:10] = tensors[1]["source_stride"]
            dims[10] = 1
            physical = {0, 1}
        elif op == "rmsnorm":
            sources = [tensors[0], weights["weight"]]
            dims[:3] = [tensors[0]["shape"][-1], 1, f32_bits(attrs["eps"])]
        elif op == "rope":
            dims[:3] = tensors[0]["shape"][-3:]
            dims[3:7] = tensors[0]["source_stride"]
            dims[7] = 1
            physical = {0}
        elif op == "softmax":
            shape = tensors[0]["shape"]
            axis = attrs.get("dim", -1) % len(shape)
            dims[:2] = [shape[axis], math.prod(shape[axis + 1:])]
        elif op == "dfl":
            dims[:2] = [attrs["reg_max"], out["shape"][-1]]
        elif op == "boxdecode":
            dims[:2] = [out["shape"][-1], 0 if attrs.get("xywh", True) else 1]
        elif op == "batchnorm":
            sources = [tensors[0], weights["weight"], weights["bias"],
                       weights["running_mean"], weights["running_var"]]
            dims[:3] = [tensors[0]["shape"][1], math.prod(tensors[0]["shape"][2:]),
                        f32_bits(attrs["eps"])]
        elif op in ("conv2d", "dwconv"):
            x, w, y = tensors[0]["shape"], weights["weight"]["shape"], out["shape"]
            dims[:15] = [x[1], x[2], x[3], y[1], y[2], y[3], w[2], w[3],
                         *pair(attrs["stride"]), *pair(attrs["padding"]),
                         *pair(attrs["dilation"]), attrs["groups"]]
            sources = [tensors[0], weights["weight"], weights.get("bias")]
        elif op == "maxpool":
            x, y = tensors[0]["shape"], out["shape"]
            dims[:13] = [x[1], x[2], x[3], y[2], y[3], *pair(attrs["kernel_size"]),
                         *pair(attrs["stride"]), *pair(attrs["padding"]),
                         *pair(attrs["dilation"])]
        elif op == "nearest":
            dims[:4] = [*tensors[0]["shape"][-2:], *out["shape"][-2:]]
        elif op in ("concat", "stack"):
            axis = attrs.get("dim", 0) % len(out["shape"])
            widths = [1 if op == "stack" else t["shape"][axis] for t in tensors]
            dims[:3] = [math.prod(out["shape"][axis + 1:]), out["shape"][axis], len(tensors)]
            dims[3:3 + len(widths)] = widths
        elif op == "copy":
            source = tensors[1] if attrs.get("aten", "").startswith("aten.copy_.") else tensors[0]
            if source["shape"] != out["shape"]:
                raise ValueError("copy 广播尚未适配，源与输出 shape 必须相同")
            sources = [source]
            src, dst = source["dtype"], out["dtype"]
            modes = {("float32", "float32"): "COPY_F32",
                     ("int64", "float32"): "COPY_I64_TO_F32",
                     ("int64", "int64"): "COPY_I64"}
            dims[0] = abi["GPGPU_EVAL_" + modes[(src, dst)]]
            shape = source["shape"]
            if len(shape) > abi["GPGPU_TENSOR_MAX_RANK"]:
                raise ValueError("copy rank 超过现有参数槽范围")
            dims[1] = len(shape)
            dims[2:2 + len(shape)] = shape
            dims[2 + len(shape):2 + 2 * len(shape)] = source["source_stride"]
            physical = {0}
            if src == "int64" and dst == "float32":
                import numpy as np
                data = np.fromfile(self.root / source["path"], dtype="<i8")
                if data.min() < -(1 << 31) or data.max() >= 1 << 31:
                    raise ValueError("INT64 转 FP32 Kernel 的输入超出 signed32 值域")
        elif op in ("add", "mul", "sub", "div"):
            if len(tensors) == 1:
                if op not in ("add", "mul"):
                    raise ValueError("尚未适配此标量操作")
                name += "_SCALAR"
                dims[0] = f32_bits(attrs["other"])
            else:
                if attrs.get("alpha", 1) != 1:
                    raise ValueError("尚未适配非 1 的 add alpha")
                dims[:2] = [math.prod(t["shape"]) for t in tensors]
        elif op in ("arange", "full"):
            integer = out["dtype"] == "int64"
            dims[2] = abi["GPGPU_EVAL_TYPE_I64" if integer else "GPGPU_EVAL_TYPE_F32"]
            start = attrs.get("fill_value", 0) if op == "full" else attrs.get("start", 0)
            step = attrs.get("step", 1)
            dims[:2] = [int(start), int(step)] if integer else [f32_bits(start), f32_bits(step)]
        elif op not in ("silu", "sigmoid", "sin", "cos"):
            raise ValueError(f"尚未映射的设备算子 {op}")
        if len(sources) > 6:
            raise ValueError("输入数组数超过评估参数块容量")
        count = math.prod(out["shape"])
        params = [0] * (abi["GPGPU_EVAL_PARAM_BYTES"] // struct.calcsize("<Q"))
        params[abi["GPGPU_EVAL_OP"] // 8] = abi["GPGPU_EVAL_" + name]
        params[abi["GPGPU_EVAL_COUNT"] // 8] = count
        start = abi["GPGPU_EVAL_DIMS"] // 8
        params[start:start + len(dims)] = [int(d) & ((1 << 64) - 1) for d in dims]
        paths = [self.tensor_path(t, i in physical)[0] for i, t in enumerate(sources)]
        paths += ["-"] * (6 - len(paths))
        reference, _ = self.tensor_path(out)
        identifier = case["id"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", identifier):
            raise ValueError("case_id 不是安全的文件名")
        path = self.output / (identifier + ".job")
        text = "\n".join(["GPGPU-EVAL-1", identifier, out["dtype"], *paths, reference]) + "\n"
        with path.open("wb") as stream:
            stream.write(text.encode())
            stream.write(struct.pack("<" + "Q" * len(params), *params))
        return {"case_id": identifier, "model": case["model"], "scenario": case["scenario"],
                "op": op, "count": case["count"], "elements": count,
                "path": path.name, "sha256": sha256(path), "dims": dims}


def prepare_command(args):
    """生成一次冻结的客体 job，后续所有执行方式复用同一输入。"""
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        raise ValueError("回放 job 必须保存在 build 下")
    output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads(args.manifest.read_text())
    writer = JobWriter(args.manifest, output)
    jobs, unsupported = [], []
    for case in manifest["cases"]:
        if case["metadata_only"] or case["reference_only"]:
            continue
        try:
            jobs.append(writer.write_case(case))
        except (KeyError, ValueError, TypeError) as error:
            unsupported.append({"case_id": case["id"], "op": case["op"], "error": str(error)})
    result = {"manifest": str(args.manifest.resolve()), "manifest_sha256": sha256(args.manifest),
              "abi_sha256": sha256(ROOT / "software/gpgpu/include/gpgpu/gpgpu_eval_abi.h"),
              "jobs": jobs, "unsupported": unsupported, "physical_inputs": writer.prepared,
              "layout": "BMM/COPY/RoPE 使用原始元素 stride；其他独立输入为逻辑 C 布局"}
    write_json(output / "jobs.json", result)
    print(json.dumps({"jobs": len(jobs), "unsupported": unsupported,
                      "physical_inputs": len(writer.prepared), "output": str(output)}, ensure_ascii=False))
    if unsupported:
        raise RuntimeError("存在未映射的真实签名，详见 jobs.json")


def linux_runtime():
    """只从仓库受控脚本目录加载既有客体运行模块。"""
    scripts = str(Path(__file__).resolve().parent)
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import gpgpu_linux
    return gpgpu_linux


def source_snapshot(output):
    """保存实际工作区源码；新增文件也必须有内容快照，而非只有 HEAD。"""
    records = {}
    for directory in (ROOT / "hw/gpgpu", ROOT / "software/gpgpu"):
        for path in directory.rglob("*"):
            if path.is_file() and (path.suffix in (".c", ".h", ".S", ".inc", ".py", ".ld")
                                   or path.name == "Makefile"):
                relative = path.relative_to(ROOT)
                target = output / "source" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                records[str(relative)] = sha256(target)
    for path in (ROOT / "Makefile.gpgpu", *ROOT.glob("tests/qtest/gpgpu-*.[ch]")):
        target = output / "source" / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        records[str(path.relative_to(ROOT))] = sha256(target)
    write_json(output / "source-manifest.json", records)
    return {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                            text=True).strip(),
            "source_manifest_sha256": sha256(output / "source-manifest.json")}


def process_memory(pid):
    values = {"pid": pid}
    for filename in ("status", "smaps_rollup"):
        path = Path(f"/proc/{pid}/{filename}")
        try:
            text = path.read_text()
        except (FileNotFoundError, PermissionError):
            continue
        for key, value in re.findall(r"^(VmRSS|VmHWM|VmSwap|Rss|Pss|Swap):\s+(\d+) kB$", text, re.M):
            values[key + "_kib"] = int(value)
    return values


def benchmark_setup(guest, driver_parameters=()):
    """沿用原示例的编译、固件安装及模块装载顺序。"""
    module_command = shlex.join(["insmod", "/mnt/gpgpu-build/gpgpu_pci.ko", *driver_parameters])
    guest.buffer = b""
    guest.send(
        "mkdir -p /mnt/gpgpu-stack /mnt/gpgpu-eval-data /mnt/gpgpu-eval-jobs "
        "/mnt/gpgpu-eval-results\n"
        "mount -t 9p -o trans=virtio,version=9p2000.L,ro gpgpu_stack /mnt/gpgpu-stack\n"
        "mount -t 9p -o trans=virtio,version=9p2000.L,ro eval_data /mnt/gpgpu-eval-data\n"
        "mount -t 9p -o trans=virtio,version=9p2000.L,ro eval_jobs /mnt/gpgpu-eval-jobs\n"
        "mount -t 9p -o trans=virtio,version=9p2000.L eval_results /mnt/gpgpu-eval-results\n"
        "(set -e; make -C /mnt/gpgpu-stack BUILD_DIR=/mnt/gpgpu-eval-results/stack all; "
        "if test -L /lib/firmware && ! test -e /lib/firmware; then unlink /lib/firmware; fi; "
        "mkdir -p /lib/firmware; cp /mnt/gpgpu-build/gpgpu-cp.bin /lib/firmware/gpgpu-cp.bin; "
        + module_command + "; test -c /dev/dri/renderD128) "
        ">/mnt/gpgpu-eval-results/setup.log 2>&1; "
        "printf 'GPGPU_EVAL_SETUP=%s\\n' \"$?\"\n")
    result = guest.wait(rb"^GPGPU_EVAL_SETUP=(\d+)$", timeout=1200)
    if result.group(1) != b"0":
        raise RuntimeError("评估客体准备失败，查看 setup.log")


def runner_command(args, job):
    command = ["/mnt/gpgpu-eval-results/stack/gpgpu-model-eval", "--job",
               "/mnt/gpgpu-eval-jobs/" + job["path"], "--repeats", str(args.repeats),
               "--warmups", str(args.warmups), "--timeout-ms", str(args.timeout_ms),
               "--backend", args.backend]
    if args.tile is not None:
        command += ["--tile", str(args.tile)]
    if args.uncached:
        command.append("--uncached")
    return command


def read_runner_log(path):
    records = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and "case_id" in value:
                records.append(value)
    return records


def device_events(output, prefix="GPGPU_DISPATCH"):
    result = []
    path = output / "device.log"
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith(prefix + " "):
                try:
                    result.append(json.loads(line.removeprefix(prefix + " ")))
                except json.JSONDecodeError:
                    # 被终止时的半行只留在原日志，不覆盖最初的运行错误。
                    continue
    return result


def dispatch_lifecycle(record, dispatches, doorbells, progress):
    """只用远端同一时钟配对；Doorbell 晚于开始时不伪造排队时长。"""
    begin, end = record["host_begin_ns"], record["host_end_ns"]
    bells = {}
    for bell in doorbells:
        if begin <= bell["host_ns"] <= end:
            bells.setdefault((bell["offset"], bell["packet"]), []).append(bell)
    advances = [p for p in progress if begin <= p["host_ns"] <= end
                and ((p["rptr_after"] - p["rptr_before"]) & 0xffffffff) == 1]
    stages = []
    for index, event in enumerate(dispatches):
        stage = {"start_ns": event["start_ns"], "end_ns": event["end_ns"],
                 "slot": event["slot"], "dispatch_wall_ns": event["end_ns"] - event["start_ns"],
                 "service_ns": event["service_ns"], "completion_tail_ns": None,
                 "doorbell_ns": None, "queue_wait_lower_bound_ns": None,
                 "polled_before_doorbell": None}
        following = next((e["start_ns"] for e in dispatches[index + 1:]
                          if e["slot"] == event["slot"]), end + 1)
        retired = next((p for p in advances if p["slot"] == event["slot"]
                        and event["end_ns"] <= p["host_ns"] < following), None)
        if retired:
            stage["packet"] = retired["rptr_before"]
            stage["completion_tail_ns"] = retired["host_ns"] - event["end_ns"]
            matches = bells.get((retired["doorbell_offset"], retired["rptr_before"]), [])
            # 同一包可以重复提醒，取本运行区间内最早一次；到达晚于开始仍保留。
            if matches:
                arrival = min(b["host_ns"] for b in matches)
                stage["doorbell_ns"] = arrival
                stage["polled_before_doorbell"] = arrival > event["start_ns"]
                if arrival <= event["start_ns"]:
                    stage["queue_wait_lower_bound_ns"] = event["start_ns"] - arrival
        stages.append(stage)
    waits = [s["queue_wait_lower_bound_ns"] for s in stages
             if s["queue_wait_lower_bound_ns"] is not None]
    return {"events": stages, "max_queue_wait_lower_bound_ns": max(waits) if waits else None,
            "max_dispatch_wall_ns": max((s["dispatch_wall_ns"] for s in stages), default=None),
            "polled_before_doorbell_count": sum(s["polled_before_doorbell"] is True for s in stages),
            "unpaired_progress_count": sum(s["completion_tail_ns"] is None for s in stages),
            "boundary": "Doorbell 到开始仅为已观测排队下界，发布早于提醒；不跨客体与宿主时钟相减"}


def wait_files(paths, guest, remote, deadline, early_exits=()):
    """应用准备/完成屏障，只观察本次运行创建的文件和子进程。"""
    while not all(path.exists() for path in paths):
        if guest.process.poll() is not None or remote.process.poll() is not None:
            raise RuntimeError("双进程屏障期间 QEMU 退出")
        if any(path.exists() for path in early_exits):
            raise RuntimeError("应用在就绪前退出，查看对应日志和 rc 文件")
        if time.monotonic() >= deadline:
            raise TimeoutError("双进程屏障超时，保留原日志")
        time.sleep(0.02)


def run_dual(args, jobs, output, guest, remote):
    """两个独立客体进程先各自建队列，再共同计算，最后共同收尾。"""
    slots, prepare_times = {}, {}
    for job in jobs:
        role = job["model"]
        command = runner_command(args, job)
        for option, name in (("--ready", role + ".ready"), ("--start", "start"),
                             ("--done", role + ".done"), ("--release", "release")):
            command += [option, "/mnt/gpgpu-eval-results/" + name]
        started = time.monotonic_ns()
        guest.send("(" + shlex.join(command)
                   + " > /mnt/gpgpu-eval-results/" + role + ".log 2>&1; "
                   + "printf '%s\\n' \"$?\" > /mnt/gpgpu-eval-results/" + role + ".rc.tmp && "
                   + "mv /mnt/gpgpu-eval-results/" + role + ".rc.tmp "
                   + "/mnt/gpgpu-eval-results/" + role + ".rc; "
                   + "touch /mnt/gpgpu-eval-results/" + role + ".done) &\n")
        wait_files([output / (role + ".ready")], guest, remote,
                   time.monotonic() + args.case_timeout, [output / (role + ".rc")])
        ended = time.monotonic_ns()
        prepare_times[role] = ended - started
        observed = [e["slot"] for e in device_events(output) if started <= e["start_ns"] <= ended]
        slots[role] = observed[-1] if observed else None
    started = time.monotonic_ns()
    (output / "start").touch()
    wait_files([output / (job["model"] + ".done") for job in jobs], guest, remote,
               time.monotonic() + args.case_timeout)
    ended = time.monotonic_ns()
    (output / "release").touch()
    wait_files([output / (job["model"] + ".rc") for job in jobs], guest, remote,
               time.monotonic() + args.case_timeout)
    results = []
    for job in jobs:
        role = job["model"]
        results.append({"case_id": job["case_id"], "model": role, "scenario": job["scenario"],
                        "op": job["op"], "return_code": int((output / (role + ".rc")).read_text()),
                        "runner": read_runner_log(output / (role + ".log")), "log": role + ".log",
                        "host_begin_ns": started, "host_end_ns": ended,
                        "host_joint_workload_elapsed_ns": ended - started,
                        "prepare_elapsed_ns": prepare_times[role], "source_slot": slots[role],
                        "host_memory": process_memory(guest.process.pid),
                        "device_memory": process_memory(remote.process.pid)})
    return results


def verify_materials(index, jobs, jobs_root):
    """回放前核对选中 job、原数组及对应 stride 存储，不计入算子时间。"""
    started = time.monotonic_ns()
    manifest_path = Path(index["manifest"]).resolve()
    if sha256(manifest_path) != index["manifest_sha256"]:
        raise ValueError("冻结参考清单发生变化，请保留旧结果后重新准备")
    cases = {case["id"]: case for case in json.loads(manifest_path.read_text())["cases"]}
    expected = {}

    def collect(value, root):
        if isinstance(value, dict):
            if {"path", "sha256"} <= value.keys():
                path = (root / value["path"]).resolve()
                if not path.is_relative_to(root):
                    raise ValueError("冻结材料路径离开数据目录")
                expected[path] = value["sha256"]
            for child in value.values():
                collect(child, root)
        elif isinstance(value, list):
            for child in value:
                collect(child, root)

    for job in jobs:
        collect(job, jobs_root)
        collect(cases[job["case_id"]], manifest_path.parent)
    original_paths = set(expected)
    for tensor in index["physical_inputs"].values():
        if (manifest_path.parent / tensor["source"]).resolve() in original_paths:
            collect(tensor, jobs_root)
    for path, digest in expected.items():
        if sha256(path) != digest:
            raise ValueError("冻结材料内容发生变化: " + str(path))
    return {"files": len(expected), "bytes": sum(path.stat().st_size for path in expected),
            "elapsed_ns": time.monotonic_ns() - started}


def run_command(args):
    """在同一客体逐例回放；每例完整结果立即落盘，失败不抹掉前例。"""
    import fcntl
    linux = linux_runtime()
    jobs_root = args.jobs.resolve()
    index = json.loads((jobs_root / "jobs.json").read_text())
    jobs = [job for job in index["jobs"]
            if (args.model == "all" or job["model"] == args.model)
            and (not args.scenario or job["scenario"] == args.scenario)
            and (not args.case_regex or re.search(args.case_regex, job["case_id"]))]
    if args.limit:
        jobs = jobs[:args.limit]
    if args.dual:
        selected = []
        for model in ("qwen", "yolo"):
            match = next((job for job in jobs if job["model"] == model), None)
            if match is None:
                raise ValueError("双进程选择必须同时包含 Qwen 和 YOLO 用例")
            selected.append(match)
        jobs = selected
    if not jobs:
        raise ValueError("没有匹配的真实算子用例")
    material_verification = verify_materials(index, jobs, jobs_root)
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        raise ValueError("运行结果必须保存在 build 下")
    output.mkdir(parents=True, exist_ok=False)
    (output / "stack").mkdir()
    # Unix socket 有路径长度限制，不能把临时目录嵌在长的结果路径中。
    temporary = ROOT / "build/qt"
    temporary.mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(temporary)
    import tempfile
    tempfile.tempdir = None
    metadata = {"created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "gpuvm": args.gpuvm, "uncached": args.uncached, "dual": args.dual,
                "execution": args.execution, "backend": args.backend, "repeats": args.repeats,
                "warmups": args.warmups, "tile": args.tile,
                "device_args": list(args.device_arg),
                "driver_parameters": list(args.driver_param),
                "manifest": index["manifest"], "manifest_sha256": index["manifest_sha256"],
                "jobs_sha256": sha256(jobs_root / "jobs.json"),
                "material_verification": material_verification,
                "selected_cases": [job["case_id"] for job in jobs],
                "host": platform.platform(), "memory_before": Path("/proc/meminfo").read_text(),
                "version": source_snapshot(output), "results": [], "status": "running"}
    write_json(output / "result.json", metadata)
    env = args.environment.resolve()
    guest, remote = None, None
    with (env / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            environment = linux.environment_metadata(env)
            if not environment:
                raise RuntimeError("现有开发客体未通过核对；不重建或覆盖开发盘")
            linux.ensure_qemu(env)
            fingerprint = linux.source_hash()
            module = linux.prepared_metadata(env)
            if not module or module.get("source_hash") != fingerprint:
                linux.compile_module(env, fingerprint, environment)
            linux.prepare_firmware(env)
            subprocess.run(["make", "-C", str(ROOT / "software/gpgpu"),
                            "BUILD_DIR=" + str(output / "stack"), "kernels"], check=True)
            extra = ["-global", "gpgpu.trace_dispatch=on", *args.device_arg]
            if args.execution != "lane":
                extra += ["-global", "gpgpu.shared_fetch=on"]
            remote = linux.RemoteDevice(args.device_qemu.resolve(), output,
                                        gpuvm=args.gpuvm == "on", extra_args=extra)
            command = linux.developer_command(env, "none", remote=True)
            command += linux.shared_directory(Path(index["manifest"]).parent, "eval_data", True)
            command += linux.shared_directory(jobs_root, "eval_jobs", True)
            command += linux.shared_directory(output, "eval_results", False)
            guest = linux.Guest(command, output / "console.log", remote)
            metadata["host_command"] = guest.process.args
            metadata["device_command"] = remote.process.args
            guest.wait(rb"^GPGPU_DEVELOPER_READY$", timeout=180)
            benchmark_setup(guest, args.driver_param)
            components = [linux.QEMU_BINARY, args.device_qemu.resolve(), env / "gpgpu_pci.ko",
                          env / "gpgpu-cp.bin", output / "stack/gpgpu-model-eval",
                          output / "stack/model_eval.bin"]
            metadata["components"] = {str(path): sha256(path) for path in components}
            binaries = output / "binaries"
            binaries.mkdir()
            for path in components:
                shutil.copy2(path, binaries / path.name)
            metadata["qemu_build_configuration"] = (linux.QEMU_DIRECTORY / "config-host.mak").read_text()
            device_configuration = args.device_qemu.resolve().parent / "config-host.mak"
            metadata["device_qemu_build_configuration"] = (device_configuration.read_text()
                                                          if device_configuration.exists() else None)
            if args.dual:
                metadata["results"] = run_dual(args, jobs, output, guest, remote)
                write_json(output / "result.json", metadata)
                jobs = []
            for i, job in enumerate(jobs):
                guest.buffer = b""
                log = output / (job["case_id"] + ".log")
                command = shlex.join(runner_command(args, job))
                marker = f"GPGPU_EVAL_RC_{i}"
                started = time.monotonic_ns()
                guest.send(command + " >" + shlex.quote("/mnt/gpgpu-eval-results/" + log.name)
                           + " 2>&1; printf '" + marker + "=%s\\n' \"$?\"\n")
                result = guest.wait(("^" + marker + r"=(\d+)$").encode(), timeout=args.case_timeout)
                record = {"case_id": job["case_id"], "model": job["model"], "scenario": job["scenario"],
                          "op": job["op"], "return_code": int(result.group(1)),
                          "host_begin_ns": started, "host_end_ns": time.monotonic_ns(),
                          "host_command_elapsed_ns": time.monotonic_ns() - started,
                          "runner": read_runner_log(log), "log": log.name,
                          "host_memory": process_memory(guest.process.pid),
                          "device_memory": process_memory(remote.process.pid)}
                metadata["results"].append(record)
                write_json(output / "result.json", metadata)
                print(json.dumps(record, ensure_ascii=False), flush=True)
                passed = (record["return_code"] == 0 and record["runner"]
                          and all(item.get("success") for item in record["runner"]))
                if not passed and not args.keep_going:
                    raise RuntimeError("真实算子失败: " + job["case_id"])
            guest.buffer = b""
            linux.check_kernel_log(guest)
            passed = all(r["return_code"] == 0 and r["runner"]
                         and all(item.get("success") for item in r["runner"])
                         for r in metadata["results"])
            metadata["status"] = "pass" if passed else "failed"
            if not passed:
                raise RuntimeError("存在失败的真实算子，查看各用例日志")
        except BaseException as error:
            metadata["status"] = "failed"
            metadata["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            metadata["memory_after"] = Path("/proc/meminfo").read_text()
            write_json(output / "result.json", metadata)
            if guest:
                guest.close()
            elif remote:
                remote.close()
            events = device_events(output)
            doorbells = device_events(output, "GPGPU_DOORBELL")
            progress = device_events(output, "GPGPU_QUEUE_PROGRESS")
            metadata["dispatch_event_count"] = len(events)
            metadata["doorbell_event_count"] = len(doorbells)
            metadata["queue_progress_event_count"] = len(progress)
            metadata["dispatch_trace_scope"] = "宿主机时钟，包含准备与预热派发；完整算子区间另见 operator_ns"
            for record in metadata["results"]:
                subset = [e for e in events
                          if record["host_begin_ns"] <= e["start_ns"] <= record["host_end_ns"]
                          and (record.get("source_slot") is None or e["slot"] == record["source_slot"])]
                if args.dual and record.get("source_slot") is None:
                    # 缺少队列归属就不把两进程的事件都算给当前进程。
                    subset = []
                record["device_dispatch_events"] = subset
                record["dispatch_lifecycle"] = dispatch_lifecycle(record, subset, doorbells, progress)
                record["dispatch_trace_available"] = bool(subset)
                record["host_dispatch_wall_ns"] = (sum(e["end_ns"] - e["start_ns"] for e in subset)
                                                   if subset else None)
                record["host_dispatch_service_ns"] = (sum(e["service_ns"] for e in subset)
                                                      if subset else None)
            write_json(output / "result.json", metadata)


def case_work(case):
    """逻辑 MAC 按两次浮点操作计，非矩阵算子用输出元素量归一化。"""
    elements = math.prod(case["output"]["shape"])
    op = case["op"]
    if op in ("linear_w8", "linear_f32"):
        weights = case["weights"]
        weight = weights.get("q", weights.get("weight"))
        return elements * weight["shape"][-1] * 2, "logical_flop"
    if op == "bmm":
        return elements * case["inputs"][0]["shape"][-1] * 2, "logical_flop"
    if op in ("conv2d", "dwconv"):
        weight = case["weights"]["weight"]["shape"]
        return elements * math.prod(weight[1:]) * 2, "logical_flop"
    return elements, "output_element"


def case_family(case):
    """只在执行形式相容的算子间借用速率，不混用单值转换和张量复制。"""
    variant = ()
    if case["op"] in ("copy", "concat", "stack"):
        variant = (len(case["inputs"][0]["shape"]),)
    elif case["op"] in ("linear_w8", "linear_f32"):
        variant = (math.prod(case["inputs"][0]["shape"][:-1]) == 1,)
    elif case["op"] == "bmm":
        shape = case["inputs"][0]["shape"]
        variant = (shape[-1] == 1, shape[-2] == 1)
    return (case["model"], case["op"], tuple(t["dtype"] for t in case["inputs"]),
            case["output"]["dtype"], variant)


def model_budget(manifest, cases, measured):
    """按完整权重和不复用数值输出的保守策略估算空间与准备成本。"""
    budgets = {}
    for name, model in manifest["models"].items():
        weights = {}
        for record in model["weights"].values():
            tensors = (record["q"], record["scales"]) if "q" in record else (record["weight"],)
            for tensor in tensors:
                weights[tensor["path"]] = tensor
        weight_bytes = sum(t["bytes"] for t in weights.values())
        observed = [(case, measured[case["id"]]) for case in cases
                    if case["model"] == name and case["id"] in measured]
        allocations, uploads, downloads = [], [], []
        fixed_setup = []
        for case, item in observed:
            vram_kind = next(b["kind"] for b in item["bos"] if b["role"] == "output")
            vram_bytes = sum(b["mapped_bytes"] for b in item["bos"] if b["kind"] == vram_kind)
            if vram_bytes:
                allocations.append((vram_bytes, item["alloc_clear_ns"] + item["buffer_setup_ns"],
                                    case["id"]))
            staging = item.get("staging_bytes")
            chunks = item.get("h2d_chunks")
            if staging and chunks:
                uploads.append((item["h2d_bytes"], item["h2d_ns"] / chunks,
                                item["input_read_ns"] / chunks, staging, case["id"]))
            if staging:
                chunks = sum(math.ceil(s["d2h_bytes"] / staging) for s in item["samples"]
                             if s["checked"])
                if chunks:
                    downloads.append((item["d2h_bytes"], item["d2h_ns"] / chunks,
                                      staging, case["id"]))
            fixed_setup.append(item["runtime_setup_ns"] + item["kernel_load_ns"]
                               + item["queue_prepare_ns"])

        # 大对象的准备速率不与几页控制对象的固定成本混合。
        def large_samples(values):
            largest = max((v[0] for v in values), default=0)
            return [v for v in values if v[0] * 2 >= largest]

        allocations = large_samples(allocations)
        uploads = large_samples(uploads)
        downloads = large_samples(downloads)
        for scenario_name, scenario in model["scenarios"].items():
            selected = [c for c in cases if c["model"] == name and c["scenario"] == scenario_name]
            output_bytes = sum(c["output"]["bytes"] * c["count"] for c in selected)
            scratch_bytes = 0
            for case in selected:
                shape = case["inputs"][0]["shape"] if case["inputs"] else []
                if case["op"] == "softmax":
                    width = shape[case["attrs"].get("dim", -1)]
                    scratch = case["inputs"][0]["bytes"] + case["output"]["bytes"] // width
                elif case["op"] == "rmsnorm":
                    scratch = case["output"]["bytes"] // shape[-1]
                elif case["op"] == "dfl":
                    scratch = case["output"]["bytes"] + 2 * case["inputs"][0]["bytes"]
                else:
                    scratch = 0
                scratch_bytes = max(scratch_bytes, scratch)
            if name == "qwen":
                kv_before = sum(t[k]["bytes"] for t in scenario["kv"]
                                for k in ("key_before", "value_before") if k in t)
                kv_after = sum(t[k]["bytes"] for t in scenario["kv"]
                               for k in ("key_after", "value_after"))
                inputs = [scenario["query_ids"]["bytes"], scenario["query_ids"]["bytes"],
                          scenario["attention_mask"]["bytes"]]
                result_bytes = scenario["last_logits_fp32"]["bytes"]
            else:
                kv_before = kv_after = 0
                inputs = [model["input"]["tensor"]["bytes"]]
                result_bytes = scenario["pre_nms"]["bytes"]
            vram_bytes = weight_bytes + kv_before + sum(inputs) + output_bytes + scratch_bytes
            budget = {"stored_weight_bytes": weight_bytes, "kv_before_bytes": kv_before,
                      "kv_after_bytes": kv_after, "numeric_outputs_no_reuse_bytes": output_bytes,
                      "scratch_peak_bytes": scratch_bytes, "boundary_input_bytes": sum(inputs),
                      "boundary_output_bytes": result_bytes, "planned_data_vram_bytes": vram_bytes,
                      "boundary_transfer_ns": None, "cold_preparation_ns": None,
                      "boundary_range_ns": None, "cold_preparation_range_ns": None}
            if allocations and uploads and downloads:
                # 以各输入至少一次 COPY 计费，不能用大块每字节速率低估小参数搬运。
                incoming = [sum(math.ceil(n / u[3]) for n in inputs) * u[1] for u in uploads]
                outgoing = [math.ceil(result_bytes / d[2]) * d[1] for d in downloads]
                allocate = [vram_bytes * a[1] / a[0] for a in allocations]
                load = [math.ceil((weight_bytes + kv_before) / u[3]) * (u[1] + u[2]) for u in uploads]
                budget["boundary_transfer_ns"] = statistics.median(incoming) + statistics.median(outgoing)
                budget["boundary_range_ns"] = [min(incoming) + min(outgoing), max(incoming) + max(outgoing)]
                budget["cold_preparation_ns"] = (statistics.median(allocate) + statistics.median(load)
                                                  + statistics.median(fixed_setup))
                budget["cold_preparation_range_ns"] = [min(allocate) + min(load) + min(fixed_setup),
                                                         max(allocate) + max(load) + max(fixed_setup)]
                budget["proxies"] = {"allocation": [a[2] for a in allocations],
                                     "upload": [u[4] for u in uploads],
                                     "download": [d[3] for d in downloads]}
            budgets[name + "/" + scenario_name] = budget
    return budgets


def estimate_command(args):
    """精确签名用实测；未测签名显式按同类速率外推，缺项不伪造总时间。"""
    manifest = json.loads(args.manifest.read_text())
    manifest_hash = sha256(args.manifest)
    samples, contexts, per_case = {}, [], {}
    for path in args.runs:
        run = json.loads(path.read_text())
        if run["status"] != "pass":
            raise ValueError("失败或未完成的运行不能用于模型耗时估算")
        if run.get("dual", False):
            raise ValueError("双进程竞争样本不能用于单模型耗时估算")
        if run["manifest_sha256"] != manifest_hash:
            raise ValueError("运行记录与模型参考清单不一致")
        context = (run["gpuvm"], run["uncached"], run["execution"], run.get("backend", "scalar"),
                   run.get("tile"),
                   tuple(run.get("device_args", [])), tuple(run.get("driver_parameters", [])),
                   sorted((Path(k).name, v) for k, v in run.get("components", {}).items()))
        contexts.append(context)
        for record in run["results"]:
            for item in record["runner"]:
                if record["return_code"] == 0 and item.get("success"):
                    samples.setdefault(record["case_id"], []).extend(item["operator_ns"])
                    per_case[record["case_id"]] = item
    if any(context != contexts[0] for context in contexts[1:]):
        raise ValueError("不能把不同配置或二进制版本的结果混成一组模型估算")
    cases = [c for c in manifest["cases"] if not c["metadata_only"] and not c["reference_only"]]
    rates = {}
    for case in cases:
        observed = samples.get(case["id"])
        if observed:
            work, unit = case_work(case)
            rates.setdefault(case_family(case), []).append(
                {"median": statistics.median(observed) / work,
                 "low": min(observed) / work, "high": max(observed) / work,
                 "work": work, "case_id": case["id"]})
    scenarios = {}
    for case in cases:
        work, unit = case_work(case)
        key = case["model"] + "/" + case["scenario"]
        scenario = scenarios.setdefault(key, {"measured_component_ns": 0, "estimated_ns": 0,
                                              "range_ns": [0, 0], "operators": [], "missing": []})
        observed = samples.get(case["id"])
        if observed:
            point, low, high = statistics.median(observed), min(observed), max(observed)
            method, proxies = "whole_operator_measured", [case["id"]]
            range_proxies = proxies
            scenario["measured_component_ns"] += point * case["count"]
        else:
            candidates = rates.get(case_family(case), [])
            if not candidates:
                scenario["missing"].append({"case_id": case["id"], "op": case["op"],
                                             "calls": case["count"]})
                continue
            distance = min(abs(math.log(v["work"] / work)) for v in candidates)
            nearest = [v for v in candidates if math.isclose(abs(math.log(v["work"] / work)),
                                                            distance, abs_tol=1e-12)]
            point = statistics.median(v["median"] for v in nearest) * work
            low = min(v["low"] for v in candidates) * work
            high = max(v["high"] for v in candidates) * work
            method = "compatible_family_nearest_work_rate"
            proxies = [v["case_id"] for v in nearest]
            range_proxies = [v["case_id"] for v in candidates]
        calls = case["count"]
        scenario["estimated_ns"] += point * calls
        scenario["range_ns"][0] += low * calls
        scenario["range_ns"][1] += high * calls
        scenario["operators"].append({"case_id": case["id"], "op": case["op"], "calls": calls,
                                      "method": method, "ns_per_call": point,
                                      "range_ns": [low, high], "proxies": proxies,
                                      "range_proxies": range_proxies,
                                      "work": work, "work_unit": unit})
    budgets = model_budget(manifest, cases, per_case)
    for name, scenario in scenarios.items():
        scenario["complete_estimate"] = not scenario["missing"]
        if scenario["missing"]:
            scenario["estimated_ns"] = None
            scenario["range_ns"] = None
        budget = budgets[name]
        scenario["resources"] = budget
        scenario["resident_estimated_ns"] = None
        scenario["resident_range_ns"] = None
        scenario["cold_start_estimated_ns"] = None
        scenario["cold_start_range_ns"] = None
        if scenario["complete_estimate"] and budget["boundary_transfer_ns"] is not None:
            scenario["resident_estimated_ns"] = (scenario["estimated_ns"]
                                                  + budget["boundary_transfer_ns"])
            scenario["resident_range_ns"] = [a + b for a, b in zip(scenario["range_ns"],
                                                                 budget["boundary_range_ns"])]
            scenario["cold_start_estimated_ns"] = (scenario["resident_estimated_ns"]
                                                    + budget["cold_preparation_ns"])
            scenario["cold_start_range_ns"] = [a + b for a, b in zip(scenario["resident_range_ns"],
                                                                    budget["cold_preparation_range_ns"])]
    output = {"manifest_sha256": sha256(args.manifest), "runs": [str(p.resolve()) for p in args.runs],
              "configuration": contexts[0], "measured_signatures": len(samples),
              "scenarios": scenarios,
              "boundary": ["这是算子重放汇总，不是完整模型实测",
                           "operator_ns 为客体单调时钟的完整算子区间；宿主墙钟与设备 Dispatch 日志另存",
                           "外推先区分 dtype、单值转换/张量复制、复制秩、单行/多行投影和外积/点积，再取相容类中工作量最近的速率",
                           "速率包含已测分块和同步成本；不同 shape 的缓存、占用和派发差异尚未逐签名验证",
                           "范围是样本及同类归一化速率的包络，不是统计置信区间",
                           "estimated_ns 只汇总算子；resident_estimated_ns 另加输入和最终输出 COPY，中间结果不逐层回读",
                           "冷启动另加分配/映射外推、一次 RT/Kernel 准备，以及权重和已有 KV 的文件读取与上传",
                           "空间预算合并不可变权重，并保留所有数值输出到网络结束，仅复用最大 scratch；未实测完整模型分配",
                           "空间不含页表、对齐碎片、控制对象和 Host 内核私有缓冲；不能据此保证连续 BO 一定分配成功",
                           "准备成本取同模型大对象的规模/分块速率包络，仍是外推；旧记录缺少分块信息时不生成冷启动总值",
                           "Qwen 已有 KV 来自 CPU 参考，常驻估算假设设备已持有上下文，不代表 GPU 已生成该上下文",
                           "输入及因果 mask 已准备；排除 tokenizer、预处理、采样、生成控制、NMS、界面及未实现的算子融合"]}
    write_json(args.output, output)
    for name, scenario in scenarios.items():
        print(json.dumps({"scenario": name, "complete_estimate": scenario["complete_estimate"],
                          "estimated_seconds": None if scenario["estimated_ns"] is None else scenario["estimated_ns"] / 1e9,
                          "range_seconds": None if scenario["range_ns"] is None else [v / 1e9 for v in scenario["range_ns"]],
                          "resident_seconds": (None if scenario["resident_estimated_ns"] is None
                                               else scenario["resident_estimated_ns"] / 1e9),
                          "cold_start_seconds": (None if scenario["cold_start_estimated_ns"] is None
                                                 else scenario["cold_start_estimated_ns"] / 1e9),
                          "missing_signatures": len(scenario["missing"])}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    add_export_subcommands(subparsers)
    prepare = subparsers.add_parser("prepare", help="把冻结的真实用例映射到客体参数块")
    prepare.add_argument("manifest", type=Path)
    prepare.add_argument("--output", type=Path, required=True)
    prepare.set_defaults(function=prepare_command)
    run = subparsers.add_parser("run", help="在现有 ARM64 客体回放冻结的真实算子")
    run.add_argument("jobs", type=Path)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--environment", type=Path, default=ROOT / "build/gpgpu-kernel-gpuvm")
    run.add_argument("--device-qemu", type=Path, default=ROOT / "build/arm64-qemu/qemu-system-riscv64")
    run.add_argument("--model", choices=("qwen", "yolo", "all"), default="all")
    run.add_argument("--scenario")
    run.add_argument("--case-regex")
    run.add_argument("--limit", type=int)
    run.add_argument("--gpuvm", choices=("on", "off"), default="on")
    run.add_argument("--execution", choices=("lane", "warp"), default="lane")
    run.add_argument("--backend", choices=("scalar", "block"), default="scalar")
    run.add_argument("--uncached", action="store_true")
    run.add_argument("--device-arg", action="append", default=[])
    run.add_argument("--driver-param", action="append", default=[],
                     help="传给 insmod 的模块参数；逐项保存，不改变现有入口默认值")
    run.add_argument("--tile", type=int)
    run.add_argument("--repeats", type=int, default=1)
    run.add_argument("--warmups", type=int, default=0)
    run.add_argument("--timeout-ms", type=int, default=300000)
    run.add_argument("--case-timeout", type=float, default=7200,
                     help="单个完整用例的保护性等待上限，不是性能通过门槛")
    run.add_argument("--keep-going", action="store_true")
    run.add_argument("--dual", action="store_true", help="同一客体各选一个 Qwen/YOLO 用例共同回放")
    run.set_defaults(function=run_command)
    estimate = subparsers.add_parser("estimate", help="按真实调用数汇总实测与明确标记的外推")
    estimate.add_argument("manifest", type=Path)
    estimate.add_argument("runs", type=Path, nargs="+")
    estimate.add_argument("--output", type=Path, required=True)
    estimate.set_defaults(function=estimate_command)
    args = parser.parse_args()
    args.function(args)


if __name__ == "__main__":
    main()
