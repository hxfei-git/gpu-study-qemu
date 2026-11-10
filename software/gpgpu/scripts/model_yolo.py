# SPDX-License-Identifier: GPL-2.0-or-later
"""YOLO11n：具名原始权重、受控设备图与独立的 CPU 参考和后处理。"""

import hashlib
import json
import math
import time
from pathlib import Path

from gpgpu_model_eval import f32_bits as _f32_bits


_INPUT_HW = (320, 320)
_INPUT_SHAPE = (1, 3, *_INPUT_HW)
_PREDICTION_SHAPE = (1, 84, 2100)


def _configuration():
    """只读取已安装包内的官方配置，不加载 checkpoint，也不访问网络。"""
    import ultralytics
    from ultralytics.utils import yaml_load

    root = Path(ultralytics.__file__).resolve().parent
    config = yaml_load(root / "cfg/models/11/yolo11.yaml")
    config["scale"] = "n"
    return config, root


class _YoloLowering:
    """逐个展开已知模块；这里只读取结构属性，不调用模块 forward。"""

    def __init__(self, graph, weights):
        self.graph = graph
        self.weights = weights
        self.used_weights = set()

    def require(self, names):
        for name in names:
            if name not in self.weights or not self.graph.has_weight(name):
                raise ValueError(f"YOLO 缺少推理权重 {name}")
            self.used_weights.add(name)

    def conv(self, module, x, prefix):
        names = [prefix + ".weight"]
        if module.bias is not None:
            names.append(prefix + ".bias")
        self.require(names)
        return self.graph.conv(
            x, prefix, stride=module.stride, padding=module.padding,
            groups=module.groups, dilation=module.dilation, name=prefix,
        )

    def attention(self, module, x, prefix):
        g = self.graph
        batch, channels, height, width = x.shape
        heads, key, value = module.num_heads, module.key_dim, module.head_dim
        tokens = height * width
        qkv = self.lower(module.qkv, x, prefix + ".qkv")
        qkv = g.reshape(qkv, (batch, heads, 2 * key + value, tokens))
        q = g.slice(qkv, 2, 0, key)
        k = g.slice(qkv, 2, key, key)
        v = g.slice(qkv, 2, 2 * key, value)

        # 各 head 的切片并不连续；reshape 由 builder 决定是否在设备上复制。
        q = g.reshape(g.permute(q, (0, 1, 3, 2)), (batch * heads, tokens, key))
        k = g.reshape(k, (batch * heads, key, tokens))
        scores = g.bmm(q, k, name=prefix + ".qk")
        scores = g.scale(scores, module.scale, name=prefix + ".scale")
        probabilities = g.softmax(scores, axis=-1, name=prefix + ".softmax")
        values = g.reshape(v, (batch * heads, value, tokens))
        attended = g.bmm(values, g.permute(probabilities, (0, 2, 1)),
                         name=prefix + ".av")
        attended = g.reshape(attended, (batch, channels, height, width))
        position = self.lower(module.pe, g.reshape(v, x.shape), prefix + ".pe")
        combined = g.binary("add", attended, position, name=prefix + ".position_add")
        return self.lower(module.proj, combined, prefix + ".proj")

    def detect(self, module, xs, prefix):
        g = self.graph
        raw_heads, anchors, strides = [], [], []
        for index, x in enumerate(xs):
            box = self.lower(module.cv2[index], x, f"{prefix}.cv2.{index}")
            cls = self.lower(module.cv3[index], x, f"{prefix}.cv3.{index}")
            raw = g.concat([box, cls], 1, name=f"{prefix}.raw{index}")
            raw_heads.append(raw)
            height, width = raw.shape[-2:]
            if _INPUT_HW[0] % height or _INPUT_HW[1] % width:
                raise ValueError("YOLO 特征图不能整除输入图像尺寸")
            stride = _INPUT_HW[0] // height
            if stride != _INPUT_HW[1] // width:
                raise ValueError("YOLO 横纵 stride 不一致")

            # 网格坐标只依赖 shape，但仍由设备生成，不能从 CPU 参考复制中间值。
            # 类型值引用协议头的唯一常量，FP32 标量按 ABI 传原始位。
            scalar_type = g.abi["GPGPU_EVAL_TYPE_F32"]
            axis_x = g.op("arange", [], (width,),
                          dims=(_f32_bits(0.5), _f32_bits(1.0), scalar_type),
                          name=f"{prefix}.anchors{index}.x")
            axis_y = g.op("arange", [], (height,),
                          dims=(_f32_bits(0.5), _f32_bits(1.0), scalar_type),
                          name=f"{prefix}.anchors{index}.y")
            grid_x = g.contiguous(g.view(axis_x, (height, width), stride=(0, 1)),
                                  name=f"{prefix}.anchors{index}.grid_x")
            grid_y = g.contiguous(g.view(axis_y, (height, width), stride=(1, 0)),
                                  name=f"{prefix}.anchors{index}.grid_y")
            anchors.append(g.concat([g.reshape(grid_x, (1, height * width)),
                                     g.reshape(grid_y, (1, height * width))],
                                    0, name=f"{prefix}.anchors{index}.xy"))
            strides.append(g.op("full", [], (1, height * width),
                                dims=(_f32_bits(stride), _f32_bits(1.0), scalar_type),
                                name=f"{prefix}.stride{index}"))

        if [x.shape for x in raw_heads] != [(1, 144, 40, 40), (1, 144, 20, 20),
                                           (1, 144, 10, 10)]:
            raise ValueError("YOLO11n 320 的 Detect 原始输出尺寸不符")
        flat = g.concat([g.reshape(x, (x.shape[0], module.no, math.prod(x.shape[2:])))
                         for x in raw_heads], 2, name=prefix + ".all_heads")
        box = g.slice(flat, 1, 0, 4 * module.reg_max)
        cls = g.slice(flat, 1, 4 * module.reg_max, module.nc)
        batch, _, count = box.shape

        # 按官方 DFL 展开，使用该层真实的具名投影权重，而非独立算子样例。
        distribution = g.reshape(box, (batch, 4, module.reg_max, count))
        distribution = g.permute(distribution, (0, 2, 1, 3))
        distribution = g.softmax(distribution, axis=1, name=prefix + ".dfl.softmax")
        distances = self.conv(module.dfl.conv, distribution, prefix + ".dfl.conv")
        distances = g.reshape(distances, (batch, 4, count))
        anchor_points = g.concat(anchors, 1, name=prefix + ".anchors")
        stride_values = g.concat(strides, 1, name=prefix + ".strides")
        decoded = g.op("box_decode", [distances, anchor_points], (batch, 4, count),
                       dims=(count, 0), name=prefix + ".box_decode")
        decoded = g.binary("mul", decoded, stride_values, name=prefix + ".pixel_boxes")
        cls = g.activation("sigmoid", cls, name=prefix + ".class_probabilities")
        output = g.concat([decoded, cls], 1, name=prefix + ".predictions")
        return {"output": output, "raw_heads": raw_heads}

    def lower(self, module, x, prefix):
        g = self.graph
        kind = type(module).__name__
        if kind in ("Conv", "DWConv"):
            x = self.conv(module.conv, x, prefix + ".conv")
            self.require([prefix + ".bn." + field for field in
                          ("weight", "bias", "running_mean", "running_var")])
            x = g.batchnorm(x, prefix + ".bn", eps=module.bn.eps, name=prefix + ".bn")
            activation = type(module.act).__name__
            if activation == "SiLU":
                return g.activation("silu", x, name=prefix + ".silu")
            if activation == "Identity":
                return x
            raise ValueError(f"YOLO 未支持的激活 {activation}")
        if kind == "Conv2d":
            return self.conv(module, x, prefix)
        if kind == "Sequential":
            for name, child in module.named_children():
                x = self.lower(child, x, prefix + "." + name)
            return x
        if kind == "Bottleneck":
            y = self.lower(module.cv1, x, prefix + ".cv1")
            y = self.lower(module.cv2, y, prefix + ".cv2")
            return g.binary("add", x, y, name=prefix + ".residual") if module.add else y
        if kind == "C3k2":
            y = self.lower(module.cv1, x, prefix + ".cv1")
            parts = [g.slice(y, 1, 0, module.c), g.slice(y, 1, module.c, module.c)]
            for index, child in enumerate(module.m):
                parts.append(self.lower(child, parts[-1], f"{prefix}.m.{index}"))
            return self.lower(module.cv2, g.concat(parts, 1, name=prefix + ".concat"),
                              prefix + ".cv2")
        if kind == "C3k":
            left = self.lower(module.cv1, x, prefix + ".cv1")
            left = self.lower(module.m, left, prefix + ".m")
            right = self.lower(module.cv2, x, prefix + ".cv2")
            return self.lower(module.cv3, g.concat([left, right], 1, name=prefix + ".concat"),
                              prefix + ".cv3")
        if kind == "SPPF":
            parts = [self.lower(module.cv1, x, prefix + ".cv1")]
            for index in range(3):
                parts.append(g.maxpool(parts[-1], module.m.kernel_size,
                                       stride=module.m.stride, padding=module.m.padding,
                                       name=f"{prefix}.maxpool{index}"))
            return self.lower(module.cv2, g.concat(parts, 1, name=prefix + ".concat"),
                              prefix + ".cv2")
        if kind == "C2PSA":
            y = self.lower(module.cv1, x, prefix + ".cv1")
            a, b = g.slice(y, 1, 0, module.c), g.slice(y, 1, module.c, module.c)
            b = self.lower(module.m, b, prefix + ".m")
            return self.lower(module.cv2, g.concat([a, b], 1, name=prefix + ".concat"),
                              prefix + ".cv2")
        if kind == "PSABlock":
            y = self.lower(module.attn, x, prefix + ".attn")
            x = g.binary("add", x, y, name=prefix + ".attention_residual") if module.add else y
            y = self.lower(module.ffn, x, prefix + ".ffn")
            return g.binary("add", x, y, name=prefix + ".ffn_residual") if module.add else y
        if kind == "Attention":
            return self.attention(module, x, prefix)
        if kind == "Upsample":
            if module.mode != "nearest" or module.size is not None:
                raise ValueError("YOLO 仅支持配置中的 nearest 倍率上采样")
            scale = module.scale_factor
            scale = (scale, scale) if isinstance(scale, (int, float)) else scale
            size = tuple(int(dimension * factor) for dimension, factor in zip(x.shape[-2:], scale))
            return g.nearest(x, size, name=prefix + ".nearest")
        if kind == "Concat":
            return g.concat(x, module.d, name=prefix + ".concat")
        if kind == "Detect":
            return self.detect(module, x, prefix)
        raise ValueError(f"YOLO 未支持的官方模块 {prefix}: {kind}")


def build_yolo(graph, manifest_model):
    """将官方 YOLO11n 结构展开为完整设备前向图；不读取 CPU 参考输出。"""
    from ultralytics.nn.tasks import parse_model
    from ultralytics.utils.torch_utils import initialize_weights

    if graph.input_tensor.shape != _INPUT_SHAPE or graph.input_tensor.dtype != "float32":
        raise ValueError("YOLO 输入必须是 NCHW FP32 [1,3,320,320]")
    if manifest_model.get("fused_conv_bn", False):
        raise ValueError("YOLO 适配器要求未融合的 Conv/BN 原始权重")
    config, _ = _configuration()
    modules, _ = parse_model(config, ch=3, verbose=False)
    # DetectionModel 同样会调用它；尤其不能沿用 nn.BatchNorm2d 默认的 eps。
    initialize_weights(modules)
    lowering = _YoloLowering(graph, manifest_model["weights"])
    outputs = []
    x = graph.input_tensor
    for module in modules:
        if type(module).__name__ == "Detect":
            graph.stage = "detect"
        elif type(module).__name__ == "C2PSA":
            graph.stage = "attention"
        else:
            graph.stage = "backbone" if module.i < len(config["backbone"]) else "neck"
        if module.f != -1:
            x = (outputs[module.f] if isinstance(module.f, int) else
                 [x if index == -1 else outputs[index] for index in module.f])
        x = lowering.lower(module, x, f"yolo.model.{module.i}")
        outputs.append(x)
    unused = set(manifest_model["weights"]) - lowering.used_weights
    if unused:
        raise ValueError(f"YOLO 图未覆盖推理权重: {sorted(unused)}")
    if not isinstance(x, dict) or x["output"].shape != _PREDICTION_SHAPE:
        raise ValueError("YOLO11n 320 的最终输出必须是 [1,84,2100]")
    return x


def preprocess_image(image_path):
    """实际读取 RGB 图像，执行与原导出一致的 OpenCV 线性 LetterBox。"""
    import numpy as np
    from PIL import Image
    from ultralytics.data.augment import LetterBox

    started = time.perf_counter()
    image_path = Path(image_path).resolve()
    with Image.open(image_path) as source:
        image = np.asarray(source.convert("RGB"))
    height, width = image.shape[:2]
    ratio = min(_INPUT_HW[0] / height, _INPUT_HW[1] / width)
    unpad_width, unpad_height = round(width * ratio), round(height * ratio)
    dw, dh = (_INPUT_HW[1] - unpad_width) / 2, (_INPUT_HW[0] - unpad_height) / 2
    left, right = round(dw - 0.1), round(dw + 0.1)
    top, bottom = round(dh - 0.1), round(dh + 0.1)
    letterbox = LetterBox(new_shape=_INPUT_HW, auto=False, scaleFill=False,
                         scaleup=True, center=True, stride=32)
    image = letterbox(image=image)
    value = np.ascontiguousarray(image.transpose(2, 0, 1)[None], dtype=np.float32)
    value /= np.float32(255.0)
    if value.shape != _INPUT_SHAPE:
        raise ValueError("YOLO LetterBox 没有产生预期输入尺寸")
    metadata = {
        "image_path": str(image_path), "original_shape": [height, width, 3],
        "input_shape": list(value.shape), "dtype": "float32", "color": "RGB",
        "ratio": [ratio, ratio], "pad": [left, top],
        "padding_ltrb": [left, top, right, bottom],
        "letterbox": {"shape": list(_INPUT_HW), "auto": False, "center": True,
                      "scale_fill": False, "scaleup": True, "border_value": 114,
                      "resize": "OpenCV INTER_LINEAR", "normalization": "float32 / 255"},
        "preprocess_seconds": time.perf_counter() - started,
    }
    return value, metadata


def _class_names():
    from ultralytics.utils import yaml_load

    _, root = _configuration()
    names = yaml_load(root / "cfg/datasets/coco.yaml")["names"]
    return dict(enumerate(names)) if isinstance(names, list) else {int(k): v for k, v in names.items()}


def _write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def postprocess(predictions, image_path, metadata, output_dir, conf=0.25, iou=0.7, max_det=300):
    """仅对传入的预测执行官方 CPU NMS；这里不会构造或运行网络。"""
    import numpy as np
    import torch
    from PIL import Image, ImageDraw
    from ultralytics.utils.ops import non_max_suppression, scale_boxes

    names = _class_names()
    started = time.perf_counter()
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if isinstance(predictions, (str, Path)):
        predictions = np.fromfile(predictions, dtype="<f4").reshape(_PREDICTION_SHAPE)
    if isinstance(predictions, torch.Tensor):
        prediction = predictions.detach().to(device="cpu", dtype=torch.float32).clone()
    else:
        prediction = torch.from_numpy(np.array(predictions, dtype=np.float32, order="C", copy=True))
    if tuple(prediction.shape) != _PREDICTION_SHAPE or not torch.isfinite(prediction).all():
        raise ValueError("YOLO 后处理需要有限的 FP32 [1,84,2100] 预测")
    nms_started = time.perf_counter()
    with torch.no_grad():
        detected = non_max_suppression(prediction, conf_thres=conf, iou_thres=iou,
                                       max_det=max_det, nc=len(names), agnostic=False,
                                       multi_label=False, in_place=False)[0]
        if len(detected):
            detected[:, :4] = scale_boxes(_INPUT_HW, detected[:, :4], metadata["original_shape"],
                                          ratio_pad=(metadata["ratio"], metadata["pad"]))
    nms_seconds = time.perf_counter() - nms_started
    detections = []
    for row in detected.tolist():
        class_id = int(row[5])
        detections.append({"class_id": class_id, "class_name": names[class_id],
                           "confidence": row[4], "box_xyxy": row[:4]})
    with Image.open(image_path) as source:
        image = source.convert("RGB")
    if list(image.size[::-1]) != list(metadata["original_shape"][:2]):
        raise ValueError("YOLO 后处理图像尺寸与预处理元数据不一致")
    draw = ImageDraw.Draw(image)
    for detection in detections:
        box = detection["box_xyxy"]
        draw.rectangle(box, outline=(220, 40, 40), width=max(1, min(image.size) // 300))
        label = f"{detection['class_name']} {detection['confidence']:.3f}"
        draw.text((max(0, box[0]), max(0, box[1] - 13)), label, fill=(220, 40, 40))
    image.save(output_dir / "annotated.jpg", quality=90)
    result = {
        "image_path": str(Path(image_path).resolve()),
        "original_shape": metadata["original_shape"],
        "prediction_shape": list(_PREDICTION_SHAPE), "conf": conf, "iou": iou, "max_det": max_det,
        "count": len(detections), "detections": detections,
        "classes": {str(class_id): names[class_id] for class_id in
                    sorted({item["class_id"] for item in detections})},
        "nms_seconds": nms_seconds, "postprocess_seconds": time.perf_counter() - started,
        "artifacts": {"detections": "detections.json", "annotated": "annotated.jpg"},
    }
    _write_json(output_dir / "detections.json", result)
    return result


def _load_raw_weights(model, manifest_path, info):
    """所有推理参数必须由 raw 覆盖；只有不参与 eval 的 BN 计数器允许补零。"""
    import numpy as np
    import torch

    root = Path(manifest_path).resolve().parent
    state = model.state_dict()
    expected = {"yolo." + name for name in state if not name.endswith(".num_batches_tracked")}
    supplied = set(info["weights"])
    if supplied != expected:
        raise ValueError(f"YOLO 推理权重不完整: 缺少 {sorted(expected - supplied)}；"
                         f"多出 {sorted(supplied - expected)}")
    loaded, zeroed, total_bytes = {}, [], 0
    for name, target in state.items():
        if name.endswith(".num_batches_tracked"):
            loaded[name] = torch.zeros_like(target)
            zeroed.append(name)
            continue
        desc = info["weights"]["yolo." + name]["weight"]
        path = (root / desc["path"]).resolve()
        if not path.is_relative_to(root):
            raise ValueError(f"YOLO raw 权重越出 manifest 目录: {name}")
        if desc["dtype"] != "float32" or tuple(desc["shape"]) != tuple(target.shape):
            raise ValueError(f"YOLO raw 权重类型或尺寸不符: {name}")
        data = path.read_bytes()
        if len(data) != target.numel() * 4 or hashlib.sha256(data).hexdigest() != desc["sha256"]:
            raise ValueError(f"YOLO raw 权重长度或 SHA256 不符: {name}")
        loaded[name] = torch.from_numpy(np.frombuffer(data, dtype="<f4").copy().reshape(target.shape))
        total_bytes += len(data)
    result = model.load_state_dict(loaded, strict=True)
    if result.missing_keys or result.unexpected_keys:
        raise ValueError("YOLO strict load 未完整覆盖网络")
    return {"raw_tensors": len(supplied), "raw_bytes": total_bytes,
            "zeroed_training_buffers": zeroed, "strict": True,
            "loading": "只读取 manifest 具名 raw 权重；不反序列化 .pt"}


def _save_tensor(path, value):
    import numpy as np

    value = np.ascontiguousarray(value, dtype="<f4")
    value.tofile(path)
    return {"path": Path(path).name, "shape": list(value.shape), "dtype": "float32",
            "layout": "C", "byte_order": "little", "bytes": value.nbytes}


def cpu_reference(manifest_path, image_path, output_dir, conf=0.25, iou=0.7, max_det=300):
    """独立生成 CPU 对照；这些中间值和预测不作为设备图的输入。"""
    import torch
    import ultralytics
    from ultralytics.nn.tasks import DetectionModel

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = Path(manifest_path).resolve()
    # 摘要与解析共用同一份字节，不在前向结束后按路径重读并签入另一版清单。
    manifest_data = manifest_path.read_bytes()
    manifest_sha256 = hashlib.sha256(manifest_data).hexdigest()
    info = json.loads(manifest_data)["models"]["yolo"]
    if info.get("fused_conv_bn", False):
        raise ValueError("YOLO CPU 参考要求未融合的原始权重")
    started = time.perf_counter()
    config, _ = _configuration()
    # 官方构造函数用零输入求 stride；这是 CPU 准备开销，不是设备前向。
    with torch.no_grad():
        model = DetectionModel(config, ch=3, nc=80, verbose=False).float().eval()
    coverage = _load_raw_weights(model, manifest_path, info)
    model_prepare_seconds = time.perf_counter() - started
    value, metadata = preprocess_image(image_path)
    started = time.perf_counter()
    with torch.inference_mode():
        predictions, raw_heads = model(torch.from_numpy(value))
    forward_seconds = time.perf_counter() - started
    if tuple(predictions.shape) != _PREDICTION_SHAPE or [tuple(x.shape) for x in raw_heads] != [
            (1, 144, 40, 40), (1, 144, 20, 20), (1, 144, 10, 10)]:
        raise ValueError("YOLO CPU 参考输出尺寸不符")
    tensors = {
        "input": _save_tensor(output_dir / "input.bin", value),
        "raw_heads": [_save_tensor(output_dir / f"raw{index}.bin", x.cpu().numpy())
                      for index, x in enumerate(raw_heads)],
        "predictions": _save_tensor(output_dir / "predictions.bin", predictions.cpu().numpy()),
    }
    detection = postprocess(predictions, image_path, metadata, output_dir, conf, iou, max_det)
    result = {
        "schema_version": 1, "model": "yolo11n", "precision": "float32",
        "implementation": {"ultralytics": ultralytics.__version__, "torch": torch.__version__},
        "manifest_path": str(manifest_path), "manifest_sha256": manifest_sha256,
        "weights": coverage,
        "input_metadata": metadata, "tensors": tensors, "postprocess": detection,
        "timings": {"model_prepare_seconds": model_prepare_seconds,
                    "preprocess_seconds": metadata["preprocess_seconds"],
                    "forward_seconds": forward_seconds,
                    "postprocess_seconds": detection["postprocess_seconds"]},
        "boundary": "CPU 参考独立保存；准备阶段包含官方求 stride 的零输入前向；未执行设备",
    }
    _write_json(output_dir / "metadata.json", result)
    return result
