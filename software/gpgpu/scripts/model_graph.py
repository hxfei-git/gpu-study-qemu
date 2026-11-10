# SPDX-License-Identifier: GPL-2.0-or-later
"""受控模型图：张量视图、具名权重、激活寿命与客体程序打包。

只编排已知模型；常量只能是权重、输入或与输入值无关的固定配置。
节点只保存现有 RV64 算子的参数，不执行神经网络数值计算。
"""

from dataclasses import dataclass, field
import ast
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct

from gpgpu_model_eval import eval_abi, f32_bits, contiguous_stride, pair, sha256


ROOT = Path(__file__).resolve().parents[3]
PROGRAM_HEADER = Path(__file__).resolve().parents[1] / "demos/model_program.h"


def program_abi():
    """只读取受控头文件的整数和字符串，不执行 C 表达式。"""
    result = {}
    for name, value in re.findall(r"^#define[ \t]+(GPGPU_MODEL_\w+)[ \t]+(\S+)",
                                  PROGRAM_HEADER.read_text(), re.M):
        result[name] = ast.literal_eval(value)
    return result


def aligned(value, alignment):
    return (value + alignment - 1) // alignment * alignment


@dataclass(eq=False)
class Storage:
    """一个底层区间；所有别名共同保活 producer 到最后一次读取。"""

    arena: int
    size: int
    name: str
    offset: int = 0
    producer: int = -1
    last: int = -1
    source: dict | None = None
    data: object = None


@dataclass
class Tensor:
    """offset 是相对 storage 的字节数，stride 的单位是元素。"""

    storage: Storage
    shape: tuple
    stride: tuple
    dtype: str = "float32"
    offset: int = 0

    @property
    def elements(self):
        return math.prod(self.shape)

    @property
    def itemsize(self):
        return {"float32": 4, "int64": 8, "int8": 1}[self.dtype]

    @property
    def span(self):
        return (1 + sum((d - 1) * s for d, s in zip(self.shape, self.stride))) * self.itemsize

    @property
    def contiguous(self):
        return all(d == 1 or s == c for d, s, c in
                   zip(self.shape, self.stride, contiguous_stride(self.shape)))


@dataclass
class Node:
    name: str
    op: str
    inputs: list
    output: Tensor
    dims: list
    rows: int
    work_count: int
    scratch: list = field(default_factory=list)
    stage: str = "model"


@dataclass
class Round:
    name: str
    kind: int
    nodes: list = field(default_factory=list)
    output: Tensor | None = None
    token_slot: Tensor | None = None
    checks: list = field(default_factory=list)


class GraphBuilder:
    """只用五类 BO，激活区间按节点序号做最后消费者复用。"""

    def __init__(self, manifest_path, model, checkpoints=False):
        self.manifest_path = Path(manifest_path).resolve()
        self.reference_root = self.manifest_path.parent
        manifest_bytes = self.manifest_path.read_bytes()
        self.manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
        self.model = model
        self.model_info = json.loads(manifest_bytes)["models"][model]
        del manifest_bytes
        self.abi = eval_abi()
        self.slot_bytes = self.abi["GPGPU_EVAL_SLOT_BYTES"]
        self.input_count = (self.abi["GPGPU_EVAL_IN5"] - self.abi["GPGPU_EVAL_IN0"]) // self.slot_bytes + 1
        self.dim_count = (self.abi["GPGPU_EVAL_D15"] - self.abi["GPGPU_EVAL_D0"]) // self.slot_bytes + 1
        self.scratch_count = (self.abi["GPGPU_EVAL_AUX2"] - self.abi["GPGPU_EVAL_AUX0"]) // self.slot_bytes + 1
        self.program = program_abi()
        self.stage = "model"
        self.capture_checks = bool(checkpoints)
        self.weights = {}
        self.used_weights = set()
        self.constants = {}
        self.storages = []
        self.nodes = []
        self.rounds = []
        self.current = None
        self.input_tensor = None
        self.arena_sizes = [0] * self.program["GPGPU_MODEL_ARENA_COUNT"]
        self.scratch = [None] * self.scratch_count
        self._planned = False

    def _storage(self, arena, size, name, source=None, data=None):
        if size <= 0:
            raise ValueError(f"非正存储长度: {name}")
        value = Storage(arena, int(size), name, source=source, data=data)
        self.storages.append(value)
        return value

    def _tensor(self, storage, shape, dtype="float32", stride=None, offset=0):
        shape = tuple(int(v) for v in shape)
        stride = tuple(contiguous_stride(shape) if stride is None else map(int, stride))
        if (not shape or any(d <= 0 for d in shape) or len(shape) != len(stride)
                or any(s < 0 for s in stride)):
            raise ValueError(f"非法张量视图: {shape}, {stride}")
        value = Tensor(storage, shape, stride, dtype, int(offset))
        if value.offset < 0 or value.offset % value.itemsize or value.offset + value.span > storage.size:
            raise ValueError(f"张量视图越过底层存储: {storage.name}")
        return value

    def has_weight(self, name):
        return name in self.model_info["weights"]

    def weight(self, name, role="weight"):
        entry = self.model_info["weights"][name][role]
        self.used_weights.add((name, role))
        key = (entry["path"], entry["dtype"])
        if key not in self.weights:
            path = (self.reference_root / entry["path"]).resolve()
            if not path.is_relative_to(self.reference_root):
                raise ValueError("权重路径离开冻结材料目录")
            if path.stat().st_size != entry["bytes"]:
                raise ValueError(f"权重长度改变: {name}")
            self.weights[key] = self._storage(self.program["GPGPU_MODEL_ARENA_WEIGHTS"],
                                               entry["bytes"], name, source=entry)
        return self._tensor(self.weights[key], entry["shape"], entry["dtype"])

    def const(self, name, array):
        import numpy as np
        value = np.asarray(array)
        if value.dtype.kind == "f":
            value = value.astype("<f4")
            dtype = "float32"
        elif value.dtype == np.int8:
            dtype = "int8"
        else:
            value = value.astype("<i8")
            dtype = "int64"
        value = np.ascontiguousarray(value)
        digest = hashlib.sha256(memoryview(value).cast("B")).hexdigest()
        key = (digest, tuple(value.shape), dtype)
        if key not in self.constants:
            self.constants[key] = self._storage(self.program["GPGPU_MODEL_ARENA_INPUT"],
                                                 value.nbytes, name, data=value)
        return self._tensor(self.constants[key], value.shape, dtype)

    def reserve_input(self, name, shape, dtype="int64"):
        import numpy as np
        return self.const(name, np.zeros(shape, dtype={"int64": "<i8", "float32": "<f4"}[dtype]))

    def persistent(self, name, shape, dtype="float32"):
        size = math.prod(shape) * {"float32": 4, "int64": 8}[dtype]
        storage = self._storage(self.program["GPGPU_MODEL_ARENA_KV"], size, name)
        return self._tensor(storage, shape, dtype)

    def view(self, tensor, shape, stride=None, offset=0):
        return self._tensor(tensor.storage, shape, tensor.dtype, stride,
                            tensor.offset + offset)

    def reshape(self, tensor, shape):
        shape = list(shape)
        if shape.count(-1) > 1:
            raise ValueError("reshape 只能推导一个维度")
        if -1 in shape:
            known = math.prod(d for d in shape if d != -1)
            if tensor.elements % known:
                raise ValueError("reshape 元素数不整除")
            shape[shape.index(-1)] = tensor.elements // known
        if math.prod(shape) != tensor.elements:
            raise ValueError("reshape 改变元素数")
        tensor = self.contiguous(tensor)
        return self.view(tensor, shape)

    def permute(self, tensor, axes):
        axes = tuple(a % len(tensor.shape) for a in axes)
        if sorted(axes) != list(range(len(tensor.shape))):
            raise ValueError("permute 轴不完整")
        return self.view(tensor, [tensor.shape[a] for a in axes],
                         [tensor.stride[a] for a in axes])

    def slice(self, tensor, axis, start, length):
        axis %= len(tensor.shape)
        if start < 0 or length <= 0 or start + length > tensor.shape[axis]:
            raise ValueError("slice 越界")
        shape = list(tensor.shape)
        shape[axis] = length
        return self.view(tensor, shape, tensor.stride,
                         start * tensor.stride[axis] * tensor.itemsize)

    def contiguous(self, tensor, name="copy"):
        if tensor.contiguous:
            return tensor
        if len(tensor.shape) > self.abi["GPGPU_TENSOR_MAX_RANK"]:
            raise ValueError("布局复制超过共享 rank 上限")
        mode = self.abi["GPGPU_EVAL_COPY_I64" if tensor.dtype == "int64" else
                        "GPGPU_EVAL_COPY_F32"]
        return self.op("copy", [tensor], tensor.shape,
                       [mode, len(tensor.shape), *tensor.shape, *tensor.stride],
                       name=name, dtype=tensor.dtype)

    def begin_round(self, name, kind):
        if self.current is not None:
            raise ValueError("上一轮未结束")
        self.current = Round(name, kind)
        self.rounds.append(self.current)
        return self.current

    def end_round(self, output=None, token_slot=None, checks=()):
        if self.current is None:
            raise ValueError("没有正在构建的轮")
        self.current.output = self.contiguous(output) if output is not None else None
        self.current.token_slot = token_slot
        # 正式图不保留诊断引用，也不为检查点布局增加复制节点。
        self.current.checks = ([(name, self.contiguous(tensor, name="check-copy"))
                                for name, tensor in checks] if self.capture_checks else [])
        last = len(self.nodes)
        for tensor in [self.current.output, *(t for _, t in self.current.checks)]:
            if tensor is not None:
                tensor.storage.last = max(tensor.storage.last, last)
        self.current = None

    def _scratch(self, index, elements):
        if self.scratch[index] is None:
            self.scratch[index] = self._storage(self.program["GPGPU_MODEL_ARENA_SCRATCH"],
                                                 elements * 4, f"scratch-{index}")
        storage = self.scratch[index]
        storage.size = max(storage.size, elements * 4)
        return self._tensor(storage, (elements,))

    def op(self, op, inputs, shape, dims=(), name="", dtype="float32", output=None):
        if self.current is None:
            raise ValueError("节点必须位于一个前向轮内")
        aliases = {"maxpool": "maxpool2d", "nearest": "nearest2d", "boxdecode": "box_decode"}
        op = aliases.get(op, op)
        if "GPGPU_EVAL_" + op.upper() not in self.abi:
            raise ValueError(f"未知设备算子: {op}")
        inputs = list(inputs)
        if len(inputs) > self.input_count or len(dims) > self.dim_count:
            raise ValueError("算子输入或属性超过共享 ABI")
        if op not in ("copy", "bmm", "rope"):
            inputs = [self.contiguous(v, name="input-copy") if v is not None else None
                      for v in inputs]
        shape = tuple(shape)
        count = math.prod(shape)
        if output is None:
            storage = self._storage(self.program["GPGPU_MODEL_ARENA_ACTIVATION"],
                                    count * {"float32": 4, "int64": 8}[dtype], name or op)
            storage.producer = len(self.nodes)
            output = self._tensor(storage, shape, dtype)
        elif output.shape != shape or output.dtype != dtype or not output.contiguous:
            raise ValueError("算子输出必须是形状和类型匹配的连续区间")
        if not name:
            name = f"{self.current.name}.{op}-{len(self.current.nodes)}"
        name = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
        rows, work = 0, count
        scratch = []
        if op in ("softmax", "rmsnorm", "dfl"):
            if not dims or dims[0] <= 0:
                raise ValueError("复合归约缺少有效宽度")
            work = count * dims[0] if op == "dfl" else count
            if work % dims[0]:
                raise ValueError("复合归约宽度不整除")
            rows = work // dims[0]
            scratch = [self._scratch(0, rows)]
            if op in ("softmax", "dfl"):
                scratch.append(self._scratch(1, work))
            if op == "dfl":
                scratch.append(self._scratch(2, work))
        index = len(self.nodes)
        for value in inputs:
            if value is not None:
                value.storage.last = max(value.storage.last, index)
        output.storage.last = max(output.storage.last, index)
        node = Node(name, op, inputs, output, list(dims), rows, work, scratch, self.stage)
        self.nodes.append(node)
        self.current.nodes.append(node)
        return output

    def copy_to(self, tensor, destination, name="copy-to"):
        mode = self.abi["GPGPU_EVAL_COPY_I64" if tensor.dtype == "int64" else
                        "GPGPU_EVAL_COPY_F32"]
        dims = [mode, len(tensor.shape), *tensor.shape, *tensor.stride]
        return self.op("copy", [tensor], tensor.shape, dims, name, tensor.dtype,
                       output=destination)

    def activation(self, op, tensor, name=""):
        return self.op(op, [tensor], tensor.shape, name=name)

    def scale(self, tensor, value, name=""):
        return self.op("mul_scalar", [tensor], tensor.shape, [f32_bits(value)], name=name)

    def binary(self, op, a, b, name=""):
        shape = []
        rank = max(len(a.shape), len(b.shape))
        padded = [(1,) * (rank - len(v.shape)) + v.shape for v in (a, b)]
        for x, y in zip(*padded):
            if x != y and x != 1 and y != 1:
                raise ValueError("二元算子广播不兼容")
            shape.append(max(x, y))
        actual = []
        for tensor, dims in zip((a, b), padded):
            tensor = self.contiguous(tensor)
            # 只有前导轴广播才等价于 Kernel 的 idx%元素数；其他情况在设备展开。
            first = next((i for i, d in enumerate(dims) if d != 1), rank)
            periodic = list(dims[first:]) == shape[first:]
            if not periodic:
                strides = (0,) * (rank - len(tensor.shape)) + tensor.stride
                strides = [0 if d == 1 else s for d, s in zip(dims, strides)]
                tensor = self.contiguous(self.view(tensor, shape, strides), "broadcast-copy")
            actual.append(tensor)
        return self.op(op, actual, shape, [v.elements for v in actual], name=name)

    def concat(self, tensors, axis, name=""):
        if not tensors or len(tensors) > 6:
            raise ValueError("concat 输入数不受共享 ABI 支持")
        tensors = [self.contiguous(v) for v in tensors]
        shape = list(tensors[0].shape)
        axis %= len(shape)
        for value in tensors[1:]:
            if len(value.shape) != len(shape) or any(a != b for i, (a, b) in
                    enumerate(zip(shape, value.shape)) if i != axis):
                raise ValueError("concat 非拼接维不一致")
        shape[axis] = sum(v.shape[axis] for v in tensors)
        dims = [math.prod(shape[axis + 1:]), shape[axis], len(tensors),
                *(v.shape[axis] for v in tensors)]
        return self.op("concat", tensors, shape, dims, name=name)

    def bmm(self, a, b, name=""):
        if len(a.shape) != 3 or len(b.shape) != 3 or a.shape[2] != b.shape[1]:
            raise ValueError("BMM 形状不兼容")
        batch = max(a.shape[0], b.shape[0])
        if a.shape[0] not in (1, batch) or b.shape[0] not in (1, batch):
            raise ValueError("BMM batch 不兼容")
        sa, sb = list(a.stride), list(b.stride)
        if a.shape[0] == 1:
            sa[0] = 0
        if b.shape[0] == 1:
            sb[0] = 0
        dims = [batch, a.shape[1], b.shape[2], a.shape[2], *sa, *sb, 1]
        return self.op("bmm", [a, b], (batch, a.shape[1], b.shape[2]), dims, name=name)

    def softmax(self, tensor, axis=-1, name=""):
        axis %= len(tensor.shape)
        return self.op("softmax", [tensor], tensor.shape,
                       [tensor.shape[axis], math.prod(tensor.shape[axis + 1:])], name=name)

    def linear(self, tensor, prefix, w8=True, name=""):
        role = "q" if w8 else "weight"
        weight = self.weight(prefix + ".weight", role)
        n, k = weight.shape
        if tensor.shape[-1] != k:
            raise ValueError("Linear 输入与权重不匹配")
        scale = self.weight(prefix + ".weight", "scales") if w8 else None
        bias = self.weight(prefix + ".bias") if self.has_weight(prefix + ".bias") else None
        return self.op("linear_w8" if w8 else "linear_f32", [tensor, weight, scale, bias],
                       (*tensor.shape[:-1], n), [tensor.elements // k, n, k], name=name)

    def rmsnorm(self, tensor, prefix, eps, name=""):
        return self.op("rmsnorm", [tensor, self.weight(prefix + ".weight")], tensor.shape,
                       [tensor.shape[-1], 1, f32_bits(eps)], name=name)

    def conv(self, tensor, prefix, stride=1, padding=0, groups=1, dilation=1, name=""):
        weight = self.weight(prefix + ".weight")
        bias = self.weight(prefix + ".bias") if self.has_weight(prefix + ".bias") else None
        n, cin, hin, win = tensor.shape
        cout, per_group, kh, kw = weight.shape
        sh, sw = pair(stride)
        ph, pw = pair(padding)
        dh, dw = pair(dilation)
        if cin != per_group * groups or cout % groups:
            raise ValueError("Conv 分组形状不兼容")
        hout = (hin + 2 * ph - dh * (kh - 1) - 1) // sh + 1
        wout = (win + 2 * pw - dw * (kw - 1) - 1) // sw + 1
        dims = [cin, hin, win, cout, hout, wout, kh, kw, sh, sw, ph, pw, dh, dw, groups]
        return self.op("conv2d", [tensor, weight, bias], (n, cout, hout, wout), dims, name=name)

    def batchnorm(self, tensor, prefix, eps=1e-5, name=""):
        inputs = [tensor, *(self.weight(prefix + "." + role) for role in
                             ("weight", "bias", "running_mean", "running_var"))]
        return self.op("batchnorm", inputs, tensor.shape,
                       [tensor.shape[1], math.prod(tensor.shape[2:]), f32_bits(eps)], name=name)

    def maxpool(self, tensor, kernel, stride=1, padding=0, name=""):
        n, c, hin, win = tensor.shape
        kh, kw = pair(kernel)
        sh, sw = pair(stride)
        ph, pw = pair(padding)
        hout, wout = (hin + 2 * ph - kh) // sh + 1, (win + 2 * pw - kw) // sw + 1
        return self.op("maxpool2d", [tensor], (n, c, hout, wout),
                       [c, hin, win, hout, wout, kh, kw, sh, sw, ph, pw, 1, 1], name=name)

    def nearest(self, tensor, output_hw, name=""):
        hout, wout = map(int, output_hw)
        return self.op("nearest2d", [tensor], (*tensor.shape[:2], hout, wout),
                       [*tensor.shape[-2:], hout, wout], name=name)

    def _plan(self):
        if self.current is not None or not self.rounds:
            raise ValueError("程序没有完整前向轮")
        align = self.program["GPGPU_MODEL_ALIGNMENT"]
        activation = self.program["GPGPU_MODEL_ARENA_ACTIVATION"]
        for storage in self.storages:
            if storage.arena != activation:
                storage.offset = aligned(self.arena_sizes[storage.arena], align)
                self.arena_sizes[storage.arena] = storage.offset + storage.size
        active, free = [], []
        for index, node in enumerate(self.nodes):
            kept = []
            for storage in active:
                if storage.last < index:
                    free.append((storage.offset, aligned(storage.size, align)))
                else:
                    kept.append(storage)
            active = kept
            # 合并空闲相邻区间，否则多层相同形状也可能积累碎片。
            merged = []
            for offset, size in sorted(free):
                if merged and merged[-1][0] + merged[-1][1] == offset:
                    old, length = merged[-1]
                    merged[-1] = (old, length + size)
                else:
                    merged.append((offset, size))
            free = merged
            storage = node.output.storage
            if storage.arena != activation or storage.producer != index:
                continue
            size = aligned(storage.size, align)
            fits = [(length, slot) for slot, (_, length) in enumerate(free) if length >= size]
            if fits:
                _, slot = min(fits)
                storage.offset, length = free.pop(slot)
                if length > size:
                    free.append((storage.offset + size, length - size))
            else:
                storage.offset = aligned(self.arena_sizes[activation], align)
                self.arena_sizes[activation] = storage.offset + size
            active.append(storage)
        self._planned = True

    def address(self, tensor):
        if tensor is None:
            return 0
        if not self._planned:
            raise ValueError("地址必须在寿命规划完成后编码")
        shift = self.program["GPGPU_MODEL_ADDRESS_SHIFT"]
        offset = tensor.storage.offset + tensor.offset
        if offset + tensor.span > self.arena_sizes[tensor.storage.arena] or offset >= 1 << shift:
            raise ValueError("文件地址越界")
        return ((tensor.storage.arena + 1) << shift) | offset

    def write(self, output, model_id, eos=(), vocabulary=0, prompt_tokens=0,
              weights_directory=None):
        """发布新程序；客体请求引用已常驻的固定权重，只写本次输入。"""
        output = Path(output).resolve()
        output.mkdir(parents=True, exist_ok=False)
        (output / "data").mkdir()
        self._plan()
        loads = []
        for storage in self.storages:
            if storage.source is None and storage.data is None:
                continue
            if storage.source is not None:
                desc = storage.source
                source = (self.reference_root / desc["path"]).resolve()
                relative = "data/" + desc["sha256"] + ".bin"
                if weights_directory is None:
                    if sha256(source) != desc["sha256"]:
                        raise ValueError(f"权重散列变化: {storage.name}")
                    target = output / relative
                    if not target.exists():
                        try:
                            os.link(source, target)
                        except OSError as error:
                            if error.errno != errno.EXDEV:
                                raise
                            shutil.copyfile(source, target)
                else:
                    # 权重已由会话装载，后续请求不能复制、重签名或重传另一份。
                    fixed = Path(weights_directory).resolve()
                    if source != (fixed / relative).resolve():
                        raise ValueError(f"请求权重不属于初始模型包: {storage.name}")
            else:
                data = memoryview(storage.data).cast("B")
                digest = hashlib.sha256(data).hexdigest()
                relative = "data/" + digest + ".bin"
                target = output / relative
                if not target.exists():
                    target.write_bytes(data)
                desc = {"sha256": digest}
            loads.append({"address": ((storage.arena + 1) << self.program["GPGPU_MODEL_ADDRESS_SHIFT"])
                                     | storage.offset,
                          "bytes": storage.size, "path": relative, "sha256": desc["sha256"],
                          "name": storage.name})
        index = {"model": self.model, "model_id": model_id, "arena_sizes": self.arena_sizes,
                 "checkpoints": self.capture_checks,
                 "manifest": str(self.manifest_path), "manifest_sha256": self.manifest_sha256,
                 "program_header_sha256": sha256(PROGRAM_HEADER), "loads": loads,
                 "vocabulary": vocabulary, "eos": list(eos), "prompt_tokens": prompt_tokens,
                 "used_weights": sorted([list(v) for v in self.used_weights]), "rounds": []}
        path = output / "program.bin"
        with path.open("xb") as stream:
            def integers(*values):
                stream.write(struct.pack("<" + "Q" * len(values), *(int(v) for v in values)))

            def text(value):
                raw = value.encode()
                integers(len(raw))
                stream.write(raw)

            stream.write(self.program["GPGPU_MODEL_PROGRAM_MAGIC"].encode())
            integers(model_id, len(self.arena_sizes), len(self.rounds), len(loads),
                     len(eos), vocabulary, prompt_tokens)
            integers(*self.arena_sizes)
            integers(*eos)
            for load in loads:
                integers(load["address"], load["bytes"])
                text(load["path"])
            for round_index, current in enumerate(self.rounds):
                integers(current.kind, len(current.nodes), self.address(current.output),
                         current.output.elements if current.output else 0,
                         self.address(current.token_slot), len(current.checks))
                text(current.name)
                checks = []
                for name, tensor in current.checks:
                    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
                        raise ValueError("检查输出名称不合法")
                    integers(self.address(tensor), tensor.elements * tensor.itemsize)
                    text(name)
                    checks.append({"name": name, "shape": tensor.shape, "dtype": tensor.dtype,
                                   "path": f"round-{round_index:04d}.{name}.bin"})
                nodes = []
                for node in current.nodes:
                    integers(node.rows, node.work_count)
                    text(node.name)
                    words = [0] * (self.abi["GPGPU_EVAL_PARAM_BYTES"] // self.slot_bytes)
                    words[self.abi["GPGPU_EVAL_OP"] // self.slot_bytes] = self.abi["GPGPU_EVAL_" + node.op.upper()]
                    words[self.abi["GPGPU_EVAL_COUNT"] // self.slot_bytes] = node.output.elements
                    values = node.inputs + [None] * (self.input_count - len(node.inputs))
                    for i, value in enumerate(values):
                        words[self.abi["GPGPU_EVAL_IN0"] // self.slot_bytes + i] = self.address(value)
                    words[self.abi["GPGPU_EVAL_OUT"] // self.slot_bytes] = self.address(node.output)
                    start = self.abi["GPGPU_EVAL_DIMS"] // self.slot_bytes
                    words[start:start + len(node.dims)] = [int(v) & ((1 << 64) - 1) for v in node.dims]
                    scratch = node.scratch + [None] * (self.scratch_count - len(node.scratch))
                    for i, value in enumerate(scratch):
                        words[self.abi["GPGPU_EVAL_AUX0"] // self.slot_bytes + i] = self.address(value)
                    integers(*words)
                    integers(*(v.span if v else 0 for v in [*values, node.output, *scratch]))
                    nodes.append({"name": node.name, "op": node.op, "stage": node.stage,
                                  "shape": node.output.shape})
                index["rounds"].append({"name": current.name, "kind": current.kind,
                                        "checks": checks, "nodes": nodes,
                                        "output_shape": current.output.shape if current.output else None})
        index["program_sha256"] = sha256(path)
        (output / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n")
        return index
