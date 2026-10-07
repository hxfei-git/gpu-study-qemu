#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Prepare, test, or boot the ARM64 module using paths from this checkout."""

import argparse
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


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def prepared_metadata(output):
    try:
        metadata = json.loads((output / "module-test.json").read_text())
        if not isinstance(metadata, dict):
            return None
        hashes = metadata.get("files")
        if not isinstance(hashes, dict):
            return None
        if any(sha256(output / name) != hashes.get(name)
               for name in RUNTIME_FILES):
            return None
        return metadata
    except (OSError, ValueError):
        return None


def source_hash():
    digest = hashlib.sha256()
    for path in sorted(SOURCE.iterdir()):
        if path.is_file():
            digest.update(path.name.encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


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


def prepare(output, force):
    output.mkdir(parents=True, exist_ok=True)
    print("Checking ARM64 QEMU build...", flush=True)
    ensure_qemu(output)
    manifest = output / "module-test.json"
    fingerprint = source_hash()
    if not force:
        metadata = prepared_metadata(output)
        if metadata and metadata.get("source_hash") == fingerprint:
            print("Using prepared module and matching kernel.", flush=True)
            return

    image = bootstrap_images()
    print("Building module and matching runtime in ARM64 guest...", flush=True)
    command = base_command("2G") + [
        "-kernel", str(IMAGES / "vmlinuz-bootstrap"),
        "-initrd", str(IMAGES / "initramfs-bootstrap"),
        "-append", "console=ttyAMA0 "
        "modules=loop,squashfs,virtio_pci,virtio_blk "
        "alpine_dev=/dev/vda:iso9660 modloop=/boot/modloop-virt",
        "-drive", f"file={image},if=none,id=bootiso,format=raw,readonly=on",
        "-device", "virtio-blk-pci,drive=bootiso",
        "-device", "gpgpu,bus=pcie.0",
    ]
    command += shared_directory(SOURCE, "gpgpu_src", True)
    command += shared_directory(output, "gpgpu_build", False)
    command += ["-netdev", "user,id=net0",
                "-device", "virtio-net-pci,netdev=net0"]
    stage = Path(tempfile.mkdtemp(prefix=".prepare-", dir=output))
    try:
        guest = Guest(command, output / "prepare-console.log")
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
                "> /mnt/gpgpu-build/native-build.log 2>&1\n"
                "printf 'GPGPU_BUILD_RC=%s\\n' \"$?\"\n"
            )
            result = guest.wait(rb"^GPGPU_BUILD_RC=(\d+)$", timeout=1800)
            if result.group(1) != b"0":
                raise RuntimeError(
                    "Guest build failed; inspect native-build.log")
        finally:
            guest.close()

        data = (stage / "gpgpu_pci.ko").read_bytes()
        if (len(data) < 20 or data[:6] != b"\x7fELF\x02\x01"
                or struct.unpack_from("<H", data, 18)[0] != 183):
            raise RuntimeError("Expected a little-endian ARM64 module")
        release = (stage / "kernel-release").read_text().strip()
        vermagic = next(part.decode() for part in data.split(b"\0")
                        if part.startswith(b"vermagic="))
        if not vermagic.startswith("vermagic=" + release + " "):
            raise RuntimeError("Module and runtime kernel versions differ")
        staged_manifest = stage / manifest.name
        staged_manifest.write_text(json.dumps({
            "source_hash": fingerprint, "kernel_release": release,
            "vermagic": vermagic.removeprefix("vermagic="),
            "files": {name: sha256(stage / name) for name in RUNTIME_FILES},
        }, indent=2) + "\n")
        for name in RUNTIME_FILES + BUILD_RECORDS:
            (stage / name).replace(output / name)
        # Publish the manifest last; incomplete output fails hash validation.
        staged_manifest.replace(manifest)
    finally:
        shutil.rmtree(stage)
    print(f"Prepared {output / 'gpgpu_pci.ko'} for {release}", flush=True)


def runtime_command(output, interactive=False):
    if not prepared_metadata(output):
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
        guest.send("dmesg | grep 'gpgpu_pci:'\n")
        guest.wait(rb"gpgpu_pci: module unloaded")
    finally:
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "test", "run"))
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build" / "gpgpu-linux-module")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if args.command in ("prepare", "test"):
        prepare(output, args.force)
    if args.command == "test":
        test(output)
    elif args.command == "run":
        command = runtime_command(output, interactive=True)
        os.execv(command[0], command)


if __name__ == "__main__":
    main()
