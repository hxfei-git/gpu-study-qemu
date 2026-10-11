#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""启动远端 GPGPU，并在 Host 退出或超时后清理两个进程。"""

import argparse
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time


class RemoteDevice:
    def __init__(self, binary, log_directory, startup_timeout=15):
        self.binary = Path(binary)
        self.log_directory = Path(log_directory)
        self.startup_timeout = startup_timeout
        self.process = None
        self.log = None
        self.directory = None

    def start(self):
        self.log_directory.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="gpgpu-remote-")
        self.socket = str(Path(self.directory.name) / "gpu.sock")
        server = {
            "qom-type": "x-vfio-user-server", "id": "vfu0",
            "device": "gpu0", "socket": {"type": "unix", "path": self.socket},
        }
        command = [
            str(self.binary), "-machine",
            "x-remote,vfio-user=on,auto-shutdown=off",
            "-accel", "tcg", "-run-with", "exit-with-parent=on",
            "-nodefaults", "-display", "none", "-monitor", "none",
            "-serial", "none", "-device", "gpgpu,id=gpu0",
            "-object", json.dumps(server),
        ]
        (self.log_directory / "device-command.json").write_text(
            json.dumps(command, ensure_ascii=False, indent=2) + "\n")
        try:
            self.log = (self.log_directory / "device.log").open("wb")
            self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                            stdout=self.log, stderr=self.log)
            deadline = time.monotonic() + self.startup_timeout
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(
                        "远端 GPGPU 启动失败，请检查 device.log")
                if Path(self.socket).is_socket():
                    return self
                time.sleep(0.02)
            raise TimeoutError(
                "等待远端 GPGPU socket 超时，请检查 device.log")
        except BaseException:
            self.close()
            raise

    def device_option(self):
        return json.dumps({
            "driver": "vfio-user-pci", "bus": "pcie.0",
            "socket": {"type": "unix", "path": self.socket},
            "x-dma-via-socket": True,
        })

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log is not None:
            self.log.close()
        if self.directory is not None:
            self.directory.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-qemu", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path,
                        default=Path("build/gpgpu-remote"))
    parser.add_argument("--startup-timeout", type=float, default=15)
    parser.add_argument("--timeout", type=float, default=0,
                        help="Host 最大运行秒数；0 表示不限时")
    parser.add_argument("host_command", nargs=argparse.REMAINDER,
                        help="Host 命令使用 -device @GPGPU_DEVICE@")
    args = parser.parse_args()
    command = args.host_command
    if command and command[0] == "--":
        command = command[1:]
    if not command or "@GPGPU_DEVICE@" not in command:
        parser.error("Host 命令必须包含 -device @GPGPU_DEVICE@")
    if args.startup_timeout <= 0 or args.timeout < 0:
        parser.error("启动超时须大于 0，运行超时不能小于 0")

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupted)
    remote = RemoteDevice(args.device_qemu, args.log_dir, args.startup_timeout)
    host = None
    try:
        remote.start()
        command = [remote.device_option() if arg == "@GPGPU_DEVICE@" else arg
                   for arg in command]
        (args.log_dir / "host-command.json").write_text(
            json.dumps(command, ensure_ascii=False, indent=2) + "\n")
        with (args.log_dir / "host.log").open("wb") as log:
            host = subprocess.Popen(command, stdout=log, stderr=log)
            deadline = time.monotonic() + args.timeout if args.timeout else None
            while host.poll() is None:
                if remote.process.poll() is not None:
                    raise RuntimeError(
                        "远端 GPGPU 提前退出，请检查 device.log")
                if deadline and time.monotonic() >= deadline:
                    raise TimeoutError("Host 运行超时，请检查 host.log")
                time.sleep(0.1)
            return host.returncode
    finally:
        if host is not None and host.poll() is None:
            host.terminate()
            try:
                host.wait(timeout=5)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait()
        remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
