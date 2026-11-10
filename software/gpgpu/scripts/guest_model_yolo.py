# SPDX-License-Identifier: GPL-2.0-or-later
"""客体 YOLO 图像前后处理：绑定输入图像字节，执行 CPU 图像操作与 NumPy NMS。

输入尺寸与输出形状由应用包的调用方传入；本模块不加载或运行模型。
"""

import hashlib
import json
import math
import time
from collections.abc import Mapping
from os import PathLike
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw


# 固定 Ultralytics 8.3.9 参考的类别坐标偏移步长，单位为输入图像像素。
_NMS_MAX_WH = 7680


def _read_image(image_path, expected_sha256=None):
    """一次读取字节，用同一份字节校验 SHA256 并按 BGR 解码，不应用 EXIF 旋转。"""
    image_path = Path(image_path).resolve()
    if not image_path.is_file():
        raise FileNotFoundError(f"YOLO 图像不存在或不是普通文件: {image_path}")
    source = image_path.read_bytes()
    source_sha256 = hashlib.sha256(source).hexdigest()
    if expected_sha256 is not None and source_sha256 != expected_sha256:
        raise ValueError(f"YOLO 图像内容与预处理时不一致，拒绝标注: {image_path}")
    try:
        image = cv2.imdecode(np.frombuffer(source, dtype=np.uint8),
                             cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
    except cv2.error as error:
        raise ValueError(f"OpenCV 无法解码 YOLO 图像: {image_path}") from error
    if image is None:
        raise ValueError(f"OpenCV 无法解码 YOLO 图像: {image_path}")
    return image_path, image, source_sha256


def preprocess_image(image_path, size):
    """返回连续的 FP32 RGB NCHW 数组与可直接 JSON 序列化的元数据。

    size 为必需的（高，宽），由调用方从应用包 input_shape 的末两维传入。
    同一份原始字节用于 SHA256 与解码，source_sha256 供后处理拒绝换图。
    ratio 保留旧元数据的名义等比倍率；resize_ratio 按实际整数 resize 尺寸
    保存（横，纵）倍率，反还原优先使用它。pad 为（左，上），padding_ltrb
    为（左，上，右，下）。preprocess_seconds 包含读取、哈希、解码、resize
    和布局转换。
    """
    started = time.perf_counter()
    try:
        target_height, target_width = size
    except (TypeError, ValueError) as error:
        raise ValueError("YOLO size 必须是（高，宽）两个正整数") from error
    if any(not isinstance(value, (int, np.integer)) or value <= 0
           for value in (target_height, target_width)):
        raise ValueError("YOLO size 必须是（高，宽）两个正整数")
    target_height, target_width = int(target_height), int(target_width)

    image_path, image, source_sha256 = _read_image(image_path)
    height, width = image.shape[:2]
    ratio = min(target_height / height, target_width / width)
    resized_width, resized_height = round(width * ratio), round(height * ratio)
    if not resized_width or not resized_height:
        raise ValueError("YOLO 图像长宽比导致 LetterBox resize 尺寸为零")
    dw, dh = (target_width - resized_width) / 2, (target_height - resized_height) / 2
    left, right = round(dw - 0.1), round(dw + 0.1)
    top, bottom = round(dh - 0.1), round(dh + 0.1)
    if (resized_width, resized_height) != (width, height):
        image = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
    image = cv2.copyMakeBorder(image, top, bottom, left, right, cv2.BORDER_CONSTANT,
                               value=(114, 114, 114))
    value = np.ascontiguousarray(image[:, :, ::-1].transpose(2, 0, 1)[None], dtype=np.float32)
    value /= np.float32(255.0)
    metadata = {
        "image_path": str(image_path), "source_sha256": source_sha256,
        "original_shape": [height, width, 3],
        "input_shape": list(value.shape), "dtype": "float32", "color": "RGB",
        "ratio": [ratio, ratio],
        "resize_ratio": [resized_width / width, resized_height / height],
        "resized_shape": [resized_height, resized_width, 3],
        "pad": [left, top], "padding_ltrb": [left, top, right, bottom],
        "letterbox": {"shape": [target_height, target_width], "auto": False, "center": True,
                      "scale_fill": False, "scaleup": True, "border_value": 114,
                      "resize": "OpenCV INTER_LINEAR", "normalization": "float32 / 255"},
        "preprocess_seconds": time.perf_counter() - started,
    }
    return value, metadata


def _read_predictions(predictions_or_path, output_shape):
    """按调用方形状读取小端 FP32；拒绝 NaN/Inf，不静默丢弃候选框。"""
    try:
        output_shape = tuple(output_shape)
    except TypeError as error:
        raise ValueError("YOLO output_shape 必须是正整数 [1,4+类别数,候选数]") from error
    if (len(output_shape) != 3
            or any(not isinstance(value, (int, np.integer)) or value <= 0 for value in output_shape)
            or output_shape[0] != 1 or output_shape[1] <= 4):
        raise ValueError("YOLO output_shape 必须是正整数 [1,4+类别数,候选数]")
    output_shape = tuple(int(value) for value in output_shape)
    if isinstance(predictions_or_path, (str, PathLike)):
        path = Path(predictions_or_path)
        expected_bytes = math.prod(output_shape) * np.dtype("<f4").itemsize
        if path.stat().st_size != expected_bytes:
            raise ValueError(f"YOLO 预测文件必须恰好包含 {expected_bytes} 字节的小端 FP32")
        prediction = np.fromfile(path, dtype="<f4").reshape(output_shape)
    else:
        prediction = np.asarray(predictions_or_path, dtype=np.float32)
    if prediction.shape != output_shape or not np.isfinite(prediction).all():
        raise ValueError(f"YOLO 后处理需要有限的 FP32 {list(output_shape)} 预测")
    if np.any(prediction[:, 2:4] < 0):
        raise ValueError("YOLO xywh 预测的宽和高不能为负数")
    return prediction


def _nms(prediction, conf, iou, max_det):
    """复用 Ultralytics 8.3.9 的类别相关、single-label NMS 数值路径。

    score 必须严格大于 conf，每框只保留最大类别；同分稳定保留原候选顺序。
    类别编号乘固定 max_wh 后按 FP32 加到框坐标，再计算 FP32 面积和 IoU，
    与固定 CPU 参考采用同一类别隔离路径，不能改成未偏移坐标的类别内 IoU。
    IoU 严格大于 iou 才抑制，零面积框的 IoU 取 0；返回未偏移的预测坐标，
    不修改调用者数组。不同 Torchvision 实现的同分选择仍不作跨平台保证。
    """
    rows = prediction[0].T
    classes = np.argmax(rows[:, 4:], axis=1)
    scores = rows[np.arange(len(rows)), classes + 4]
    selected = np.flatnonzero(scores > conf)
    rows, scores, classes = rows[selected], scores[selected], classes[selected]
    boxes = np.empty((len(rows), 4), dtype=np.float32)
    half_size = rows[:, 2:4] / np.float32(2.0)
    boxes[:, :2] = rows[:, :2] - half_size
    boxes[:, 2:] = rows[:, :2] + half_size
    offsets = classes.astype(np.float32) * np.float32(_NMS_MAX_WH)
    nms_boxes = boxes + offsets[:, None]
    areas = np.prod(nms_boxes[:, 2:] - nms_boxes[:, :2], axis=1)
    order = np.argsort(-scores, kind="stable")
    kept = []
    while len(order) and len(kept) < max_det:
        index = order[0]
        kept.append(index)
        rest = order[1:]
        intersection_size = np.maximum(
            np.minimum(nms_boxes[index, 2:], nms_boxes[rest, 2:])
            - np.maximum(nms_boxes[index, :2], nms_boxes[rest, :2]), np.float32(0.0),
        )
        intersection = np.prod(intersection_size, axis=1)
        union = areas[index] + areas[rest] - intersection
        overlap = np.divide(intersection, union, out=np.zeros_like(intersection), where=union > 0)
        # Torchvision CPU 的阈值为 double；这里只提升比较，不改变 FP32 IoU。
        order = rest[~(overlap.astype(np.float64) > iou)]
    kept = np.asarray(kept, dtype=np.intp)
    return boxes[kept], scores[kept], classes[kept]


def _restore_boxes(boxes, metadata, image_shape):
    """新元数据使用实际横纵倍率；旧 ratio/pad 元数据沿用原参考的还原语义。"""
    if list(metadata.get("original_shape", ())) != list(image_shape):
        raise ValueError("YOLO 后处理图像尺寸与预处理元数据不一致")
    ratio = np.asarray(metadata.get("resize_ratio", metadata.get("ratio")), dtype=np.float32)
    pad = np.asarray(metadata.get("pad"), dtype=np.float32)
    if (ratio.shape != (2,) or pad.shape != (2,) or not np.isfinite(ratio).all()
            or not np.isfinite(pad).all() or np.any(ratio <= 0) or np.any(pad < 0)):
        raise ValueError("YOLO 元数据需要有限的正横纵倍率 ratio 和非负（左，上）pad")
    boxes[:, 0::2] = (boxes[:, 0::2] - pad[0]) / ratio[0]
    boxes[:, 1::2] = (boxes[:, 1::2] - pad[1]) / ratio[1]
    boxes[:, 0::2] = np.clip(boxes[:, 0::2], 0, image_shape[1])
    boxes[:, 1::2] = np.clip(boxes[:, 1::2], 0, image_shape[0])
    return boxes


def _class_name(names, class_id):
    """名称由模型包传入；缺省用编号，不在客体复制一份 COCO 清单。"""
    if names is None:
        return str(class_id)
    try:
        if isinstance(names, Mapping):
            name = names[class_id] if class_id in names else names[str(class_id)]
        elif not isinstance(names, (str, bytes)):
            name = names[class_id]
        else:
            raise TypeError("names 不是名称序列或映射")
    except (IndexError, KeyError, TypeError) as error:
        raise ValueError(f"YOLO names 缺少类别 {class_id} 的名称") from error
    if not isinstance(name, str) or not name:
        raise ValueError(f"YOLO 类别 {class_id} 的名称必须是非空字符串")
    return name


def postprocess(predictions_or_path, image_path, metadata, output_dir,
                conf=0.25, iou=0.7, max_det=300, names=None, *, output_shape):
    """保存 detections.json、annotated.jpg 并返回检测字典，不运行 CPU 模型。

    output_shape 是必需关键词，由调用方传入应用包的 [1,4+类别数,候选数]。
    预测包含 xywh 和各类概率，没有 objectness；names 接受名称序列或以
    整数／JSON 字符串编号为键的映射。metadata 必须包含预处理记录的
    source_sha256；再次读取的字节必须相同，才能用这份字节解码并标注。
    输出父目录按需创建，结果文件仅独占新建；已有文件或链接一律拒绝，
    不能通过同名路径、硬链接或符号链接覆盖输入 JPEG。
    nms_seconds 只含筛选、xywh 转换和 NMS；postprocess_seconds 从入口计至
    标注图落盘，包含预测读取、图像读取校验和解码、坐标还原与标注，不含
    最后的 JSON 写入。输入及计算中出现非有限数或浮点溢出时直接报错，
    不保存部分检测结果。
    """
    started = time.perf_counter()
    conf, iou = float(conf), float(iou)
    if not math.isfinite(conf) or not 0 <= conf <= 1:
        raise ValueError("YOLO conf 必须是 [0,1] 内的有限数")
    if not math.isfinite(iou) or not 0 <= iou <= 1:
        raise ValueError("YOLO iou 必须是 [0,1] 内的有限数")
    if not isinstance(max_det, (int, np.integer)) or max_det < 0:
        raise ValueError("YOLO max_det 必须是非负整数")
    max_det = int(max_det)
    prediction = _read_predictions(predictions_or_path, output_shape)
    expected_sha256 = metadata.get("source_sha256")
    if not isinstance(expected_sha256, str) or not expected_sha256:
        raise ValueError("YOLO 后处理需要预处理元数据中的 source_sha256")
    image_path, image_bgr, source_sha256 = _read_image(image_path, expected_sha256)

    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            nms_started = time.perf_counter()
            boxes, scores, classes = _nms(prediction, conf, iou, max_det)
            nms_seconds = time.perf_counter() - nms_started
            boxes = _restore_boxes(boxes, metadata, image_bgr.shape)
    except FloatingPointError as error:
        raise ValueError("YOLO 预测或元数据导致坐标／IoU 计算浮点溢出") from error
    if not np.isfinite(boxes).all():
        raise ValueError("YOLO 原图坐标还原产生了非有限数")
    detections = [
        {"class_id": int(class_id), "class_name": _class_name(names, int(class_id)),
         "confidence": float(score), "box_xyxy": box.tolist()}
        for box, score, class_id in zip(boxes, scores, classes)
    ]

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    annotated_path = output_dir / "annotated.jpg"
    detections_path = output_dir / "detections.json"
    for path in (annotated_path, detections_path):
        if path.resolve() == image_path:
            raise ValueError(f"YOLO 结果路径不能覆盖输入图像: {path}")
        if path.exists() or path.is_symlink():
            raise FileExistsError(f"YOLO 结果文件已存在，拒绝覆盖: {path}")

    image = Image.fromarray(cv2.cvtColor(image_bgr, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(image)
    for detection in detections:
        box = detection["box_xyxy"]
        draw.rectangle(box, outline=(220, 40, 40), width=max(1, min(image.size) // 300))
        label = f"{detection['class_name']} {detection['confidence']:.3f}"
        draw.text((max(0, box[0]), max(0, box[1] - 13)), label, fill=(220, 40, 40))
    # 独占创建也阻止检查后才出现的同名链接覆盖原图。
    with annotated_path.open("xb") as output:
        image.save(output, format="JPEG", quality=90)
    result = {
        "image_path": str(image_path), "source_sha256": source_sha256,
        "original_shape": list(image_bgr.shape),
        "prediction_shape": list(prediction.shape), "conf": conf, "iou": iou, "max_det": max_det,
        "max_wh": _NMS_MAX_WH, "count": len(detections), "detections": detections,
        "classes": {str(item["class_id"]): item["class_name"] for item in detections},
        "nms_seconds": nms_seconds, "postprocess_seconds": time.perf_counter() - started,
        "artifacts": {"detections": detections_path.name, "annotated": annotated_path.name},
    }
    with detections_path.open("x", encoding="utf-8") as output:
        output.write(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    return result
