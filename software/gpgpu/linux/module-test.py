#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Prepare, test, or boot the ARM64 module using paths from this checkout."""

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
import struct
import subprocess
import tempfile
import time
import urllib.request


SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
IMAGES = ROOT / "build" / "gpgpu-arm64-linux"
QEMU_DIRECTORY = ROOT / "build" / "arm64-qemu"
QEMU_BINARY = QEMU_DIRECTORY / "qemu-system-aarch64"
ISO_NAME = "alpine-virt-3.24.2-aarch64.iso"
ISO_SHA256 = "a57ba668b5f6b17a670fcf8e799d5d7fe43766ed086d6ce2927b0625bf43dbf6"
ISO_URL = (
    "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/" + ISO_NAME
)
RUNTIME_FILES = ("gpgpu_pci.ko", "vmlinuz-virt", "initramfs-module")
BUILD_RECORDS = ("kernel-release", "kernel-config", "kernel-Module.symvers",
                 "kernel-packages")
BUILD_INPUTS = ("gpgpu_pci.c", "gpgpu_regs.h", "Makefile", "guest-compile.sh",
                "../include/gpgpu_uapi.h")
BUILD_VERSION = 3
ENVIRONMENT_INPUTS = ("guest-build.sh", "developer-init.sh",
                      "guest-dev-init.sh", "module-init.sh")
ENVIRONMENT_FILES = ("vmlinuz-virt", "initramfs-module",
                     "initramfs-developer") + BUILD_RECORDS
ENVIRONMENT_VERSION = 1
DEVELOPER_DISK_SIZE = 2 * 1024 * 1024 * 1024


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def read_metadata(output, manifest, filenames):
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
    return read_metadata(output, "module-test.json", RUNTIME_FILES)


def environment_metadata(output):
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


def inputs_hash(filenames, version):
    digest = hashlib.sha256()
    digest.update(f"gpgpu-build:{version}\0".encode())
    for name in filenames:
        digest.update(name.encode() + b"\0" + (SOURCE / name).read_bytes())
    return digest.hexdigest()


def source_hash():
    return inputs_hash(BUILD_INPUTS, BUILD_VERSION)


def ensure_qemu(output):
    QEMU_DIRECTORY.mkdir(parents=True, exist_ok=True)
    with (output / "qemu-build.log").open("w") as log:
        if not (QEMU_DIRECTORY / "build.ninja").is_file():
            subprocess.run([
                str(ROOT / "configure"), "--target-list=aarch64-softmmu",
                "--disable-rust", "--disable-docs", "--disable-tools",
                "--disable-guest-agent", "--enable-download",
                "--extra-cflags=-O0 -g0",
            ], cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
        subprocess.run(["ninja", "-j4", "qemu-system-aarch64"],
                       cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
    subprocess.run([str(QEMU_BINARY), "-device", "gpgpu,help"],
                   stdout=subprocess.DEVNULL, check=True)


def bootstrap_images():
    IMAGES.mkdir(parents=True, exist_ok=True)
    image = IMAGES / ISO_NAME
    if not image.is_file() or sha256(image) != ISO_SHA256:
        print("Downloading official Alpine ARM64 bootstrap ISO...", flush=True)
        temporary = image.with_suffix(".download")
        with urllib.request.urlopen(ISO_URL, timeout=30) as response:
            with temporary.open("wb") as target:
                while chunk := response.read(1024 * 1024):
                    target.write(chunk)
        if sha256(temporary) != ISO_SHA256:
            raise RuntimeError("Alpine ISO checksum mismatch")
        temporary.replace(image)

    with image.open("rb") as stream:
        stream.seek(16 * 2048)
        descriptor = stream.read(2048)
        if descriptor[:7] != b"\x01CD001\x01":
            raise RuntimeError("Expected ISO9660 primary volume descriptor")

        def entries(record):
            extent = struct.unpack_from("<I", record, 2)[0]
            length = struct.unpack_from("<I", record, 10)[0]
            stream.seek(extent * 2048)
            data = stream.read(length)
            offset = 0
            result = {}
            while offset < len(data):
                size = data[offset]
                if not size:
                    offset = (offset // 2048 + 1) * 2048
                    continue
                entry = data[offset:offset + size]
                name = entry[33:33 + entry[32]]
                offset += size
                if name not in (b"\x00", b"\x01"):
                    result[name.decode("ascii")] = entry
            return result

        boot = entries(entries(descriptor[156:190])["BOOT"])
        for iso_name, filename in {
            "VMLINUZ_VIRT.;1": "vmlinuz-bootstrap",
            "INITRAMFS_VIRT.;1": "initramfs-bootstrap",
        }.items():
            record = boot[iso_name]
            stream.seek(struct.unpack_from("<I", record, 2)[0] * 2048)
            length = struct.unpack_from("<I", record, 10)[0]
            data = stream.read(length)
            if len(data) != length:
                raise RuntimeError("Truncated bootstrap ISO")
            (IMAGES / filename).write_bytes(data)
    return image


def base_command(memory="512M", interactive=False):
    command = [str(QEMU_BINARY), "-machine", "virt,gic-version=3",
               "-cpu", "cortex-a57", "-accel", "tcg",
               "-m", memory, "-smp", "2"]
    if interactive:
        return command + ["-nographic"]
    return command + ["-display", "none", "-serial", "stdio",
                      "-monitor", "none"]


def shared_directory(path, tag, readonly):
    option = f"local,path={path},mount_tag={tag},security_model=none"
    if readonly:
        option += ",readonly=on"
    return ["-virtfs", option]


class Guest:
    def __init__(self, command, logfile):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
        self.log = logfile.open("wb")
        self.buffer = b""
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def send(self, text):
        self.process.stdin.write(text.encode())
        self.process.stdin.flush()

    def wait(self, pattern, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, self.buffer.replace(b"\r", b""), re.M)
            if match:
                return match
            if b"Kernel panic" in self.buffer:
                raise RuntimeError("Guest kernel panic; inspect console log")
            for key, _ in self.selector.select(timeout=1):
                data = os.read(key.fd, 65536)
                if not data:
                    raise RuntimeError("QEMU exited before expected response")
                self.log.write(data)
                self.log.flush()
                self.buffer = (self.buffer + data)[-2 * 1024 * 1024:]
        raise TimeoutError("Guest timeout; inspect console log")

    def close(self):
        try:
            if self.process.poll() is None:
                self.send("poweroff -f\n")
                self.process.wait(timeout=15)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.terminate()
            self.process.wait(timeout=15)
        finally:
            self.selector.close()
            self.log.close()


def module_metadata(module, runtime, fingerprint, environment):
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
    path.write_text(json.dumps(metadata, indent=2) + "\n")


def prepare_environment(output, fingerprint):
    image = bootstrap_images()
    print("Preparing persistent ARM64 compiler environment (once)...",
          flush=True)
    stage = Path(tempfile.mkdtemp(prefix=".environment-", dir=output))
    with (stage / "developer.raw").open("wb") as disk:
        disk.truncate(DEVELOPER_DISK_SIZE)
    command = base_command("2G") + [
        "-kernel", str(IMAGES / "vmlinuz-bootstrap"),
        "-initrd", str(IMAGES / "initramfs-bootstrap"),
        "-append", "console=ttyAMA0 "
        "modules=loop,squashfs,virtio_pci,virtio_blk "
        "alpine_dev=/dev/vda:iso9660 modloop=/boot/modloop-virt",
        "-drive", f"file={image},if=none,id=bootiso,format=raw,readonly=on",
        "-device", "virtio-blk-pci,drive=bootiso",
        "-drive", f"file={stage / 'developer.raw'},if=none,id=developer,"
        "format=raw",
        "-device", "virtio-blk-pci,drive=developer",
        "-device", "gpgpu,bus=pcie.0",
    ]
    command += shared_directory(SOURCE, "gpgpu_src", True)
    command += shared_directory(SOURCE.parent, "gpgpu_stack", True)
    command += shared_directory(output, "gpgpu_build", False)
    command += ["-netdev", "user,id=net0",
                "-device", "virtio-net-pci,netdev=net0"]
    environment_saved = False
    try:
        guest = Guest(command, output / "environment-console.log")
        try:
            guest.wait(rb"localhost login:")
            guest.send("root\n")
            guest.wait(rb"localhost:~# ")
            guest.send(
                "mkdir -p /mnt/gpgpu-src /mnt/gpgpu-build\n"
                "modprobe 9pnet_virtio\n"
                "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
                "gpgpu_src /mnt/gpgpu-src\n"
                "mount -t 9p -o trans=virtio,version=9p2000.L "
                "gpgpu_build /mnt/gpgpu-build\n"
                "sh /mnt/gpgpu-src/guest-build.sh "
                + shlex.quote(stage.name) + " "
                "> /mnt/gpgpu-build/environment-build.log 2>&1\n"
                "printf 'GPGPU_ENVIRONMENT_RC=%s\\n' \"$?\"\n"
            )
            result = guest.wait(rb"^GPGPU_ENVIRONMENT_RC=(\d+)$", timeout=1800)
            if result.group(1) != b"0":
                raise RuntimeError(
                    "Environment setup failed; inspect environment-build.log")
        finally:
            guest.close()

        release = (stage / "kernel-release").read_text().strip()
        environment = {
            "source_hash": inputs_hash(ENVIRONMENT_INPUTS,
                                       ENVIRONMENT_VERSION),
            "kernel_release": release,
            "disk_size": DEVELOPER_DISK_SIZE,
            "files": {name: sha256(stage / name)
                      for name in ENVIRONMENT_FILES},
        }
        write_metadata(stage / "environment.json", environment)
        for name in ENVIRONMENT_FILES + ("developer.raw",):
            (stage / name).replace(output / name)
        (stage / "environment.json").replace(output / "environment.json")
        environment_saved = True
    finally:
        if environment_saved:
            shutil.rmtree(stage)
        else:
            print(f"Environment staging assets retained at {stage}",
                  flush=True)
    print(f"Saved ARM64 compiler environment for {release}", flush=True)
    compile_module(output, fingerprint, environment)


def compile_module(output, fingerprint, environment):
    print("Incrementally building module with the saved compiler...",
          flush=True)
    stage = Path(tempfile.mkdtemp(prefix=".module-", dir=output))
    command = developer_command(output)
    try:
        guest = Guest(command, output / "module-console.log")
        try:
            guest.wait(rb"^GPGPU_DEVELOPER_READY$")
            guest.send(
                "sh /mnt/gpgpu-src/guest-compile.sh "
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


def developer_command(output):
    command = base_command("1G") + [
        "-kernel", str(output / "vmlinuz-virt"),
        "-initrd", str(output / "initramfs-developer"),
        "-append", "console=ttyAMA0",
        "-drive", f"file={output / 'developer.raw'},if=none,id=developer,"
        "format=raw",
        "-device", "virtio-blk-pci,drive=developer",
        "-device", "gpgpu,bus=pcie.0", "-nic", "none",
    ]
    command += shared_directory(SOURCE, "gpgpu_src", True)
    command += shared_directory(SOURCE.parent, "gpgpu_stack", True)
    command += shared_directory(output, "gpgpu_build", False)
    return command


def prepare(output, force, rebuild_module=False):
    output.mkdir(parents=True, exist_ok=True)
    print("Checking ARM64 QEMU build...", flush=True)
    ensure_qemu(output)
    fingerprint = source_hash()
    environment = environment_metadata(output)
    if (force or not environment or environment.get("source_hash")
            != inputs_hash(ENVIRONMENT_INPUTS, ENVIRONMENT_VERSION)):
        prepare_environment(output, fingerprint)
        return
    print("Using saved kernel and ARM64 compiler environment.", flush=True)
    metadata = prepared_metadata(output)
    if (not rebuild_module and metadata
            and metadata.get("source_hash") == fingerprint
            and metadata.get("environment_hash") == environment["source_hash"]):
        print("Module is already up to date.", flush=True)
        return
    compile_module(output, fingerprint, environment)


def runtime_command(output, interactive=False):
    if not QEMU_BINARY.is_file() or not prepared_metadata(output):
        raise RuntimeError("Run 'make prepare' before booting the module")
    command = base_command(interactive=interactive) + [
        "-kernel", str(output / "vmlinuz-virt"),
        "-initrd", str(output / "initramfs-module"),
        "-append", "console=ttyAMA0", "-device", "gpgpu,bus=pcie.0",
    ]
    return (command + shared_directory(output, "gpgpu_module", True)
            + ["-nic", "none"])


def test(output):
    guest = Guest(runtime_command(output), output / "test-console.log")
    try:
        guest.wait(rb"^.*# $")
        for command, marker in [
            ("insmod /mnt/gpgpu/gpgpu_pci.ko", "INSMOD"),
            ("test -d /sys/module/gpgpu_pci", "PRESENT"),
            ("lsmod | grep '^gpgpu_pci '", "LSMOD"),
            ("rmmod gpgpu_pci", "RMMOD"),
            ("test ! -d /sys/module/gpgpu_pci", "REMOVED"),
        ]:
            guest.send(command + "\n" +
                       f"printf 'GPGPU_{marker}_RC=%s\\n' \"$?\"\n")
            result = guest.wait(fr"^GPGPU_{marker}_RC=(\d+)$".encode())
            if result.group(1) != b"0":
                raise RuntimeError(f"{marker} failed; inspect test-console.log")
            print(f"{marker}: passed", flush=True)
        check_kernel_log(guest)
    finally:
        guest.close()


def check_kernel_log(guest):
    guest.send("dmesg\n" "printf 'GPGPU_DMESG_END\\n'\n")
    guest.wait(rb"^GPGPU_DMESG_END$")
    if re.search(rb"(?:Oops:|BUG:|Kernel panic)", guest.buffer):
        raise RuntimeError("Guest kernel fault; inspect console log")


def stack_test(output):
    (output / "stack-test.json").unlink(missing_ok=True)
    if not environment_metadata(output) or not prepared_metadata(output):
        raise RuntimeError("Run 'make prepare' before testing the stack")
    stack_output = output / "stack"
    # RV32 kernel 在宿主机汇编；客体使用已保存的 C 编译器，
    # 因此重复测试无需下载开发软件包。
    subprocess.run(["make", "-C", str(SOURCE.parent),
                    f"BUILD_DIR={stack_output}", "kernels"], check=True)
    command = developer_command(output)
    guest = Guest(command, output / "stack-console.log")
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        guest.send(
            "mkdir -p /mnt/gpgpu-stack\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
            "gpgpu_stack /mnt/gpgpu-stack\n"
            "sh /mnt/gpgpu-src/guest-stack-test.sh "
            "> /mnt/gpgpu-build/stack-test.log 2>&1\n"
            "printf 'GPGPU_STACK_RC=%s\\n' \"$?\"\n"
        )
        result = guest.wait(rb"^GPGPU_STACK_RC=(\d+)$", timeout=600)
        log = (output / "stack-test.log").read_text(errors="replace")
        print(log, end="", flush=True)
        if result.group(1) != b"0":
            raise RuntimeError("Stack test failed; inspect stack-test.log")
        for pattern in (r"GPGPU demos: 3/3 PASS",
                        r"GPGPU runtime tests: (\d+)/(\d+) PASS",
                        r"GPGPU unload/reload: 2/2 PASS"):
            match = re.search(pattern, log)
            if not match or (match.groups() and
                             (match.group(1) != match.group(2) or
                              int(match.group(1)) == 0)):
                raise RuntimeError("Missing complete result in stack-test.log")
        check_kernel_log(guest)
        write_metadata(output / "stack-test.json", {
            "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                            time.gmtime()),
            "kernel_release": (output / "kernel-release").read_text().strip(),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "demo_sha256": sha256(stack_output / "gpgpu-demo"),
            "tests_sha256": sha256(stack_output / "test-runtime"),
            "demo_passed": 3,
            "runtime_tests_passed": int(re.search(
                r"GPGPU runtime tests: (\d+)/\d+ PASS", log).group(1)),
            "kernel_faults": 0,
        })
    finally:
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "module", "test", "run",
                                            "stack-test"))
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build" / "gpgpu-linux-module")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--skip-prepare", action="store_true",
                        help="Test existing artifacts without rebuilding")
    args = parser.parse_args()
    if args.skip_prepare and (args.command not in ("test", "stack-test")
                              or args.force):
        parser.error("--skip-prepare requires a test without --force")
    output = args.output_dir.resolve()
    if args.command == "run":
        command = runtime_command(output, interactive=True)
        os.execv(command[0], command)
    else:
        output.mkdir(parents=True, exist_ok=True)
        with (output / ".build.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if not args.skip_prepare:
                prepare(output, args.force)
            if args.command == "test":
                test(output)
            elif args.command == "stack-test":
                stack_test(output)


if __name__ == "__main__":
    main()
