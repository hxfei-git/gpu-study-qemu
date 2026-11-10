# SPDX-License-Identifier: GPL-2.0-or-later
"""模型应用：离线导出固定材料，或在客体 CPU 上聊天、处理图片并调用设备。"""

import argparse
from contextlib import contextmanager
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import time


SCRIPTS = Path(__file__).resolve().parent
STACK = SCRIPTS.parent
ROOT = STACK.parents[1]
# 每个临时程序最多包含的完整前向轮数，限制客体构图与解析的常驻元数据。
QWEN_WINDOW_ROUNDS = 8
sys.dont_write_bytecode = True
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def new_directory(path):
    path = Path(path).resolve()
    path.mkdir(parents=True, exist_ok=False)
    return path


def export_models(args):
    """只导出配置、图和权重；不运行参考模型，不绑定任何用户文本或图片。"""
    import gpgpu_model_eval as base
    from model_graph import GraphBuilder

    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        raise ValueError("离线应用材料必须放在本仓库 build 下的新目录")
    new_directory(output)
    base.prepare_environment()
    names = ("qwen", "yolo") if args.model == "all" else (args.model,)
    exported = {}
    for name in names:
        graph = GraphBuilder(args.manifest, name)
        info = graph.model_info
        app = {"schema_version": 1, "model": name, "source_manifest_sha256": graph.manifest_sha256,
               "program": "program.bin", "manifest": "manifest.json"}
        if name == "qwen":
            import model_qwen as adapter

            config = adapter._configuration(info)
            generation = adapter._generation()[0]
            assets = {"config": config, "generation_config": generation,
                      "inv_freq": adapter._inverse_frequencies(config).tolist()}
            # 初始程序只用于建立同一份权重合同；真实请求由客体按历史构图。
            built = adapter.build_qwen(graph, [0], max_new_tokens=1, capacity=args.capacity,
                                       prefill_chunk=args.prefill_chunk, assets=assets)
            index = graph.write(output / name, graph.program["GPGPU_MODEL_QWEN"],
                                eos=built["eos"], vocabulary=built["vocabulary"], prompt_tokens=1)
            app.update({"assets": assets, "capacity": args.capacity,
                        "prefill_chunk": args.prefill_chunk, "default_max_new_tokens": 16,
                        "tokenizer": "tokenizer"})
            target = output / name / "tokenizer"
            target.mkdir()
            source, _ = adapter._material()
            files = ("config.json", "generation_config.json", "tokenizer.json",
                     "tokenizer_config.json", "vocab.json", "merges.txt", "LICENSE")
            app["tokenizer_files"] = {}
            for filename in files:
                shutil.copy2(source / filename, target / filename)
                app["tokenizer_files"][filename] = digest(target / filename)
        else:
            import model_yolo as adapter
            from ultralytics.utils import yaml_load

            graph.input_tensor = graph.reserve_input("yolo.image", adapter._INPUT_SHAPE,
                                                      dtype="float32")
            graph.begin_round("image", graph.program["GPGPU_MODEL_ROUND_DETECT"])
            built = adapter.build_yolo(graph, info)
            graph.end_round(built["output"])
            index = graph.write(output / name, graph.program["GPGPU_MODEL_YOLO"])
            app["input_address"] = next(v["address"] for v in index["loads"]
                                        if v["name"] == "yolo.image")
            app["input_shape"] = list(adapter._INPUT_SHAPE)
            app["output_shape"] = list(adapter._PREDICTION_SHAPE)
            _, library = adapter._configuration()
            app["names"] = yaml_load(library / "cfg/datasets/coco.yaml")["names"]
        # 只携带设备实际使用的角色，不携带原 FP32 对照和任何参考激活。
        weights = {}
        for weight, role in graph.used_weights:
            desc = copy.deepcopy(info["weights"][weight][role])
            desc["path"] = "data/" + desc["sha256"] + ".bin"
            weights.setdefault(weight, {})[role] = desc
        model_info = {key: copy.deepcopy(info[key]) for key in
                      ("repository", "revision", "config", "precision", "checkpoint_sha256")
                      if key in info}
        model_info["weights"] = weights
        manifest = {"schema_version": 1, "models": {name: model_info},
                    "source_manifest_sha256": graph.manifest_sha256}
        directory = output / name
        write_json(directory / "manifest.json", manifest)
        app["manifest_sha256"] = digest(directory / "manifest.json")
        app["program_sha256"] = index["program_sha256"]
        write_json(directory / "app.json", app)
        exported[name] = str(directory)
    write_json(output / "models.json", exported)
    print(json.dumps(exported, ensure_ascii=False, indent=2))


class ModelPackage:
    """固定只读模型材料；权重校验每个应用会话一次，不在每次提问时重复。"""

    def __init__(self, directory, kind):
        self.root = Path(directory).resolve()
        self.app = json.loads((self.root / "app.json").read_text())
        if self.app.get("schema_version") != 1 or self.app.get("model") != kind:
            raise ValueError("应用包类型或版本不符")
        self.manifest = self.root / self.app["manifest"]
        self.program = self.root / self.app["program"]
        if (not self.manifest.resolve().is_relative_to(self.root) or
                not self.program.resolve().is_relative_to(self.root)):
            raise ValueError("应用包路径越界")
        if (digest(self.manifest) != self.app["manifest_sha256"] or
                digest(self.program) != self.app["program_sha256"]):
            raise ValueError("模型配置或初始程序发生变化")
        index = json.loads((self.root / "index.json").read_text())
        checked = set()
        for load in index["loads"]:
            path = (self.root / load["path"]).resolve()
            if not path.is_relative_to(self.root) or path.stat().st_size != load["bytes"]:
                raise ValueError("模型数据路径或长度非法：" + load["name"])
            if path not in checked and digest(path) != load["sha256"]:
                raise ValueError("模型数据散列变化：" + load["name"])
            checked.add(path)
        for name, expected in self.app.get("tokenizer_files", {}).items():
            path = (self.root / self.app["tokenizer"] / name).resolve()
            if not path.is_relative_to(self.root) or digest(path) != expected:
                raise ValueError("官方 tokenizer 文件发生变化：" + name)


@contextmanager
def session_transition():
    """先保存 C 句柄，再交付中断；也覆盖信号由 OpenCV 等工作线程接收的情况。"""
    previous = {value: signal.getsignal(value) for value in (signal.SIGINT, signal.SIGTERM)}
    pending = [None, None]

    def defer(signum, frame):
        if pending[0] is None:
            pending[0], pending[1] = signum, frame

    try:
        for signum, handler in previous.items():
            if handler != signal.SIG_IGN:
                signal.signal(signum, defer)
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
        if pending[0] is not None:
            handler = previous[pending[0]]
            if callable(handler):
                handler(*pending)
            elif handler == signal.SIG_DFL:
                os.kill(os.getpid(), pending[0])


class ModelSession:
    """客体进程直接调用既有 RT/KMT 后端；无 CPU 网络执行或 WSL 请求服务。"""

    def __init__(self, runtime):
        self.handle = None
        self.resource_state = "not_opened"
        self.library = ctypes.CDLL(str(Path(runtime).resolve()))
        self.library.gpgpu_model_session_open.argtypes = [ctypes.c_char_p, ctypes.c_uint64,
                                                         ctypes.c_uint32]
        self.library.gpgpu_model_session_open.restype = ctypes.c_void_p
        self.library.gpgpu_model_session_run.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                                        ctypes.c_char_p, ctypes.c_uint64,
                                                        ctypes.c_void_p, ctypes.c_size_t,
                                                        ctypes.c_bool]
        self.library.gpgpu_model_session_run.restype = ctypes.c_bool
        self.library.gpgpu_model_session_close.argtypes = [ctypes.c_void_p]
        self.library.gpgpu_model_session_close.restype = ctypes.c_bool

    def open(self, program, timeout_ms):
        if self.resource_state != "not_opened":
            raise RuntimeError("模型会话不能重复打开")
        with session_transition():
            self.resource_state = "opening"
            self.handle = self.library.gpgpu_model_session_open(os.fsencode(program), 0, timeout_ms)
            self.resource_state = "open" if self.handle else "open_failed_unconfirmed"
        if not self.handle:
            raise RuntimeError("设备模型装载失败；API 未返回句柄，收尾状态须检查后端日志")

    def run(self, program, output, input_address=0, value=None, checkpoints=False):
        output = new_directory(output)
        if value is None:
            pointer, size = None, 0
        else:
            if not value.flags.c_contiguous:
                raise ValueError("上传输入必须为连续数组")
            pointer, size = value.ctypes.data_as(ctypes.c_void_p), value.nbytes
        okay = self.library.gpgpu_model_session_run(
            self.handle, os.fsencode(program) if program is not None else None,
            os.fsencode(output), input_address, pointer, size, checkpoints)
        if not okay:
            raise RuntimeError("设备请求失败，停止使用本会话：" + str(output / "model.log"))
        return json.loads((output / "sample-0000/sample.json").read_text())

    def close(self):
        if not self.handle:
            return self.resource_state in ("not_opened", "closed")
        with session_transition():
            okay = self.library.gpgpu_model_session_close(self.handle)
            self.resource_state = "closed" if okay else "close_failed_retained"
            if okay:
                self.handle = None
        if not okay:
            raise RuntimeError("设备访问尚未确认结束，模型会话资源保留")
        return True


def finish_session(session, record):
    """保存实际资源状态，不能把成功 close 后交付的中断记成资源保留。"""
    try:
        if session:
            session.close()
    except KeyboardInterrupt:
        if session and session.resource_state == "close_failed_retained":
            record.update({"status": "failed", "cleanup_error": "关闭未成功，资源保留"})
        elif record.get("status") != "failed":
            record["status"] = "interrupted"
        record["cleanup_interrupted"] = True
        raise
    except BaseException as error:
        record.update({"status": "failed", "cleanup_error": f"{type(error).__name__}: {error}"})
        raise
    finally:
        state = session.resource_state if session else "not_opened"
        record.update({"resource_state": state, "cleanup_ok": state in ("not_opened", "closed")})


def qwen_generate(session, package, ids, turn, args, capacity, chunk):
    """一轮回答按有界前向窗口推进；窗口间延续 KV，不跨对话轮复用。"""
    from model_graph import GraphBuilder
    from model_qwen import _round_plan, build_qwen

    plan = _round_plan(len(ids), args.max_new_tokens, chunk)
    tokens, windows = [], []
    cursor, live = 0, 0
    prepare_ns, backend_ns = 0, 0
    while cursor < len(plan):
        end = min(cursor + QWEN_WINDOW_ROUNDS, len(plan))
        window = new_directory(turn / f"window-{len(windows):04d}")
        begin = time.monotonic_ns()
        graph = GraphBuilder(package.manifest, "qwen", checkpoints=args.checkpoints)
        built = build_qwen(graph, ids, max_new_tokens=args.max_new_tokens, capacity=capacity,
                           prefill_chunk=chunk, assets=package.app["assets"],
                           round_range=(cursor, end), generated_tokens=tokens)
        index = graph.write(window / "program", graph.program["GPGPU_MODEL_QWEN"],
                            eos=built["eos"], vocabulary=built["vocabulary"],
                            prompt_tokens=len(ids), weights_directory=package.root)
        program_sha256 = index["program_sha256"]
        eos_ids = built["eos"]
        # 序列化后即释放图与节点索引，不能与 C 解析出的下一份图同时长期持有。
        del index, graph, built
        prepared = time.monotonic_ns()
        sample = session.run(window / "program/program.bin", window / "inference",
                             checkpoints=args.checkpoints)
        computed = time.monotonic_ns()
        completed = sample["rounds"]
        if not isinstance(completed, int) or not 0 < completed <= end - cursor:
            raise RuntimeError("设备窗口返回的完成轮次不合法")
        executed = plan[cursor:cursor + completed]
        expected_tokens = sum(item["kind"] == "token" for item in executed)
        if (len(sample["tokens"]) != expected_tokens or
                any(not isinstance(value, int) or not 0 <= value < package.app["assets"]["config"]["vocab_size"]
                    for value in sample["tokens"])):
            raise RuntimeError("设备窗口的 token 数量或值域不合法")
        eos = sample["stop_reason"] == "eos"
        if ((not eos and (sample["stop_reason"] != "budget" or completed != end - cursor)) or
                (eos and (not sample["tokens"] or
                          sample["tokens"][-1] not in eos_ids))):
            raise RuntimeError("设备窗口的停止原因与实际输出不符")
        tokens.extend(sample["tokens"])
        live = executed[-1]["past"] + executed[-1]["length"]
        record = {"window": len(windows), "path": window.name,
                  "global_round_start": cursor, "global_round_end": cursor + completed,
                  "kv_tokens": live, "program_sha256": program_sha256,
                  "program_prepare_ns": prepared - begin, "backend_call_ns": computed - prepared,
                  "sample": sample}
        write_json(window / "window.json", record)
        windows.append(record)
        prepare_ns += prepared - begin
        backend_ns += computed - prepared
        cursor += completed
        if eos:
            break
    if not tokens or (not eos and len(tokens) != args.max_new_tokens):
        raise RuntimeError("设备没有按预算或 EOS 完成本轮生成")
    summary = {"tokens": tokens, "rounds": cursor, "kv_tokens": live,
               "stop_reason": "eos" if eos else "budget",
               **{key: sum(item["sample"][key] for item in windows)
                  for key in ("request_ns", "input_ns", "input_h2d_ns", "input_h2d_bytes")}}
    return summary, windows, prepare_ns, backend_ns


def qwen_chat(args):
    from guest_model_tokenizer import QwenTokenizer

    output = new_directory(args.output)
    status = {"model": "qwen", "status": "loading", "pid": os.getpid(),
              "command": sys.orig_argv, "turns": [], "events": [], "cleanup_ok": False}
    session = None
    history = []
    try:
        print("校验并装载 Qwen，权重仅在本会话开始时上传一次……", flush=True)
        started = time.monotonic_ns()
        package = ModelPackage(args.model, "qwen")
        tokenizer = QwenTokenizer(package.root / package.app["tokenizer"])
        capacity = args.capacity or package.app["capacity"]
        chunk = args.prefill_chunk or package.app["prefill_chunk"]
        if capacity > package.app["capacity"]:
            raise ValueError("会话容量超过离线模型包的约定")
        session = ModelSession(args.runtime)
        session.open(package.program, args.timeout_ms)
        status["model_setup_ns"] = time.monotonic_ns() - started
        status["status"] = "ready"
        status["source_manifest_sha256"] = package.app["source_manifest_sha256"]
        write_json(output / "session.json", status)
        print("Qwen 已就绪。/clear 清空历史，/exit 退出。", flush=True)
        while True:
            try:
                text = input("你：")
            except EOFError:
                break
            if text.strip() == "/exit":
                break
            if text.strip() == "/clear":
                history.clear()
                status["events"].append({"type": "clear", "after_turn": len(status["turns"])})
                write_json(output / "session.json", status)
                print("对话历史已清空，模型权重继续常驻。", flush=True)
                continue
            if not text.strip():
                continue
            begin = time.monotonic_ns()
            messages = [*history, {"role": "user", "content": text}]
            encoded = tokenizer.apply_chat_template(messages, add_generation_prompt=True)
            ids = tokenizer.encode(encoded)
            if len(ids) + args.max_new_tokens - 1 > capacity:
                print(f"上下文加生成预算超过 {capacity} token；请 /clear 后重试，不自动截断。",
                      flush=True)
                continue
            tokenized = time.monotonic_ns()
            turn = new_directory(output / f"turn-{len(status['turns']):04d}")
            sample, windows, prepare_ns, backend_ns = qwen_generate(
                session, package, ids, turn, args, capacity, chunk)
            computed = time.monotonic_ns()
            answer = tokenizer.decode(sample["tokens"], skip_special_tokens=True)
            history = [*messages, {"role": "assistant", "content": answer}]
            record = {"turn": len(status["turns"]), "messages": messages, "prompt_ids": ids,
                      "prompt_tokens": len(ids), "tokens": sample["tokens"], "text": answer,
                      "stop_reason": sample["stop_reason"], "sample": sample,
                      "windows": windows, "window_round_limit": QWEN_WINDOW_ROUNDS,
                      "tokenize_ns": tokenized - begin, "program_prepare_ns": prepare_ns,
                      "device_request_ns": backend_ns, "generation_ns": computed - tokenized,
                      "request_ns": time.monotonic_ns() - begin}
            write_json(turn / "generated.json", record)
            (turn / "generated.txt").write_text(answer, encoding="utf-8")
            status["turns"].append(record)
            write_json(output / "session.json", status)
            print("Qwen：" + answer, flush=True)
        status["status"] = "completed"
    except KeyboardInterrupt:
        status["status"] = "interrupted"
        print("\n会话中断，正在按设备完成状态回收资源……", flush=True)
    except BaseException as error:
        status.update({"status": "failed", "error": f"{type(error).__name__}: {error}"})
        raise
    finally:
        try:
            finish_session(session, status)
        finally:
            write_json(output / "session.json", status)


def yolo_detect(args):
    from guest_model_yolo import preprocess_image, postprocess

    output = new_directory(args.output)
    record = {"model": "yolo", "status": "running", "pid": os.getpid(),
              "command": sys.orig_argv, "image": str(args.image.resolve()), "cleanup_ok": False}
    session = None
    begin = time.monotonic_ns()
    try:
        package = ModelPackage(args.model, "yolo")
        value, metadata = preprocess_image(args.image, size=package.app["input_shape"][-2:])
        preprocessed = time.monotonic_ns()
        print("图像已在客体预处理，正在装载并执行 YOLO……", flush=True)
        session = ModelSession(args.runtime)
        session.open(package.program, args.timeout_ms)
        loaded = time.monotonic_ns()
        sample = session.run(None, output / "inference", package.app["input_address"], value)
        computed = time.monotonic_ns()
        prediction = output / "inference/sample-0000/round-0000.output.bin"
        post = postprocess(prediction, args.image, metadata, output / "postprocess",
                           conf=args.conf, iou=args.iou, max_det=args.max_det,
                           names=package.app["names"], output_shape=package.app["output_shape"])
        record.update({"status": "pass", "sample": sample, "preprocess": metadata,
                       "postprocess": post, "input_sha256": hashlib.sha256(memoryview(value).cast("B")).hexdigest(),
                       "source_manifest_sha256": package.app["source_manifest_sha256"],
                       "preprocess_and_verification_ns": preprocessed - begin,
                       "model_setup_ns": loaded - preprocessed,
                       "device_request_ns": computed - loaded,
                       "request_ns": time.monotonic_ns() - begin})
        for detection in post["detections"]:
            print(f"{detection['class_name']} {detection['confidence']:.4f} "
                  f"{detection['box_xyxy']}", flush=True)
        print("结果已由客体保存到：" + str(output / "postprocess"), flush=True)
    except BaseException as error:
        record.update({"status": "failed", "error": f"{type(error).__name__}: {error}"})
        raise
    finally:
        try:
            finish_session(session, record)
        finally:
            write_json(output / "result.json", record)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("export", help="在构建端导出固定模型材料，不绑定真实输入")
    export.add_argument("--manifest", type=Path, default=ROOT / "build/gpgpu-model-eval/reference-v2/manifest.json")
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--model", choices=("qwen", "yolo", "all"), default="all")
    export.add_argument("--capacity", type=int, default=2048)
    export.add_argument("--prefill-chunk", type=int, default=128)
    export.set_defaults(function=export_models)
    for name, function in (("qwen-chat", qwen_chat), ("yolo", yolo_detect)):
        app = sub.add_parser(name)
        home = os.environ.get("GPGPU_MODELS_DIR")
        kind = "qwen" if name == "qwen-chat" else "yolo"
        app.add_argument("--model", type=Path, default=Path(home) / kind if home else None)
        app.add_argument("--runtime", type=Path, required=True)
        result_home = Path(os.environ.get("GPGPU_MODEL_OUTPUT", "."))
        app.add_argument("--output", type=Path,
                         default=result_home / f"{name}-{time.time_ns()}")
        app.add_argument("--timeout-ms", type=int, default=300000)
        app.set_defaults(function=function)
        if name == "qwen-chat":
            app.add_argument("--max-new-tokens", type=int, default=16)
            app.add_argument("--capacity", type=int)
            app.add_argument("--prefill-chunk", type=int)
            app.add_argument("--checkpoints", action="store_true")
        else:
            app.add_argument("--image", type=Path, required=True)
            app.add_argument("--conf", type=float, default=0.25)
            app.add_argument("--iou", type=float, default=0.7)
            app.add_argument("--max-det", type=int, default=300)
    args = parser.parse_args()
    if not sys.flags.isolated:
        parser.error("必须使用 python3 -I，避免从输入或模型目录导入代码")
    if args.command != "export" and args.model is None:
        parser.error("请提供 --model，或先进入 model-shell.sh 设置的模型环境")
    if args.command == "qwen-chat":
        if (args.max_new_tokens <= 0 or (args.capacity is not None and args.capacity <= 0) or
                (args.prefill_chunk is not None and args.prefill_chunk <= 0)):
            parser.error("生成预算、容量和分块必须为正数")
    if args.command != "export" and not 0 < args.timeout_ms <= 0xffffffff:
        parser.error("单次派发超时必须是可表示的正毫秒数")

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    args.function(args)


if __name__ == "__main__":
    main()
