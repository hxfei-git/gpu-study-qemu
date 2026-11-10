# SPDX-License-Identifier: GPL-2.0-or-later
"""完整模型入口：准备独立运行包、运行设备请求、汇总实际测量。"""

import argparse
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import signal
import statistics
import subprocess
import sys
import time
import traceback


SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parents[2]
BUILD = ROOT / "build"
_ATOL = 2e-4
_RTOL = 2e-4

# -I 不把脚本目录加入搜索路径；这里只补入仓库自己的受控目录。
sys.dont_write_bytecode = True
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))
import gpgpu_model_eval as base


def _json(path, value):
    base.write_json(path, value)


def _new_directory(path):
    path = Path(path).resolve()
    if not path.is_relative_to(BUILD):
        raise ValueError("输出必须位于本仓库 build 目录")
    path.mkdir(parents=True, exist_ok=False)
    return path


def _model_module(model):
    return importlib.import_module("model_" + model)


def _torch_threads(threads):
    import torch

    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)


def _input_array(value, model):
    import numpy as np

    return np.ascontiguousarray(value, dtype="<f4" if model == "yolo" else "<i8")


def _input_digest(value, model):
    value = _input_array(value, model)
    return {"sha256": hashlib.sha256(memoryview(value).cast("B")).hexdigest(),
            "shape": list(value.shape), "dtype": "float32" if model == "yolo" else "int64",
            "bytes": value.nbytes}


def _input_loads(index):
    from model_graph import program_abi

    abi = program_abi()
    arena = abi["GPGPU_MODEL_ARENA_INPUT"] + 1
    shift = abi["GPGPU_MODEL_ADDRESS_SHIFT"]
    return [{key: load[key] for key in ("name", "sha256", "bytes", "address")}
            for load in index["loads"] if load["address"] >> shift == arena]


def _detect_round(index):
    from model_graph import program_abi

    kind = program_abi()["GPGPU_MODEL_ROUND_DETECT"]
    return next(i for i, item in enumerate(index["rounds"]) if item["kind"] == kind)


def prepare_command(args):
    from model_graph import GraphBuilder

    output = _new_directory(args.output)
    request = {"schema_version": 1, "status": "preparing", "model": args.model,
               "command": sys.orig_argv, "manifest": str(args.manifest.resolve()),
               "manifest_sha256": base.sha256(args.manifest), "threads": args.threads,
               "max_new_tokens": args.max_new_tokens, "capacity": args.capacity,
               "prefill_chunk": args.prefill_chunk, "prefix_length": args.prefix_length,
               "use_chat": not args.no_chat, "conf": args.conf, "iou": args.iou,
               "max_det": args.max_det, "no_reference": args.no_reference,
               "original_reference": args.original_reference, "checkpoints": args.checkpoints}
    _json(output / "request.json", request)
    try:
        _torch_threads(args.threads)
        adapter = _model_module(args.model)
        graph = GraphBuilder(args.manifest, args.model, checkpoints=args.checkpoints)
        if args.model == "yolo":
            image = (args.image or Path(graph.model_info["input"]["image_path"])).resolve()
            value, metadata = adapter.preprocess_image(image)
            request.update({"image": str(image), "image_sha256": base.sha256(image),
                            "input": _input_digest(value, "yolo"), "preprocess": metadata})
            graph.input_tensor = graph.const("yolo.image", value)
            graph.begin_round("image", graph.program["GPGPU_MODEL_ROUND_DETECT"])
            built = adapter.build_yolo(graph, graph.model_info)
            graph.end_round(built["output"],
                            checks=[(f"raw{i}", x) for i, x in enumerate(built["raw_heads"])])
            index = graph.write(output / "bundle", graph.program["GPGPU_MODEL_YOLO"])
        else:
            if args.text_file:
                prompt = args.text_file.read_text(encoding="utf-8")
                request.update({"text_file": str(args.text_file.resolve()),
                                "text_file_sha256": base.sha256(args.text_file)})
            elif args.prompt is not None:
                prompt = args.prompt
            elif args.prefix_length is not None:
                prompt = None
            else:
                raise ValueError("Qwen prepare 需要 --prompt、--text-file 或 --prefix-length")
            ids, metadata = adapter.tokenize(prompt, use_chat=not args.no_chat,
                                              prefix_length=args.prefix_length)
            request.update({"prompt": metadata["original_text"], "tokenizer": metadata,
                            "use_chat": metadata["use_chat"], "input": _input_digest(ids, "qwen")})
            built = adapter.build_qwen(graph, ids, max_new_tokens=args.max_new_tokens,
                                       capacity=args.capacity, prefill_chunk=args.prefill_chunk)
            index = graph.write(output / "bundle", graph.program["GPGPU_MODEL_QWEN"],
                                eos=built["eos"], vocabulary=built["vocabulary"],
                                prompt_tokens=built["prompt_tokens"])
        request.update({"bundle_program_sha256": index["program_sha256"],
                        "bundle_input_loads": _input_loads(index),
                        "bundle": "bundle", "reference": None})
        _json(output / "request.json", request)
        # CPU 参考单独保存，运行时只挂载 bundle 和结果目录。
        if not args.no_reference:
            if args.model == "yolo":
                adapter.cpu_reference(args.manifest, image, output / "reference",
                                      conf=args.conf, iou=args.iou, max_det=args.max_det)
            else:
                adapter.cpu_reference(args.manifest, ids, output / "reference",
                                      max_new_tokens=args.max_new_tokens,
                                      prefill_chunk=args.prefill_chunk,
                                      original_fp32=args.original_reference)
            request["reference"] = "reference/metadata.json"
        request["status"] = "complete"
        _json(output / "request.json", request)
        print(json.dumps({"package": str(output / "bundle"), "request": str(output / "request.json"),
                          "reference": str(output / request["reference"]) if request["reference"] else None,
                          "rounds": len(index["rounds"]),
                          "weights": len({name for name, _ in index["used_weights"]}),
                          "weight_arrays": len(index["used_weights"])},
                         ensure_ascii=False, indent=2))
    except BaseException as error:
        request.update({"status": "failed", "error": f"{type(error).__name__}: {error}"})
        _json(output / "request.json", request)
        raise


def _package(path):
    bundle = Path(path).resolve()
    if not (bundle / "index.json").is_file() and (bundle / "bundle/index.json").is_file():
        bundle /= "bundle"
    index = json.loads((bundle / "index.json").read_text())
    if index["model"] not in ("yolo", "qwen"):
        raise ValueError("运行包不是已支持的完整模型")
    started = time.monotonic_ns()
    if base.sha256(bundle / "program.bin") != index["program_sha256"]:
        raise ValueError("模型程序散列发生变化")
    allowed = {"index.json", "program.bin"}
    checked, byte_count = set(), 0
    for load in index["loads"]:
        path = (bundle / load["path"]).resolve()
        if not path.is_relative_to(bundle):
            raise ValueError("运行包文件越出 bundle")
        allowed.add(str(path.relative_to(bundle)))
        if path not in checked:
            if path.stat().st_size != load["bytes"] or base.sha256(path) != load["sha256"]:
                raise ValueError("运行包权重或实际输入发生变化: " + load["name"])
            checked.add(path)
            byte_count += path.stat().st_size
    # 防止把 reference 或其他 CPU 中间文件随包挂载给设备客体。
    extra = {str(p.relative_to(bundle)) for p in bundle.rglob("*") if p.is_file()} - allowed
    if extra:
        raise ValueError(f"bundle 包含未登记文件，不能挂载: {sorted(extra)}")
    return bundle, index, {"files": len(checked) + 1, "bytes": byte_count,
                           "elapsed_ns": time.monotonic_ns() - started}


def _reference(path):
    if path is None:
        return None
    path = Path(path).resolve()
    if path.is_dir():
        path /= "metadata.json"
    return {"path": path, "root": path.parent, "sha256": base.sha256(path),
            "metadata": json.loads(path.read_text())}


def _request(args, bundle, index):
    path = args.request.resolve() if args.request else bundle.parent / "request.json"
    if path.is_dir():
        path /= "request.json"
    if args.request and not path.is_file():
        raise ValueError("指定的 request.json 不存在")
    request = json.loads(path.read_text()) if path.is_file() else {"model": index["model"]}
    if request.get("status", "complete") != "complete":
        raise ValueError("prepare 尚未完成；需先完成 CPU 参考或重新以 --no-reference 准备")
    if request["model"] != index["model"]:
        raise ValueError("request 与 bundle 的模型不一致")
    if request.get("bundle_program_sha256", index["program_sha256"]) != index["program_sha256"]:
        raise ValueError("request 不属于这个模型程序")
    frozen_loads = _input_loads(index)
    if request.get("bundle_input_loads", frozen_loads) != frozen_loads:
        raise ValueError("request 与 bundle 的冻结输入不一致")
    if index["model"] == "yolo":
        image = args.image or request.get("image")
        if image is None:
            raise ValueError("旧 YOLO bundle 没有 request.json，运行时必须明确 --image")
        request["image"] = str(Path(image).resolve())
        request["actual_image_sha256"] = base.sha256(request["image"])
        image_load = next((x for x in index["loads"] if x["name"] == "yolo.image"), None)
        if image_load is None:
            raise ValueError("YOLO bundle 缺少 yolo.image 实际输入")
        expected = request.get("input", {}).get("sha256", image_load["sha256"])
        if expected != image_load["sha256"]:
            raise ValueError("request 的图像输入散列与 bundle 不符")
        request["input"] = {**request.get("input", {}), "sha256": expected}
    else:
        if request.get("prefix_length") is not None and (args.prompt is not None or args.text_file):
            raise ValueError("Alice 前缀包不能用 --prompt/--text-file 覆盖，需重新 prepare")
        if args.text_file:
            request["text_file"] = str(args.text_file.resolve())
            request["prompt"] = args.text_file.read_text(encoding="utf-8")
        elif args.prompt is not None:
            request["prompt"] = args.prompt
            request.pop("text_file", None)
        if "prompt" not in request or "input" not in request:
            raise ValueError("Qwen 运行需要 prepare 生成的 request.json 和实际文本")
    return request, path if path.is_file() else None


def _model_records(path):
    records = {name: [] for name in ("SETUP", "SAMPLE", "ROUND", "NODE", "DONE")}
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if not line.startswith("GPGPU_MODEL_"):
                continue
            prefix, _, payload = line.partition(" ")
            kind = prefix.removeprefix("GPGPU_MODEL_")
            if kind not in records:
                continue
            try:
                records[kind].append(json.loads(payload))
            except json.JSONDecodeError:
                continue
    return records


def _publish(path):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text("1\n")
    temporary.replace(path)


def _prepare_input(adapter, model, request):
    if model == "yolo":
        value, metadata = adapter.preprocess_image(request["image"])
    else:
        prompt = (Path(request["text_file"]).read_text(encoding="utf-8")
                  if request.get("text_file") else request["prompt"])
        value, metadata = adapter.tokenize(prompt, use_chat=request.get("use_chat", True),
                                           prefix_length=request.get("prefix_length"))
    digest = _input_digest(value, model)
    if digest["sha256"] != request["input"]["sha256"]:
        raise ValueError("本次真实输入与 bundle 冻结输入不同，必须重新 prepare")
    return metadata, digest


def _tensor_comparison(actual, desc, root):
    import numpy as np

    expected = (root / desc["path"]).resolve()
    if not expected.is_relative_to(root):
        raise ValueError("参考张量越出参考目录")
    result = {"actual": str(actual), "reference": str(expected), "shape": desc["shape"],
              "dtype": desc["dtype"], "atol": _ATOL, "rtol": _RTOL}
    if not actual.is_file():
        return {**result, "passed": False, "error": "missing_actual"}
    if actual.stat().st_size != expected.stat().st_size:
        return {**result, "passed": False, "error": "size_mismatch",
                "actual_bytes": actual.stat().st_size, "reference_bytes": expected.stat().st_size}
    dtype = {"float32": "<f4", "int64": "<i8", "int8": "i1"}[desc["dtype"]]
    a, b = np.fromfile(actual, dtype=dtype), np.fromfile(expected, dtype=dtype)
    if a.size != b.size or b.size != int(np.prod(desc["shape"])):
        return {**result, "passed": False, "error": "size_mismatch",
                "actual_elements": int(a.size), "reference_elements": int(b.size)}
    if desc["dtype"] != "float32":
        failed = int(np.count_nonzero(a != b))
        return {**result, "passed": not failed, "failed": failed, "nonfinite": 0}
    a, b = a.astype(np.float64), b.astype(np.float64)
    finite_a, finite_b = np.isfinite(a), np.isfinite(b)
    finite = finite_a & finite_b
    failed = int(np.count_nonzero(~(finite & np.isclose(a, b, atol=_ATOL, rtol=_RTOL,
                                                        equal_nan=False))))
    difference = a[finite] - b[finite]
    denominator = float(np.linalg.norm(b[finite]))
    numerator = float(np.linalg.norm(difference))
    return {**result, "passed": not failed and bool(finite.all()), "failed": failed,
            "nonfinite": int(np.count_nonzero(~finite_a)),
            "reference_nonfinite": int(np.count_nonzero(~finite_b)),
            "max_abs": float(np.max(np.abs(difference))) if difference.size else None,
            "rel_l2": numerator / denominator if denominator else (0.0 if not numerator else None)}


def _detection_comparison(actual, expected):
    import numpy as np

    unused = set(range(len(expected)))
    failed, matched, max_box, max_conf = 0, 0, 0.0, 0.0
    for detection in actual:
        candidates = [i for i in unused if expected[i]["class_id"] == detection["class_id"]]
        if not candidates:
            failed += 1
            continue
        value = np.asarray([*detection["box_xyxy"], detection["confidence"]])
        def distance(index):
            ref = np.asarray([*expected[index]["box_xyxy"], expected[index]["confidence"]])
            return float(np.max(np.abs(value - ref) / (_ATOL + _RTOL * np.abs(ref))))
        index = min(candidates, key=distance)
        ref = np.asarray([*expected[index]["box_xyxy"], expected[index]["confidence"]])
        unused.remove(index)
        matched += 1
        max_box = max(max_box, float(np.max(np.abs(value[:4] - ref[:4]))))
        max_conf = max(max_conf, float(abs(value[4] - ref[4])))
        failed += int(not np.allclose(value, ref, atol=_ATOL, rtol=_RTOL))
    failed += len(unused)
    return {"passed": not failed, "failed": failed, "matched": matched,
            "actual_count": len(actual), "reference_count": len(expected),
            "max_box_abs": max_box, "max_conf_abs": max_conf, "atol": _ATOL, "rtol": _RTOL}


def _validate_sample(model, sample, guest_sample, index, reference, sample_dir, post, checkpoints):
    if reference is None:
        return {"available": False, "passed": None, "reason": "未提供 CPU 参考"}
    started = time.monotonic_ns()
    meta, root = reference["metadata"], reference["root"]
    comparisons = []
    if model == "yolo":
        tensors = meta["tensors"]
        round_id = _detect_round(index)
        comparisons.append(_tensor_comparison(sample_dir / f"round-{round_id:04d}.output.bin",
                                               tensors["predictions"], root))
        if checkpoints:
            for i, desc in enumerate(tensors["raw_heads"]):
                comparisons.append(_tensor_comparison(sample_dir / f"round-{round_id:04d}.raw{i}.bin",
                                                       desc, root))
        detections = _detection_comparison(post["detections"], meta["postprocess"]["detections"])
        passed = all(x["passed"] for x in comparisons) and detections["passed"]
        extra = {"detections": detections}
    else:
        tokens = {"passed": guest_sample["tokens"] == meta["tokens"],
                  "actual": guest_sample["tokens"], "reference": meta["tokens"]}
        if checkpoints:
            reference_rounds = {item["round"]: item for item in meta["rounds"]}
            for round_id in range(guest_sample["rounds"]):
                expected = reference_rounds.get(round_id)
                if expected is None:
                    comparisons.append({"passed": False, "error": "missing_reference_round",
                                        "round": round_id})
                    continue
                if expected.get("output") is not None:
                    comparisons.append(_tensor_comparison(sample_dir / f"round-{round_id:04d}.output.bin",
                                                           expected["output"], root))
                for desc in expected["checks"]:
                    name = desc["name"]
                    comparisons.append(_tensor_comparison(sample_dir / f"round-{round_id:04d}.{name}.bin",
                                                           desc, root))
        rounds_match = guest_sample["rounds"] == len(meta["rounds"])
        passed = tokens["passed"] and rounds_match and all(x["passed"] for x in comparisons)
        extra = {"tokens": tokens, "rounds_match": rounds_match}
    return {"available": True, "passed": passed, "tensors": comparisons, **extra,
            "validation_ns": time.monotonic_ns() - started}


def _runner_command(args):
    command = ["/mnt/gpgpu-eval-results/stack/gpgpu-model-run", "--program",
               "/mnt/gpgpu-eval-data/program.bin", "--output",
               "/mnt/gpgpu-eval-results/inference", "--control-dir",
               "/mnt/gpgpu-eval-results/control", "--repeats", str(args.repeats),
               "--warmups", str(args.warmups), "--timeout-ms", str(args.timeout_ms),
               "--control-timeout-ms", str(math.ceil(args.case_timeout * 1000))]
    if args.tile is not None:
        command += ["--tile", str(args.tile)]
    if args.checkpoints:
        command.append("--checkpoints")
    return command


def _component_hashes(linux, args, output):
    paths = [linux.QEMU_BINARY, args.device_qemu.resolve(), args.environment / "vmlinuz-virt",
             args.environment / "initramfs-developer", args.environment / "gpgpu_pci.ko",
             args.environment / "gpgpu-cp.bin", output / "stack/gpgpu-model-run",
             output / "stack/model_eval.bin"]
    return {str(path): base.sha256(path) for path in paths}


def run_command(args):
    import fcntl
    import tempfile

    linux = base.linux_runtime()
    bundle, index, verification = _package(args.package)
    if args.checkpoints != bool(index.get("checkpoints", False)):
        raise ValueError("run --checkpoints 必须与 prepare 的检查点设置一致，正式包与诊断包不能混用")
    request, request_path = _request(args, bundle, index)
    reference_path = args.reference
    if reference_path is None and (bundle.parent / "reference/metadata.json").is_file():
        reference_path = bundle.parent / "reference/metadata.json"
    reference = _reference(reference_path)
    config = {key: getattr(args, key) if getattr(args, key) is not None else request.get(key, default)
              for key, default in (("conf", 0.25), ("iou", 0.7), ("max_det", 300))}
    if reference:
        expected = reference["metadata"]
        if expected.get("manifest_sha256") != index["manifest_sha256"]:
            raise ValueError("CPU 参考缺少权重清单指纹或与模型包来源不同，请重新生成对应参考")
        if index["model"] == "yolo":
            if any(expected["postprocess"][key] != config[key] for key in config):
                raise ValueError("检测参考与请求 NMS 参数不同，请使用相同参数重新准备参考")
            input_path = (reference["root"] / expected["tensors"]["input"]["path"]).resolve()
            if not input_path.is_relative_to(reference["root"]) or base.sha256(input_path) != request["input"]["sha256"]:
                raise ValueError("YOLO CPU 参考与设备包的实际输入不同")
        else:
            if expected.get("precision") != "full-w8-fp32-activation-kv":
                raise ValueError("Qwen 正确性参考必须是完整 W8，不能用原始 FP32 替代")
            if _input_digest(expected["prompt_ids"], "qwen")["sha256"] != request["input"]["sha256"]:
                raise ValueError("Qwen CPU 参考的实际 token 前缀与设备包不同")
            if any(expected[key] != request[key] for key in ("max_new_tokens", "prefill_chunk")):
                raise ValueError("Qwen 参考的生成预算或 prefill 分块与设备包不同")
    output = _new_directory(args.output)
    control = output / "control"
    control.mkdir()
    (output / "stack").mkdir()
    (output / "inference").mkdir()
    # 临时 socket 使用用户提供的 TMPDIR；路径过长时应由调用者调整，不越过沙箱。
    tempfile.tempdir = None
    metadata = {"schema_version": 1, "status": "running", "model": index["model"],
                "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "command": sys.orig_argv, "package": str(bundle),
                "index_sha256": base.sha256(bundle / "index.json"),
                "program_sha256": index["program_sha256"],
                "program_rounds": index["rounds"], "arena_sizes": index["arena_sizes"],
                "prompt_tokens": index["prompt_tokens"], "vocabulary": index["vocabulary"],
                "request": request, "request_path": str(request_path) if request_path else None,
                "request_sha256": base.sha256(request_path) if request_path else None,
                "reference": {"path": str(reference["path"]), "sha256": reference["sha256"]}
                if reference else None,
                "gpuvm": "on", "cache": "WB", "shared_fetch": True, "tensor_native": True,
                "vram_clear_dma": True, "checkpoints": args.checkpoints,
                "profile": args.profile, "trace": args.trace, "tile": args.tile,
                "repeats": args.repeats, "warmups": args.warmups,
                "case_timeout": args.case_timeout, "timeout_ms": args.timeout_ms,
                "threads": args.threads,
                "host": platform.platform(), "material_verification": verification,
                "memory_before": Path("/proc/meminfo").read_text(),
                "version": base.source_snapshot(output), "samples": []}
    _json(output / "result.json", metadata)
    guest, remote, error = None, None, None
    model_log, exit_path = output / "model.log", output / "guest-exit"
    args.environment = args.environment.resolve()
    with (args.environment / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            started = time.monotonic_ns()
            environment = linux.environment_metadata(args.environment)
            if not environment:
                raise RuntimeError("现有开发客体环境未通过核对；不会下载或重建开发盘")
            metadata["environment"] = environment
            if not linux.qemu_configured():
                raise RuntimeError("现有 QEMU 构建目录未配置；本入口不下载或初始化依赖")
            linux.ensure_qemu(args.environment)
            module = linux.prepared_metadata(args.environment)
            fingerprint = linux.source_hash()
            if not module or module.get("source_hash") != fingerprint:
                linux.compile_module(args.environment, fingerprint, environment)
            linux.prepare_firmware(args.environment)
            with (output / "kernel-build.log").open("w") as log:
                subprocess.run(["make", "-C", str(ROOT / "software/gpgpu"),
                                "BUILD_DIR=" + str(output / "stack"), "kernels"],
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            metadata["environment_prepare_ns"] = time.monotonic_ns() - started
            extra = ["-global", "gpgpu.shared_fetch=on", "-global", "gpgpu.tensor_native=on",
                     "-global", "gpgpu.profile_dispatch=" + ("on" if args.profile else "off"),
                     "-global", "gpgpu.trace_dispatch=" + ("on" if args.trace else "off")]
            remote = linux.RemoteDevice(args.device_qemu.resolve(), output, gpuvm=True, extra_args=extra)
            command = linux.developer_command(args.environment, "none", remote=True)
            command += linux.shared_directory(bundle, "eval_data", True)
            command += linux.shared_directory(bundle, "eval_jobs", True)
            command += linux.shared_directory(output, "eval_results", False)
            started = time.monotonic_ns()
            guest = linux.Guest(command, output / "console.log", remote)
            metadata["host_command"] = guest.process.args
            metadata["device_command"] = remote.process.args
            guest.wait(rb"^GPGPU_DEVELOPER_READY$", timeout=180)
            metadata["guest_boot_ns"] = time.monotonic_ns() - started
            started = time.monotonic_ns()
            base.benchmark_setup(guest, ["vram_clear_dma=1"])
            metadata["guest_driver_setup_ns"] = time.monotonic_ns() - started
            metadata["components"] = _component_hashes(linux, args, output)
            model_setup_started = time.monotonic_ns()
            _torch_threads(args.threads)
            adapter = _model_module(index["model"])
            runner = _runner_command(args)
            metadata["runner_command"] = runner
            guest.send("(" + shlex.join(runner) + " > /mnt/gpgpu-eval-results/model.log 2>&1; "
                       "rc=$?; printf '%s\\n' \"$rc\" > /mnt/gpgpu-eval-results/guest-exit.tmp; "
                       "mv /mnt/gpgpu-eval-results/guest-exit.tmp /mnt/gpgpu-eval-results/guest-exit) &\n")
            base.wait_files([control / "ready"], guest, remote,
                            time.monotonic() + args.case_timeout, [exit_path])
            metadata["host_model_setup_ns"] = time.monotonic_ns() - model_setup_started
            metadata["effective_tile"] = _model_records(model_log)["SETUP"][0]["tile"]
            metadata["memory_ready"] = {"host": base.process_memory(os.getpid()),
                                        "guest": base.process_memory(guest.process.pid),
                                        "device": base.process_memory(remote.process.pid)}
            _json(output / "result.json", metadata)
            for sample in range(args.warmups + args.repeats):
                begin = time.monotonic_ns()
                input_meta, digest = _prepare_input(adapter, index["model"], request)
                preprocess_ns = time.monotonic_ns() - begin
                _publish(control / f"start-{sample:04d}")
                start_published = time.monotonic_ns()
                base.wait_files([control / f"done-{sample:04d}"], guest, remote,
                                time.monotonic() + args.case_timeout, [exit_path])
                done_observed = time.monotonic_ns()
                sample_dir = output / "inference" / f"sample-{sample:04d}"
                post_started = time.monotonic_ns()
                guest_sample = json.loads((sample_dir / "sample.json").read_text())
                if guest_sample["sample"] != sample:
                    raise RuntimeError("客体发布的必要结果不属于当前请求")
                if index["model"] == "yolo":
                    round_id = _detect_round(index)
                    post = adapter.postprocess(sample_dir / f"round-{round_id:04d}.output.bin",
                                               request["image"], input_meta,
                                               sample_dir / "postprocess", **config)
                else:
                    text = adapter.decode_tokens(guest_sample["tokens"])
                    post = {"tokens": guest_sample["tokens"], "text": text}
                    _json(sample_dir / "generated.json", post)
                    (sample_dir / "generated.txt").write_text(text, encoding="utf-8")
                end = time.monotonic_ns()
                record = {"sample": sample, "warmup": sample < args.warmups,
                          "host_begin_ns": begin, "host_end_ns": end,
                          "host_request_ns": end - begin, "host_preprocess_ns": preprocess_ns,
                          "host_postprocess_ns": end - post_started,
                          "host_device_wait_ns": done_observed - start_published,
                          "input": digest, "preprocess": input_meta, "postprocess": post,
                          "guest": guest_sample}
                # 正确性比较在请求截止之后进行，不混入正式请求延迟。
                record["validation"] = _validate_sample(index["model"], sample, guest_sample, index,
                                                         reference, sample_dir, post, args.checkpoints)
                record["memory"] = {"host": base.process_memory(os.getpid()),
                                     "guest": base.process_memory(guest.process.pid),
                                     "device": base.process_memory(remote.process.pid)}
                metadata["samples"].append(record)
                _json(output / "result.json", metadata)
                _publish(control / f"release-{sample:04d}")
                # 上次遥测完全结束后再计下一请求，不能转嫁历史日志开销。
                base.wait_files([control / f"idle-{sample:04d}"], guest, remote,
                                time.monotonic() + args.case_timeout, [exit_path])
                print(json.dumps({"sample": sample, "warmup": record["warmup"],
                                  "host_request_ns": record["host_request_ns"],
                                  "validation_passed": record["validation"]["passed"]},
                                 ensure_ascii=False), flush=True)
            base.wait_files([exit_path], guest, remote, time.monotonic() + args.case_timeout)
            metadata["guest_exit_code"] = int(exit_path.read_text().strip())
            records = _model_records(model_log)
            metadata["runner"] = records
            done = records["DONE"]
            if metadata["guest_exit_code"] or len(done) != 1 or not done[0].get("success") or not done[0].get("cleanup_ok") or done[0].get("clock_error"):
                raise RuntimeError("设备模型运行或资源回收失败，查看 model.log")
            guest.buffer = b""
            linux.check_kernel_log(guest)
            (output / "kernel.log").write_bytes(guest.buffer)
            checks = [sample["validation"] for sample in metadata["samples"]]
            metadata["status"] = ("pass" if reference else "completed-unverified") if all(x["passed"] is not False for x in checks) else "failed"
            if metadata["samples"]:
                metadata["cold_application_ns"] = metadata["host_model_setup_ns"] + metadata["samples"][0]["host_request_ns"]
            if metadata["status"] == "failed":
                raise RuntimeError("完整模型正确性比较失败；保留所有已完成样本，不放宽容差")
        except BaseException as failure:
            error = failure
            metadata.update({"status": "failed", "error": f"{type(failure).__name__}: {failure}",
                             "traceback": traceback.format_exc()})
            _publish(control / "abort")
        finally:
            if exit_path.exists():
                metadata["guest_exit_code"] = int(exit_path.read_text().strip())
            metadata["runner"] = _model_records(model_log)
            metadata["memory_after"] = Path("/proc/meminfo").read_text()
            if error is not None and guest and not exit_path.exists():
                try:
                    base.wait_files([exit_path], guest, remote, time.monotonic() + 5)
                except (RuntimeError, TimeoutError):
                    pass
            try:
                try:
                    if guest:
                        guest.close()
                finally:
                    if remote:
                        remote.close()
            except BaseException as close_error:
                metadata["close_error"] = f"{type(close_error).__name__}: {close_error}"
                if error is None:
                    error = close_error
                    metadata["status"] = "failed"
            metadata["device_events"] = base.device_events(output)
            metadata["doorbells"] = base.device_events(output, "GPGPU_DOORBELL")
            metadata["queue_progress"] = base.device_events(output, "GPGPU_QUEUE_PROGRESS")
            if guest:
                metadata["host_qemu_exit_code"] = guest.process.poll()
            if remote and remote.process:
                metadata["device_qemu_exit_code"] = remote.process.poll()
            _json(output / "result.json", metadata)
    if error is not None:
        raise error
    print(json.dumps({"status": metadata["status"], "result": str(output / "result.json")},
                     ensure_ascii=False, indent=2))


def _timing_summary(values):
    if not values:
        return None
    return {"samples": len(values), "min_ns": min(values), "median_ns": statistics.median(values),
            "mean_ns": statistics.mean(values), "max_ns": max(values)}


def _model_performance(result, samples):
    """只从实际完成的请求和同一客体时钟的逐轮记录推导速率。"""
    if result["model"] == "yolo":
        return {"images_per_second": [1e9 / item["host_request_ns"] for item in samples]}
    from model_graph import program_abi
    token_kind = program_abi()["GPGPU_MODEL_ROUND_TOKEN"]
    prompt = result.get("prompt_tokens")
    prefill, first_ids, decode_rates, intervals = [], [], [], []
    for sample in samples:
        rounds = [item for item in result.get("runner", {}).get("ROUND", [])
                  if item["sample"] == sample["sample"]]
        token_rounds = [item for item in rounds if item["kind"] == token_kind]
        if not token_rounds:
            continue
        first = token_rounds[0]
        prefill.append(sum(item["forward_ns"] for item in rounds if item["round"] <= first["round"]))
        first_ids.append(first["ready_ns"])
        for index, (previous, current) in enumerate(zip(token_rounds, token_rounds[1:]), 1):
            intervals.append({"sample": sample["sample"], "token_index": index,
                              "context_before": prompt + index - 1 if prompt is not None else None,
                              "elapsed_ns": current["ready_ns"] - previous["ready_ns"]})
        if len(token_rounds) > 1:
            decode_rates.append((len(token_rounds) - 1) * 1e9 /
                                (token_rounds[-1]["ready_ns"] - first["ready_ns"]))
    return {"prompt_tokens": prompt, "prefill_forward_ns": _timing_summary(prefill),
            "prefill_tokens_per_second": [prompt * 1e9 / value for value in prefill]
            if prompt is not None else None,
            "first_token_id_ready_guest_ns": _timing_summary(first_ids),
            "decode_tokens_per_second": decode_rates, "decode_intervals": intervals,
            "text_published_host_ns": _timing_summary([item["host_request_ns"] for item in samples]),
            "boundary": "首 token 指客体内 ID 可用，文本在完整请求后发布；decode 不重复计首 token；诊断回读不作为正式性能"}


def report_command(args):
    paths = [*args.inputs, *args.result]
    if not paths:
        raise ValueError("report 需要至少一个 result.json 或结果目录")
    reports = []
    for path in paths:
        path = path.resolve()
        if path.is_dir():
            path /= "result.json"
        result = json.loads(path.read_text())
        formal = [x for x in result.get("samples", []) if not x["warmup"]]
        sample_ids = {x["sample"] for x in formal}
        stages, operators, network_stages, categories = {}, {}, {}, {}
        rounds = result.get("runner", {}).get("ROUND", [])
        for item in rounds:
            if item["sample"] not in sample_ids:
                continue
            stage = stages.setdefault(item["name"], {"forward_ns": [], "output_ns": [],
                                                      "token_upload_ns": [], "checks_ns": [],
                                                      "ready_ns": []})
            for key in stage:
                stage[key].append(item[key])
        for item in result.get("runner", {}).get("NODE", []):
            if item["sample"] not in sample_ids:
                continue
            node = result["program_rounds"][item["round"]]["nodes"][item["node"]]
            stage = node.get("stage", "unclassified")
            category = stage.split(".", 1)[-1] if stage.startswith("layer-") else stage
            for bucket, group in ((operators, node["op"]), (network_stages, stage),
                                  (categories, category)):
                value = bucket.setdefault(group, {"operator_ns": 0, "params_ns": 0,
                                                   "dispatch_ns": 0, "dispatches": 0,
                                                   "work_items": 0, "calls": 0})
                for key in value:
                    value[key] += 1 if key == "calls" else item[key]
        events = [event for event in result.get("device_events", [])
                  if any(sample["host_begin_ns"] <= event["start_ns"] <= event["end_ns"] <= sample["host_end_ns"]
                         for sample in formal)]
        profile = {}
        for event in events:
            for name, value in event.get("profile", {}).items():
                profile[name] = profile.get(name, 0) + value
        reports.append({"result": str(path), "model": result["model"], "status": result["status"],
                        "diagnostic": bool(result.get("profile") or result.get("trace") or result.get("checkpoints")),
                        "host_model_setup_ns": result.get("host_model_setup_ns"),
                        "guest_model_setup": result.get("runner", {}).get("SETUP", []),
                        "cold_application_ns": result.get("cold_application_ns"),
                        "environment_prepare_ns": result.get("environment_prepare_ns"),
                        "guest_boot_ns": result.get("guest_boot_ns"),
                        "guest_driver_setup_ns": result.get("guest_driver_setup_ns"),
                        "profile_totals": profile or None,
                        "device_service_ns": sum(event["service_ns"] for event in events) if events else None,
                        "arena_sizes": result.get("arena_sizes"),
                        "material_verification_ns": result["material_verification"]["elapsed_ns"],
                        "host_request": _timing_summary([x["host_request_ns"] for x in formal]),
                        "host_preprocess": _timing_summary([x["host_preprocess_ns"] for x in formal]),
                        "host_postprocess": _timing_summary([x["host_postprocess_ns"] for x in formal]),
                        "guest_request": _timing_summary([x["guest"]["request_ns"] for x in formal]),
                        "guest_input": _timing_summary([x["guest"]["input_ns"] for x in formal]),
                        "guest_input_h2d": _timing_summary([x["guest"]["input_h2d_ns"] for x in formal]),
                        "stages": {name: {key: _timing_summary(values) for key, values in stage.items()}
                                   for name, stage in stages.items()},
                        "operators": sorted([{"op": name, **value} for name, value in operators.items()],
                                            key=lambda value: value["operator_ns"], reverse=True),
                        "network_stages": network_stages, "stage_categories": categories,
                        "model_performance": _model_performance(result, formal),
                        "performance_eligible": result["status"] == "pass" and not (
                            result.get("profile") or result.get("trace") or result.get("checkpoints")),
                        "correctness": [x["validation"] for x in formal],
                        "boundary": "只汇总本次完整设备执行；宿主与客体时钟不混减，不使用独立算子外推"})
    summary = {"models": reports}
    if args.output:
        path = args.output.resolve()
        if not path.is_relative_to(BUILD) or path.exists():
            raise ValueError("report 输出必须是 build 下的新文件")
        path.parent.mkdir(parents=True, exist_ok=True)
        _json(path, summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare", help="准备完整设备图和独立 CPU 参考")
    prepare.add_argument("--model", choices=("yolo", "qwen"), required=True)
    prepare.add_argument("--manifest", type=Path, default=BUILD / "gpgpu-model-eval/reference-v2/manifest.json")
    prepare.add_argument("--output", type=Path, required=True)
    prepare.add_argument("--image", type=Path)
    text = prepare.add_mutually_exclusive_group()
    text.add_argument("--prompt")
    text.add_argument("--text-file", type=Path)
    prepare.add_argument("--prefix-length", type=int, help="使用本地 Alice 正文的真实 token 前缀，不加聊天模板")
    prepare.add_argument("--max-new-tokens", type=int, default=16)
    prepare.add_argument("--capacity", type=int, default=2048)
    prepare.add_argument("--prefill-chunk", type=int, default=128)
    prepare.add_argument("--checkpoints", action="store_true", help="构建含中间检查输出的诊断包")
    prepare.add_argument("--no-chat", action="store_true")
    prepare.add_argument("--no-reference", action="store_true")
    prepare.add_argument("--original-reference", action="store_true")
    prepare.add_argument("--threads", type=int, default=2)
    prepare.add_argument("--conf", type=float, default=0.25)
    prepare.add_argument("--iou", type=float, default=0.7)
    prepare.add_argument("--max-det", type=int, default=300)
    prepare.set_defaults(function=prepare_command)
    run = sub.add_parser("run", help="在现有 Linux 客体执行完整设备模型")
    run.add_argument("--package", "--bundle", dest="package", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--reference", type=Path)
    run.add_argument("--request", type=Path)
    run.add_argument("--image", type=Path)
    text = run.add_mutually_exclusive_group()
    text.add_argument("--prompt")
    text.add_argument("--text-file", type=Path)
    run.add_argument("--checkpoints", action="store_true")
    run.add_argument("--repeats", type=int, default=1)
    run.add_argument("--warmups", type=int, default=0)
    run.add_argument("--profile", action="store_true")
    run.add_argument("--trace", action="store_true")
    run.add_argument("--case-timeout", type=float, default=7200)
    run.add_argument("--timeout-ms", type=int, default=300000)
    run.add_argument("--tile", type=int, help="默认由客体按共享执行能力选择上限")
    run.add_argument("--threads", type=int, default=2)
    run.add_argument("--environment", type=Path, default=BUILD / "gpgpu-kernel-gpuvm")
    run.add_argument("--device-qemu", type=Path, default=BUILD / "arm64-qemu-o2/qemu-system-riscv64")
    run.add_argument("--conf", type=float)
    run.add_argument("--iou", type=float)
    run.add_argument("--max-det", type=int)
    run.set_defaults(function=run_command)
    report = sub.add_parser("report", help="汇总实际完整请求、分阶段和算子瓶颈")
    report.add_argument("inputs", type=Path, nargs="*")
    report.add_argument("--result", type=Path, action="append", default=[])
    report.add_argument("--output", type=Path)
    report.set_defaults(function=report_command)
    args = parser.parse_args()
    if not sys.flags.isolated:
        parser.error("必须使用当前独立环境 python -I 运行")
    if args.command in ("prepare", "run"):
        if args.threads <= 0:
            parser.error("--threads 必须为正数")
        if args.command == "prepare":
            if args.no_reference and args.original_reference:
                parser.error("--no-reference 不能与 --original-reference 同用")
            if args.prefix_length is not None and (args.prompt is not None or args.text_file):
                parser.error("--prefix-length 选择 Alice 正文，不能同时指定 --prompt/--text-file")
            if min(args.max_new_tokens, args.capacity, args.prefill_chunk) <= 0:
                parser.error("token 预算、容量和 prefill 分块必须为正数")
        elif (args.repeats <= 0 or args.warmups < 0 or not math.isfinite(args.case_timeout) or
              min(args.timeout_ms, args.case_timeout) <= 0 or
              (args.tile is not None and args.tile <= 0)):
            parser.error("重复次数、tile 和超时必须为正数，预热次数不能为负数")
        for name in ("conf", "iou"):
            value = getattr(args, name)
            if value is not None and not 0 <= value <= 1:
                parser.error(f"--{name} 必须在 [0,1] 内")
        if args.max_det is not None and args.max_det <= 0:
            parser.error("--max-det 必须为正数")
        base.prepare_environment()
        # 补齐旧 helper 没有覆盖的缓存变量，避免继承外部路径。
        cache = base.EVAL_ROOT / "cache"
        os.environ.update({"HF_HUB_CACHE": str(cache / "huggingface/hub"),
                           "HF_ASSETS_CACHE": str(cache / "huggingface/assets"),
                           "HF_DATASETS_CACHE": str(cache / "huggingface/datasets"),
                           "XDG_CONFIG_HOME": str(cache / "xdg/config"),
                           "XDG_DATA_HOME": str(cache / "xdg/data"),
                           "XDG_STATE_HOME": str(cache / "xdg/state")})
        os.environ.pop("TRANSFORMERS_CACHE", None)
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    args.function(args)


if __name__ == "__main__":
    main()
