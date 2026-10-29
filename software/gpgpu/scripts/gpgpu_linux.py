#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""ARM64 驱动客体的模块编译、交互运行与核心验收。

子命令：
  module      在已保存的开发客体中重编驱动模块
  run         启动远端 CP 设备与开发客体，进入交互 shell
  stack-test  GPUVM 开、关各运行一次三个计算示例，并核对结果

所有子命令都需要先用 kernel-build.py 准备好内核环境。
"""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import signal
import struct
import subprocess
import tempfile
import time

from gpgpu_remote import RemoteDevice


SCRIPTS = Path(__file__).resolve().parent
STACK = SCRIPTS.parent
SOURCE = STACK / "linux"
GUEST_SCRIPTS = SCRIPTS / "guest"
ROOT = STACK.parents[1]
QEMU_DIRECTORY = ROOT / "build" / "arm64-qemu"
QEMU_BINARY = QEMU_DIRECTORY / "qemu-system-aarch64"
DEVICE_QEMU_BINARY = QEMU_DIRECTORY / "qemu-system-riscv64"
# 模块清单 module-test.json 中登记的运行文件。
RUNTIME_FILES = ("gpgpu_pci.ko", "vmlinuz-virt")
BUILD_RECORDS = ("kernel-release", "kernel-config", "kernel-Module.symvers",
                 "kernel-packages")
# 驱动构建指纹的输入：驱动源码、共享协议头（相对 linux/）与客体编译脚本。
BUILD_INPUT_GLOBS = ("Makefile", "*.c", "*.h",
                     "../include/gpgpu/*.h", "../include/gpgpu/uapi/*.h")
BUILD_SCRIPT = "build-module.sh"
# 指纹格式版本：输入集合或计算方式改变时递增，使已有模块缓存失效。
BUILD_VERSION = 6
# 写入 environment.json 的客体初始化脚本指纹，仅作记录，不参与缓存判断。
ENVIRONMENT_INPUTS = ("developer-init.sh", "guest-dev-init.sh")
ENVIRONMENT_FILES = ("vmlinuz-virt", "initramfs-developer") + BUILD_RECORDS
# 环境指纹格式版本。
ENVIRONMENT_VERSION = 2


def file_digest(path, algorithm="sha256"):
    """分块计算文件散列，避免一次载入大文件。"""
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def sha256(path):
    """文件的 SHA-256，用于核对镜像、模块和缓存内容。"""
    return file_digest(path)


def read_metadata(output, manifest, filenames):
    """读取构建清单并核对全部产物散列，不完整或过期缓存返回 None。"""
    try:
        metadata = json.loads((output / manifest).read_text())
        if not isinstance(metadata, dict):
            return None
        hashes = metadata.get("files")
        if not isinstance(hashes, dict):
            return None
        if any(sha256(output / name) != hashes.get(name)
               for name in filenames):
            return None
        return metadata
    except (OSError, ValueError):
        return None


def prepared_metadata(output):
    """检查已有驱动模块和运行时镜像是否与构建记录一致。"""
    return read_metadata(output, "module-test.json", RUNTIME_FILES)


def environment_metadata(output):
    """核对开发环境产物、内核版本及磁盘大小，判断缓存能否复用。"""
    metadata = read_metadata(output, "environment.json", ENVIRONMENT_FILES)
    if not metadata:
        return None
    try:
        release = (output / "kernel-release").read_text().strip()
        if not release or metadata.get("kernel_release") != release:
            return None
        size = (output / "developer.raw").stat().st_size
        if size != metadata.get("disk_size"):
            return None
    except (OSError, ValueError):
        return None
    return metadata


def inputs_hash(directory, names, version):
    """按文件名与内容计算指纹；names 为相对 directory 的路径。"""
    digest = hashlib.sha256()
    digest.update(f"gpgpu-build:{version}\0".encode())
    for name in names:
        digest.update(name.encode() + b"\0" + (directory / name).read_bytes())
    return digest.hexdigest()


def environment_hash():
    """客体初始化脚本的指纹，写入 environment.json 作为记录。"""
    return inputs_hash(GUEST_SCRIPTS, ENVIRONMENT_INPUTS, ENVIRONMENT_VERSION)


def build_inputs():
    """驱动构建指纹的输入文件，路径相对 linux/ 并排序。"""
    return tuple(sorted({os.path.relpath(path, SOURCE)
                         for pattern in BUILD_INPUT_GLOBS
                         for path in SOURCE.glob(pattern)}))


def source_hash():
    """按驱动源码、协议头与客体编译脚本计算当前构建指纹。"""
    digest = hashlib.sha256()
    digest.update(inputs_hash(SOURCE, build_inputs(), BUILD_VERSION).encode())
    digest.update((GUEST_SCRIPTS / BUILD_SCRIPT).read_bytes())
    return digest.hexdigest()


def qemu_configured():
    """已有构建目录是否同时包含 aarch64 Host 与带 vfio-user 的 riscv64 远端 CP。"""
    if not (QEMU_DIRECTORY / "build.ninja").is_file():
        return False
    targets = (QEMU_DIRECTORY / "config-host.mak").read_text()
    config_path = QEMU_DIRECTORY / "riscv64-softmmu-config-devices.h"
    config = config_path.read_text() if config_path.is_file() else ""
    return ("riscv64-softmmu" in targets and
            "#define CONFIG_VFIO_USER_SERVER 1" in config and
            "#define CONFIG_GPGPU_CP 1" in config)


def ensure_qemu(output):
    """1. 构建目录缺少所需目标时重新配置；2. 构建 Host 与远端 CP 两个 QEMU。"""
    QEMU_DIRECTORY.mkdir(parents=True, exist_ok=True)
    with (output / "qemu-build.log").open("w") as log:
        if not qemu_configured():
            subprocess.run([
                str(ROOT / "configure"), "--target-list=aarch64-softmmu,riscv64-softmmu",
                "--disable-rust", "--disable-docs", "--disable-tools",
                "--disable-guest-agent", "--enable-download",
                "--extra-cflags=-O0 -g0", "--enable-vfio-user-server",
            ], cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
        subprocess.run(["ninja", "-j4", "qemu-system-aarch64", "qemu-system-riscv64"],
                       cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
    subprocess.run([str(QEMU_BINARY), "-device", "gpgpu,help"],
                   stdout=subprocess.DEVNULL, check=True)


def base_command(memory="512M", iommu="none"):
    """组装 ARM64 virt 客体的公共参数：串口接到 stdio，按需开启 SMMUv3。"""
    machine = "virt,gic-version=3"
    if iommu == "smmuv3":
        machine += ",iommu=smmuv3,default-bus-bypass-iommu=off"
    command = [str(QEMU_BINARY), "-machine", machine,
               "-cpu", "cortex-a57", "-accel", "tcg",
               "-m", memory, "-smp", "2"]
    return command + ["-display", "none", "-serial", "stdio",
                      "-monitor", "none"]


def kernel_command_line(iommu):
    """生成客体控制台与 IOMMU 模式对应的内核参数。"""
    command = "console=ttyAMA0"
    if iommu == "smmuv3":
        command += " iommu.passthrough=0 iommu.strict=1"
    return command


def check_iommu_kernel(output, iommu):
    """启用 SMMUv3 前检查已保存的客体内核是否具备相应配置。"""
    if iommu == "smmuv3":
        config = (output / "kernel-config").read_text().splitlines()
        for option in ("CONFIG_ARM_SMMU_V3", "CONFIG_IOMMU_SUPPORT",
                       "CONFIG_IOMMU_DMA"):
            # 最小运行 initramfs 不额外装载 SMMU 模块。
            if f"{option}=y" not in config:
                raise RuntimeError(f"客体内核缺少 {option}=y")


def shared_directory(path, tag, readonly):
    """将目录、标签和只读选项转换为 QEMU 9p 共享参数。"""
    option = f"local,path={path},mount_tag={tag},security_model=none"
    if readonly:
        option += ",readonly=on"
    return ["-virtfs", option]


class Guest:
    """管理一台测试客体的进程、串口日志与可选的远端设备进程。"""
    def __init__(self, command, logfile, remote=None):
        """先启动可选的远端设备，再创建客体进程并登记串口输出监听。"""
        self.remote = remote
        try:
            if remote:
                remote.start()
                command = [remote.device_option() if arg == "@GPGPU_DEVICE@"
                           else arg for arg in command]
                (remote.log_directory / "host-command.json").write_text(
                    json.dumps(command, ensure_ascii=False, indent=2) + "\n")
            self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT)
        except BaseException:
            if remote:
                remote.close()
            raise
        self.log = logfile.open("wb")
        self.buffer = b""
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def send(self, text):
        """向客体串口发送文本并立即刷新，使测试命令及时进入客体。"""
        self.process.stdin.write(text.encode())
        self.process.stdin.flush()

    def wait(self, pattern, timeout=120):
        """持续读取并保存串口输出，匹配预期标记，同时检查 panic、退出和超时。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, self.buffer.replace(b"\r", b""), re.M)
            if match:
                return match
            if b"Kernel panic" in self.buffer:
                raise RuntimeError("Guest kernel panic; inspect console log")
            if self.remote and self.remote.process.poll() is not None:
                raise RuntimeError(
                    "远端 GPGPU 提前退出，请检查 device.log")
            for key, _ in self.selector.select(timeout=1):
                data = os.read(key.fd, 65536)
                if not data:
                    raise RuntimeError("QEMU exited before expected response")
                self.log.write(data)
                self.log.flush()
                self.buffer = (self.buffer + data)[-2 * 1024 * 1024:]
        raise TimeoutError("Guest timeout; inspect console log")

    def close(self):
        """先请求客体关机，超时再终止进程，最后释放日志及远端设备资源。"""
        try:
            if self.process.poll() is None:
                self.send("poweroff -f\n")
                self.process.wait(timeout=15)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        finally:
            self.selector.close()
            self.log.close()
            if self.remote:
                self.remote.close()


def module_metadata(module, runtime, fingerprint, environment):
    """校验模块的 ARM64 ELF 身份和内核版本，生成关联运行环境的散列清单。"""
    data = module.read_bytes()
    if (len(data) < 20 or data[:6] != b"\x7fELF\x02\x01"
            or struct.unpack_from("<H", data, 18)[0] != 183):
        raise RuntimeError("Expected a little-endian ARM64 module")
    release = environment["kernel_release"]
    vermagic = next(part.decode() for part in data.split(b"\0")
                    if part.startswith(b"vermagic="))
    if not vermagic.startswith("vermagic=" + release + " "):
        raise RuntimeError("Module and runtime kernel versions differ")
    return {
        "source_hash": fingerprint,
        "environment_hash": environment["source_hash"],
        "kernel_release": release,
        "vermagic": vermagic.removeprefix("vermagic="),
        "files": {name: sha256(module if name == "gpgpu_pci.ko"
                               else runtime / name) for name in RUNTIME_FILES},
    }


def write_metadata(path, metadata):
    """将构建记录以稳定缩进写为 JSON，并保留文件末尾换行。"""
    path.write_text(json.dumps(metadata, indent=2) + "\n")


def compile_module(output, fingerprint, environment):
    """使用已保存的客体开发环境增量编译驱动，校验后替换模块和清单。"""
    print("Incrementally building module with the saved compiler...",
          flush=True)
    stage = Path(tempfile.mkdtemp(prefix=".module-", dir=output))
    command = developer_command(output)
    try:
        guest = Guest(command, output / "module-console.log")
        try:
            guest.wait(rb"^GPGPU_DEVELOPER_READY$")
            guest.send(
                f"sh /mnt/gpgpu-src/{BUILD_SCRIPT} "
                + shlex.quote(stage.name) + " "
                "> /mnt/gpgpu-build/module-build.log 2>&1\n"
                "printf 'GPGPU_MODULE_RC=%s\\n' \"$?\"\n"
            )
            result = guest.wait(rb"^GPGPU_MODULE_RC=(\d+)$", timeout=600)
            if result.group(1) != b"0":
                raise RuntimeError(
                    "Module build failed; inspect module-build.log")
        finally:
            guest.close()
        metadata = module_metadata(stage / "gpgpu_pci.ko", output,
                                   fingerprint, environment)
        write_metadata(stage / "module-test.json", metadata)
        (stage / "gpgpu_pci.ko").replace(output / "gpgpu_pci.ko")
        (stage / "module-test.json").replace(output / "module-test.json")
    finally:
        shutil.rmtree(stage)
    print(f"Updated {output / 'gpgpu_pci.ko'}", flush=True)


def developer_command(output, iommu="none", remote=False):
    """组合开发客体的内核、持久磁盘、共享目录与设备连接参数。"""
    check_iommu_kernel(output, iommu)
    command = base_command("1G", iommu=iommu) + [
        "-kernel", str(output / "vmlinuz-virt"),
        "-initrd", str(output / "initramfs-developer"),
        "-append", kernel_command_line(iommu),
        "-drive", f"file={output / 'developer.raw'},if=none,id=developer,"
        "format=raw",
        "-device", "virtio-blk-pci,drive=developer",
        "-device", "@GPGPU_DEVICE@" if remote else "gpgpu,bus=pcie.0",
        "-nic", "none",
    ]
    command += shared_directory(GUEST_SCRIPTS, "gpgpu_src", True)
    command += shared_directory(STACK, "gpgpu_stack", True)
    command += shared_directory(output, "gpgpu_build", False)
    return command


def check_kernel_log(guest):
    """读取客体内核日志，拒绝包含严重内核错误的验收结果。"""
    guest.send("dmesg\n" "printf 'GPGPU_DMESG_END\\n'\n")
    guest.wait(rb"^GPGPU_DMESG_END$")
    if re.search(rb"(?:Oops:|BUG:|WARNING: CPU:|Kernel panic|"
                 rb"(?:smmu|iommu)[^\n]*\b(?:fault|error|event 0x|aborted)|"
                 rb"(?:CMDQ|EVTQ)[^\n]*(?:error|overflow))",
                 guest.buffer, re.I):
        raise RuntimeError("Guest kernel fault; inspect console log")


def prepare_firmware(output):
    """构建 RV64 CP 固件并复制到输出目录，客体从这里装入 /lib/firmware。"""
    subprocess.run(["make", "-C", str(STACK / "firmware")], check=True)
    shutil.copy2(ROOT / "build/gpgpu-firmware/gpgpu-cp.bin", output / "gpgpu-cp.bin")


# 三个示例在两种模式下都必须给出的逐元素比较结果。
EXPECTED_VALUES = [("17003", "17003"), ("437", "437"), ("513", "513")]


def check_compute_log(log, mode):
    """核对示例输出：数值比较、资源回收与设备报告的地址模式。"""
    values = re.findall(r"^.*: (\d+)/(\d+) values PASS$", log, re.M)
    if values != EXPECTED_VALUES:
        raise RuntimeError("三个示例的数值比较结果不完整")
    if not re.search(r"^GPGPU demos: 3/3 PASS$", log, re.M):
        raise RuntimeError("示例未正常完成回收")
    if f"GPGPU DRM ABI=2 GPUVM={mode}" not in log:
        raise RuntimeError("设备返回的地址模式或接口版本与本次运行不符")
    return values


def stack_test(output, device_qemu):
    """在 A57、1 GiB、IOMMU=none 下运行 GPUVM 开和关的三个计算示例。

    1. 宿主机构建固件与 Kernel（客体没有 Python，Kernel 头文件经 9p 共享给客体）；
    2. 每种模式各启动一对远端设备与客体，运行 run-demo.sh 并核对输出与内核日志；
    3. 两种模式都通过后写出 core/summary.json。
    """
    prepare_firmware(output)
    stack_output = output / "stack"
    subprocess.run(["make", "-C", str(STACK), f"BUILD_DIR={stack_output}", "kernels"],
                   check=True)
    summary = {}
    for enabled in (True, False):
        mode = "on" if enabled else "off"
        results = output / "core" / mode
        results.mkdir(parents=True, exist_ok=True)
        (results / "result.json").unlink(missing_ok=True)
        remote = RemoteDevice(device_qemu, results, gpuvm=enabled)
        guest = Guest(developer_command(output, "none", remote=True),
                      results / "console.log", remote)
        try:
            guest.wait(rb"^GPGPU_DEVELOPER_READY$")
            guest.send(
                "mkdir -p /mnt/gpgpu-stack\n"
                "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
                "gpgpu_stack /mnt/gpgpu-stack\n"
                "sh /mnt/gpgpu-src/run-demo.sh "
                f"> /mnt/gpgpu-build/core/{mode}/compute.log 2>&1\n"
                "printf 'GPGPU_STACK_RC=%s\\n' \"$?\"\n"
            )
            result = guest.wait(rb"^GPGPU_STACK_RC=(\d+)$", timeout=1200)
            log = (results / "compute.log").read_text(errors="replace")
            print(f"GPUVM={mode}\n{log}", end="", flush=True)
            if result.group(1) != b"0":
                raise RuntimeError(f"GPUVM={mode} 计算失败；查看 compute.log")
            values = check_compute_log(log, mode)
            check_kernel_log(guest)
            write_metadata(results / "result.json", {
                "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "kernel_release": (output / "kernel-release").read_text().strip(),
                "gpuvm": mode, "platform": "virt", "cpu": "cortex-a57",
                "memory": "1G", "iommu": "none", "transport": "vfio-user",
                "module_sha256": sha256(output / "gpgpu_pci.ko"),
                "firmware_sha256": sha256(output / "gpgpu-cp.bin"),
                "demo_sha256": sha256(stack_output / "gpgpu-demo"),
                "guest_exit_code": int(result.group(1)),
                "computed_values": [int(passed) for passed, _ in values],
            })
            summary[mode] = "pass"
        finally:
            guest.close()
    write_metadata(output / "core" / "summary.json", {
        "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "modes": summary,
    })


def run_interactive(output, device_qemu, iommu, gpuvm):
    """启动远端设备与开发客体，串口直接交给当前终端。"""
    remote = RemoteDevice(device_qemu, output / "interactive", gpuvm=gpuvm)
    try:
        remote.start()
        command = [remote.device_option() if arg == "@GPGPU_DEVICE@" else arg
                   for arg in developer_command(output, iommu, remote=True)]
        subprocess.run(command, check=True)
    finally:
        remote.close()


def main():
    """解析子命令，持输出目录锁后按需重编模块，再执行验收或交互运行。"""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("module", "run", "stack-test"))
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/gpgpu-kernel-gpuvm")
    parser.add_argument("--device-qemu", type=Path, default=DEVICE_QEMU_BINARY)
    parser.add_argument("--iommu", choices=("none", "smmuv3"), default="none",
                        help="交互运行的系统 IOMMU 配置；核心计算固定 none")
    parser.add_argument("--gpuvm", choices=("on", "off"), default="on",
                        help="交互运行的设备属性；核心计算自动运行两种状态")
    args = parser.parse_args()
    if args.command == "stack-test" and (args.iommu != "none" or args.gpuvm != "on"):
        parser.error("stack-test 固定运行 IOMMU=none、GPUVM=on/off")
    output = args.output_dir.resolve()
    device_qemu = args.device_qemu.resolve()
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        environment = environment_metadata(output)
        if not environment:
            raise RuntimeError("先通过 kernel-build.py 准备启用 DRM/GPU_BUDDY 的内核环境")
        ensure_qemu(output)
        metadata = prepared_metadata(output)
        fingerprint = source_hash()
        if args.command == "module" or not metadata or metadata.get("source_hash") != fingerprint:
            compile_module(output, fingerprint, environment)
        if args.command == "stack-test":
            stack_test(output, device_qemu)
        elif args.command == "run":
            prepare_firmware(output)
            run_interactive(output, device_qemu, args.iommu, args.gpuvm == "on")


if __name__ == "__main__":
    main()
