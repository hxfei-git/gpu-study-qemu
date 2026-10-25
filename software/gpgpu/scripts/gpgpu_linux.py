#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""使用当前检出的路径准备、验收或启动 ARM64 驱动客体。"""

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
import urllib.request

from gpgpu_remote import RemoteDevice


SCRIPTS = Path(__file__).resolve().parent
STACK = SCRIPTS.parent
SOURCE = STACK / "linux"
GUEST_SCRIPTS = SCRIPTS / "guest"
ROOT = STACK.parents[1]
IMAGES = ROOT / "build" / "gpgpu-arm64-linux"
QEMU_DIRECTORY = ROOT / "build" / "arm64-qemu"
QEMU_BINARY = QEMU_DIRECTORY / "qemu-system-aarch64"
DEVICE_QEMU_BINARY = QEMU_DIRECTORY / "qemu-system-riscv64"
ISO_NAME = "alpine-virt-3.24.2-aarch64.iso"
ISO_SHA256 = "a57ba668b5f6b17a670fcf8e799d5d7fe43766ed086d6ce2927b0625bf43dbf6"
ISO_URL = (
    "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/" + ISO_NAME
)
RUNTIME_FILES = ("gpgpu_pci.ko", "vmlinuz-virt", "initramfs-module")
BUILD_RECORDS = ("kernel-release", "kernel-config", "kernel-Module.symvers",
                 "kernel-packages")
BUILD_INPUTS = ("gpgpu_pci_main.c", "gpgpu_memory.c", "gpgpu_queue.c",
                "gpgpu_event.c", "gpgpu_queue_drv.h", "gpgpu_regs.h",
                "Makefile", "guest-compile.sh", "../include/gpgpu_uapi.h",
                "../include/gpgpu_queue.h", "../include/gpgpu_vm.h",
                "../include/gpgpu_drm.h", "gpgpu_gpuvm.c")
BUILD_VERSION = 3
ENVIRONMENT_INPUTS = ("guest-build.sh", "developer-init.sh",
                      "guest-dev-init.sh", "module-init.sh")
ENVIRONMENT_FILES = ("vmlinuz-virt", "initramfs-module",
                     "initramfs-developer") + BUILD_RECORDS
ENVIRONMENT_VERSION = 1
DEVELOPER_DISK_SIZE = 2 * 1024 * 1024 * 1024


def sha256(path):
    """分块计算文件 SHA-256，用于核对镜像、模块和缓存内容。"""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


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


def inputs_hash(filenames, version):
    """按原逻辑名称和实际内容计算指纹，目录迁移本身不使缓存过期。"""
    digest = hashlib.sha256()
    digest.update(f"gpgpu-build:{version}\0".encode())
    for name in filenames:
        # 环境脚本只移动位置，仍用原名称参与散列，保留已安装的固定内核。
        directory = (GUEST_SCRIPTS if name in ENVIRONMENT_INPUTS or
                     name == "guest-compile.sh" else SOURCE)
        digest.update(name.encode() + b"\0" + (directory / name).read_bytes())
    return digest.hexdigest()


def source_hash():
    """按固定输入清单计算当前驱动源码与 ABI 的构建指纹。"""
    return inputs_hash(BUILD_INPUTS, BUILD_VERSION)


def ensure_qemu(output, remote=False):
    """检查当前配置是否包含所需目标与设备，并构建测试使用的 QEMU。"""
    QEMU_DIRECTORY.mkdir(parents=True, exist_ok=True)
    with (output / "qemu-build.log").open("w") as log:
        configure = not (QEMU_DIRECTORY / "build.ninja").is_file()
        if remote and not configure:
            targets = (QEMU_DIRECTORY / "config-host.mak").read_text()
            config_path = (QEMU_DIRECTORY /
                           "riscv64-softmmu-config-devices.h")
            config = config_path.read_text() if config_path.is_file() else ""
            configure = ("riscv64-softmmu" not in targets or
                         "#define CONFIG_VFIO_USER_SERVER 1" not in config or
                         "#define CONFIG_GPGPU_CP 1" not in config)
        if configure:
            target_list = "aarch64-softmmu"
            if remote:
                target_list += ",riscv64-softmmu"
            subprocess.run([
                str(ROOT / "configure"), f"--target-list={target_list}",
                "--disable-rust", "--disable-docs", "--disable-tools",
                "--disable-guest-agent", "--enable-download",
                "--extra-cflags=-O0 -g0",
            ] + (["--enable-vfio-user-server"] if remote else []),
                cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
        binaries = ["qemu-system-aarch64"]
        if remote:
            binaries += ["qemu-system-riscv64"]
        subprocess.run(["ninja", "-j4"] + binaries,
                       cwd=QEMU_DIRECTORY, stdout=log, stderr=log, check=True)
    subprocess.run([str(QEMU_BINARY), "-device", "gpgpu,help"],
                   stdout=subprocess.DEVNULL, check=True)


def bootstrap_images():
    """下载并核对固定 Alpine 镜像，从中提取首次启动所需文件。"""
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
            """按 ISO9660 目录记录读取子项，用文件名索引保留原始记录。"""
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


def base_command(memory="512M", interactive=False, iommu="none"):
    """组装 ARM64 virt 客体的公共参数，按需开启交互和 SMMUv3。"""
    machine = "virt,gic-version=3"
    if iommu == "smmuv3":
        machine += ",iommu=smmuv3,default-bus-bypass-iommu=off"
    command = [str(QEMU_BINARY), "-machine", machine,
               "-cpu", "cortex-a57", "-accel", "tcg",
               "-m", memory, "-smp", "2"]
    if interactive:
        return command + ["-nographic"]
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


def prepare_environment(output, fingerprint):
    """在首次启动客体中建立持久编译环境，校验并保存后继续编译驱动。"""
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
    command += shared_directory(GUEST_SCRIPTS, "gpgpu_src", True)
    command += shared_directory(STACK, "gpgpu_stack", True)
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


def prepare(output, force, rebuild_module=False, remote=False):
    """检查 QEMU、环境和模块指纹，按缓存有效性执行必要的准备步骤。"""
    output.mkdir(parents=True, exist_ok=True)
    print("Checking ARM64 QEMU build...", flush=True)
    ensure_qemu(output, remote)
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


def runtime_command(output, interactive=False, iommu="none", remote=False):
    """组合运行验收所需的内核、initramfs 和同步或远端设备参数。"""
    if not QEMU_BINARY.is_file() or not prepared_metadata(output):
        raise RuntimeError("Run 'make prepare' before booting the module")
    check_iommu_kernel(output, iommu)
    command = base_command(interactive=interactive, iommu=iommu) + [
        "-kernel", str(output / "vmlinuz-virt"),
        "-initrd", str(output / "initramfs-module"),
        "-append", kernel_command_line(iommu),
        "-device", "@GPGPU_DEVICE@" if remote else "gpgpu,bus=pcie.0",
    ]
    return (command + shared_directory(output, "gpgpu_module", True)
            + ["-nic", "none"])


def check_iommu_domain(guest, iommu):
    """在客体中检查设备的 IOMMU 归属，核实实际模式与请求一致。"""
    guest.send(
        "for dev in /sys/bus/pci/devices/*; do\n"
        "  test \"$(cat \"$dev/vendor\")\" = 0x1234 || continue\n"
        "  test \"$(cat \"$dev/device\")\" = 0x1337 || continue\n"
        "  group=none\n"
        "  domain=none\n"
        "  if test -e \"$dev/iommu_group\"; then\n"
        "    group=$(readlink -f \"$dev/iommu_group\")\n"
        "    domain=$(cat \"$dev/iommu_group/type\")\n"
        "  fi\n"
        "  printf 'GPGPU_IOMMU_DEVICE=%s GROUP=%s TYPE=%s\\n' "
        "\"${dev##*/}\" \"${group##*/}\" \"$domain\"\n"
        "done\n"
        "printf 'GPGPU_IOMMU_END\\n'\n"
    )
    guest.wait(rb"^GPGPU_IOMMU_END$")
    matches = re.findall(
        rb"^GPGPU_IOMMU_DEVICE=(\S+) GROUP=(\S+) TYPE=(\S+)\r?$",
        guest.buffer, re.M)
    expected = b"DMA" if iommu == "smmuv3" else b"none"
    if len(matches) != 1 or matches[0][2] != expected:
        raise RuntimeError(f"GPGPU IOMMU domain 不符合 {iommu}: {matches!r}")
    device, group, domain = (value.decode() for value in matches[0])
    print(f"GPGPU {device}: IOMMU={iommu} group={group} type={domain}",
          flush=True)
    return {"mode": iommu, "device": device, "group": group, "type": domain}


def test(output, iommu, device_qemu=None):
    """启动客体执行驱动装卸载测试，并根据实际通过标记和日志判断结果。"""
    results = output / "remote" / iommu if device_qemu else output / iommu
    results.mkdir(parents=True, exist_ok=True)
    (results / "test.json").unlink(missing_ok=True)
    remote = RemoteDevice(device_qemu, results) if device_qemu else None
    guest = Guest(runtime_command(output, iommu=iommu, remote=bool(remote)),
                  results / "test-console.log", remote)
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
            if marker == "INSMOD":
                domain = check_iommu_domain(guest, iommu)
        check_kernel_log(guest)
        write_metadata(results / "test.json", {
            "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                            time.gmtime()),
            "kernel_release": (output / "kernel-release").read_text().strip(),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "iommu": domain,
            "transport": "vfio-user" if remote else "local",
            "module_checks_passed": 5,
            "kernel_faults": 0,
            "smmu_faults": 0,
        })
    finally:
        guest.close()


def check_kernel_log(guest):
    """读取客体内核日志，拒绝包含严重内核错误的验收结果。"""
    guest.send("dmesg\n" "printf 'GPGPU_DMESG_END\\n'\n")
    guest.wait(rb"^GPGPU_DMESG_END$")
    if re.search(rb"(?:Oops:|BUG:|WARNING: CPU:|Kernel panic|"
                 rb"(?:smmu|iommu)[^\n]*\b(?:fault|error|event 0x|aborted)|"
                 rb"(?:CMDQ|EVTQ)[^\n]*(?:error|overflow))",
                 guest.buffer, re.I):
        raise RuntimeError("Guest kernel fault; inspect console log")


def stack_test(output, iommu, device_qemu=None):
    """在指定设备及 IOMMU 组合下运行同步示例和运行时测试，核对实际通过数。"""
    results = output / "remote" / iommu if device_qemu else output / iommu
    results.mkdir(parents=True, exist_ok=True)
    (results / "stack-test.json").unlink(missing_ok=True)
    if not environment_metadata(output) or not prepared_metadata(output):
        raise RuntimeError("Run 'make prepare' before testing the stack")
    stack_output = output / "stack"
    # RV32 kernel 在宿主机汇编；客体使用已保存的 C 编译器，
    # 因此重复测试无需下载开发软件包。
    subprocess.run(["make", "-C", str(STACK),
                    f"BUILD_DIR={stack_output}", "kernels"], check=True)
    remote = RemoteDevice(device_qemu, results) if device_qemu else None
    command = developer_command(output, iommu, remote=bool(remote))
    guest = Guest(command, results / "stack-console.log", remote)
    guest_log = f"/mnt/gpgpu-build/{results.relative_to(output)}/stack-test.log"
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        guest.send(
            "mkdir -p /mnt/gpgpu-stack\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
            "gpgpu_stack /mnt/gpgpu-stack\n"
            "sh /mnt/gpgpu-src/guest-test.sh sync "
            f"> {guest_log} 2>&1\n"
            "printf 'GPGPU_STACK_RC=%s\\n' \"$?\"\n"
        )
        result = guest.wait(rb"^GPGPU_STACK_RC=(\d+)$", timeout=600)
        log = (results / "stack-test.log").read_text(errors="replace")
        print(log, end="", flush=True)
        if result.group(1) != b"0":
            raise RuntimeError("Stack test failed; inspect stack-test.log")
        for pattern in (r"^GPGPU demos: 3/3 PASS$",
                        r"^GPGPU runtime tests: 68/68 PASS$",
                        r"^GPGPU unload/reload: 2/2 PASS$"):
            if len(re.findall(pattern, log, re.M)) != 1:
                raise RuntimeError("Missing complete result in stack-test.log")
        values = re.findall(r"^.*: (\d+)/(\d+) values PASS$", log, re.M)
        if (values != [("17003", "17003"), ("437", "437"), ("513", "513")]
                or len(re.findall(r"^PASS ", log, re.M)) != 68):
            raise RuntimeError("示例输出或运行时检查数量不足")
        domain = check_iommu_domain(guest, iommu)
        check_kernel_log(guest)
        write_metadata(results / "stack-test.json", {
            "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                            time.gmtime()),
            "kernel_release": (output / "kernel-release").read_text().strip(),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "demo_sha256": sha256(stack_output / "gpgpu-demo"),
            "tests_sha256": sha256(stack_output / "test-runtime"),
            "iommu": domain,
            "transport": "vfio-user" if remote else "local",
            "guest_exit_code": int(result.group(1)),
            "demo_passed": 3,
            "demo_values_passed": sum(int(passed) for passed, _ in values),
            "runtime_tests_passed": int(re.search(
                r"GPGPU runtime tests: (\d+)/\d+ PASS", log).group(1)),
            "kernel_faults": 0,
            "smmu_faults": 0,
            "unload_reload_passed": 2,
        })
    finally:
        guest.close()


def queue_test(output, device_qemu):
    """启动双进程设备执行五组队列测试，并检查设备移除及内核日志。"""
    if not device_qemu:
        raise RuntimeError("队列验证需要 --remote 和原生 RV64 CP")
    config = (output / "kernel-config").read_text()
    if "CONFIG_GPU_BUDDY=y" not in config.splitlines():
        raise RuntimeError("队列验证要求使用 GPU_BUDDY=y 的重建内核")
    results = output / "remote" / "none"
    results.mkdir(parents=True, exist_ok=True)
    (results / "queue-test.json").unlink(missing_ok=True)
    subprocess.run(["make", "-C", str(STACK / "firmware")], check=True)
    shutil.copyfile(ROOT / "build/gpgpu-firmware/gpgpu-cp.bin",
                    output / "gpgpu-cp.bin")
    stack = output / "queue-stack"
    subprocess.run(["make", "-C", str(STACK),
                    f"BUILD_DIR={stack}", "kernels"], check=True)
    remote = RemoteDevice(device_qemu, results)
    guest = Guest(developer_command(output, "none", remote=True),
                  results / "queue-console.log", remote)
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        guest.send(
            "mkdir -p /mnt/gpgpu-stack\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
            "gpgpu_stack /mnt/gpgpu-stack\n"
            "sh /mnt/gpgpu-src/guest-test.sh queue "
            "> /mnt/gpgpu-build/remote/none/queue-test.log 2>&1\n"
            "printf 'GPGPU_QUEUE_RC=%s\\n' \"$?\"\n"
        )
        result = guest.wait(rb"^GPGPU_QUEUE_RC=(\d+)$", timeout=1200)
        log = (results / "queue-test.log").read_text(errors="replace")
        print(log, end="", flush=True)
        if result.group(1) != b"0":
            raise RuntimeError("队列测试失败，请检查 queue-test.log")
        groups = re.findall(r"^PASS queue-group-([1-5])$", log, re.M)
        if groups != ["1", "2", "3", "4", "5"] or len(re.findall(
                r"^GPGPU queue tests: 5/5 PASS$", log, re.M)) != 1:
            raise RuntimeError("队列测试缺少完整的五组运行记录")
        if len(re.findall(r"^GPGPU queue unload: 1/1 PASS$", log, re.M)) != 1:
            raise RuntimeError("队列模块卸载检查未通过")
        domain = check_iommu_domain(guest, "none")
        check_kernel_log(guest)
        write_metadata(results / "queue-test.json", {
            "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "kernel_release": (output / "kernel-release").read_text().strip(),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "firmware_sha256": sha256(output / "gpgpu-cp.bin"),
            "tests_sha256": sha256(stack / "test-queue"),
            "guest_exit_code": int(result.group(1)),
            "groups_passed": len(groups), "iommu": domain,
            "transport": "vfio-user", "kernel_faults": 0,
        })
    finally:
        guest.close()


def prepare_gpuvm_firmware(output):
    """构建并复制两种 GPUVM 验收共用的 CP 固件。"""
    subprocess.run(["make", "-C", str(STACK / "firmware")], check=True)
    shutil.copy2(ROOT / "build/gpgpu-firmware/gpgpu-cp.bin",
                 output / "gpgpu-cp.bin")


def gpuvm_test(output, iommu, device_qemu):
    """在指定 IOMMU 模式下运行五组 GPUVM 检查，核对卸载及内核日志。"""
    check_iommu_kernel(output, iommu)
    prepare_gpuvm_firmware(output)
    stack = output / "gpuvm-stack"
    subprocess.run(["make", "-C", str(STACK), f"BUILD_DIR={stack}", "kernels"],
                   check=True)
    results = output / "gpuvm" / iommu
    results.mkdir(parents=True, exist_ok=True)
    (results / "result.json").unlink(missing_ok=True)
    remote = RemoteDevice(device_qemu, results, gpuvm=True)
    guest = Guest(developer_command(output, iommu, remote=True),
                  results / "console.log", remote)
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        guest.send(
            "mkdir -p /mnt/gpgpu-stack\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
            "gpgpu_stack /mnt/gpgpu-stack\n"
            "sh /mnt/gpgpu-src/guest-test.sh gpuvm "
            f"> /mnt/gpgpu-build/gpuvm/{iommu}/test.log 2>&1\n"
            "printf 'GPGPU_GPUVM_RC=%s\\n' \"$?\"\n"
        )
        result = guest.wait(rb"^GPGPU_GPUVM_RC=(\d+)$", timeout=1200)
        log = (results / "test.log").read_text(errors="replace")
        print(log, end="", flush=True)
        groups = re.findall(r"^PASS gpuvm-group-([1-5])$", log, re.M)
        if (result.group(1) != b"0" or groups != list("12345") or
                "GPUVM unload: PASS" not in log):
            raise RuntimeError("GPUVM 验收不完整，请检查 test.log")
        domain = check_iommu_domain(guest, iommu)
        check_kernel_log(guest)
        write_metadata(results / "result.json", {
            "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "groups_passed": len(groups), "iommu": domain,
            "transport": "vfio-user",
            "kernel_release": (output / "kernel-release").read_text().strip(),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "firmware_sha256": sha256(output / "gpgpu-cp.bin"),
            "test_sha256": sha256(stack / "test-gpuvm"),
            "kernel_faults": 0,
        })
    finally:
        guest.close()


def gpuvm_fault_test(output, device_qemu):
    """在独立模块副本中注入未知结果，检查三个场景的资源保留与安全回收。"""
    prepare_gpuvm_firmware(output)
    results = output / "gpuvm" / "retention"
    results.mkdir(parents=True, exist_ok=True)
    (results / "result.json").unlink(missing_ok=True)
    remote = RemoteDevice(device_qemu, results, gpuvm=True)
    guest = Guest(developer_command(output, "none", remote=True),
                  results / "console.log", remote)
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        guest.send(
            "mkdir -p /mnt/gpgpu-stack\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
            "gpgpu_stack /mnt/gpgpu-stack\n"
            "make -C /mnt/gpgpu-stack "
            "BUILD_DIR=/mnt/gpgpu-build/gpuvm-stack all "
            "> /mnt/gpgpu-build/gpuvm/retention/build.log 2>&1 &&\n"
            "sh /mnt/gpgpu-src/guest-gpuvm-fault-test.sh "
            "> /mnt/gpgpu-build/gpuvm/retention/test.log 2>&1\n"
            "printf 'GPGPU_RETAIN_RC=%s\\n' \"$?\"\n"
        )
        result = guest.wait(rb"^GPGPU_RETAIN_RC=(\d+)$", timeout=1200)
        log = (results / "test.log").read_text(errors="replace")
        print(log, end="", flush=True)
        groups = re.findall(r"^PASS gpuvm-retain-(map|unmap|invalidate)$",
                            log, re.M)
        if result.group(1) != b"0" or groups != ["map", "unmap", "invalidate"]:
            raise RuntimeError("未知结果保留测试失败")
        check_kernel_log(guest)
        write_metadata(results / "result.json", {
            "tests_passed": groups, "kernel_faults": 0,
            "test_sha256": sha256(output / "gpuvm-stack/test-gpuvm"),
            "module_sha256": sha256(output / "gpgpu_pci.ko"),
            "fault_modules": {
                name: sha256(results / f"fault-{name}.ko") for name in groups
            },
        })
    finally:
        guest.close()


def main():
    """选择准备、编译、交互运行或验收入口，串行保护同一输出目录。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "module", "test", "run",
                                            "stack-test", "queue-test",
                                            "gpuvm-test", "gpuvm-fault-test"))
    parser.add_argument("--output-dir", type=Path,
                        help="产物目录；GPUVM 默认使用独立内核环境")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--iommu", choices=("none", "smmuv3"), default="none",
                        help="系统 IOMMU 模式，默认 none")
    parser.add_argument("--skip-prepare", action="store_true",
                        help="直接验收已有产物，不重新准备或编译")
    parser.add_argument("--remote", action="store_true",
                        help="连接远端 GPGPU 与 RV64 CP")
    parser.add_argument("--device-qemu", type=Path, default=DEVICE_QEMU_BINARY,
                        help="远端设备 QEMU 路径")
    args = parser.parse_args()
    gpuvm = args.command in ("gpuvm-test", "gpuvm-fault-test")
    if gpuvm:
        # 两种 GPUVM 验收都依赖独立的 DRM 内核及远端 CP，不能重建成发行版环境。
        if args.force or args.skip_prepare:
            parser.error("GPUVM 验收不接受 --force 或 --skip-prepare")
        args.remote = True
    if args.command == "gpuvm-fault-test" and args.iommu != "none":
        parser.error("gpuvm-fault-test 使用 --iommu none")
    if args.skip_prepare and (args.command not in ("test", "stack-test", "queue-test")
                              or args.force):
        parser.error("--skip-prepare 只适用于验收，不能与 --force 同时使用")
    if args.remote and args.command not in ("test", "stack-test", "queue-test",
                                            "gpuvm-test", "gpuvm-fault-test", "run"):
        parser.error("--remote 只适用于验收或 run")
    if args.command == "queue-test" and (not args.remote or args.iommu != "none"):
        parser.error("queue-test 要求 --remote --iommu none")
    device_qemu = args.device_qemu.resolve() if args.remote else None
    if args.remote:
        def interrupted(signum, frame):
            """将终止信号转换为异常，让统一清理路径关闭客体和远端设备。"""
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, interrupted)
    default_output = "gpgpu-kernel-gpuvm" if gpuvm else "gpgpu-linux-module"
    output = (args.output_dir or ROOT / "build" / default_output).resolve()
    if args.command == "run":
        command = runtime_command(output, interactive=True, iommu=args.iommu,
                                  remote=args.remote)
        if not args.remote:
            os.execv(command[0], command)
        remote = RemoteDevice(device_qemu, output / "remote" / args.iommu)
        try:
            remote.start()
            command = [remote.device_option() if arg == "@GPGPU_DEVICE@"
                       else arg for arg in command]
            subprocess.run(command, check=True)
        finally:
            remote.close()
    else:
        output.mkdir(parents=True, exist_ok=True)
        with (output / ".build.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if gpuvm and not environment_metadata(output):
                raise RuntimeError("先完成启用 DRM 的独立内核构建与导入")
            if not args.skip_prepare:
                prepare(output, args.force, remote=args.remote)
            if args.command == "test":
                test(output, args.iommu, device_qemu)
            elif args.command == "stack-test":
                stack_test(output, args.iommu, device_qemu)
            elif args.command == "queue-test":
                queue_test(output, device_qemu)
            elif args.command == "gpuvm-test":
                gpuvm_test(output, args.iommu, device_qemu)
            elif args.command == "gpuvm-fault-test":
                gpuvm_fault_test(output, device_qemu)


if __name__ == "__main__":
    main()
