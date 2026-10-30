#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""从固定 ISO 和 APK 构建 Alpine 基础开发盘。

供 kernel-build.py 使用；宿主机校验下载，客体离线安装。
保留 APK 签名检查。失败时保留暂存目录，
重启验证成功后才发布 environment.json。
"""
import argparse
import fcntl
import importlib.util
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import tempfile
import urllib.error

import gpgpu_linux as module

_spec = importlib.util.spec_from_file_location(
    "kernel_build", Path(__file__).with_name("kernel-build.py"))
kernel = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kernel)
ISO_NAME = "alpine-virt-3.24.2-aarch64.iso"
ISO_SHA256 = "a57ba668b5f6b17a670fcf8e799d5d7fe43766ed086d6ce2927b0625bf43dbf6"
REPOSITORY = "https://dl-cdn.alpinelinux.org/alpine/v3.24/"
ISO_URL = REPOSITORY + "releases/aarch64/" + ISO_NAME
MATERIALS = module.SOURCE / "kernel"


def inputs(output):
    """给出 URL、校验值与路径；内核沿用已有清单。"""
    result = [(output / "downloads" / ISO_NAME, ISO_URL, ISO_SHA256)]
    manifest = json.loads((MATERIALS / "bootstrap-packages.json").read_text())
    for item in manifest["packages"]:
        result.append((output / "apk-cache" / item["file"],
                       item["url"], item["sha256"]))
    cached = json.loads((MATERIALS / "cached-packages.json").read_text())
    for name, item in cached.items():
        # 缓存名带 apk 缓存后缀，镜像使用原始版本名。
        filename = re.sub(r"\.[0-9a-f]{8}\.apk$", ".apk", name)
        url = REPOSITORY + "main/aarch64/" + filename
        result.append((output / "apk-cache" / name, url, item["sha256"]))
    return result


def prepare_inputs(output, offline=False):
    """核对全部 SHA-256；失败时列出缺项，不替换版本。"""
    missing = []
    for path, url, digest in inputs(output):
        if offline and not path.is_file():
            missing.append({"path": str(path), "url": url, "sha256": digest,
                            "reason": "离线缓存缺失"})
            continue
        try:
            kernel.download_checked(path, url, digest, timeout=30)
        except (OSError, urllib.error.URLError, RuntimeError) as exc:
            missing.append({"path": str(path), "url": url, "sha256": digest,
                            "reason": str(exc)})
    module.write_metadata(output / "missing-inputs.json", missing)
    if missing:
        for item in missing:
            print(json.dumps(item, ensure_ascii=False), flush=True)
        raise RuntimeError("请按 missing-inputs.json 补齐同版原文件")
    return output / "downloads" / ISO_NAME


def extract_images(image, images):
    """从已验证 ISO 的目录记录提取启动内核与 initramfs。"""
    images.mkdir(parents=True, exist_ok=True)
    with image.open("rb") as stream:
        stream.seek(16 * 2048)
        descriptor = stream.read(2048)
        if descriptor[:7] != b"\x01CD001\x01":
            raise RuntimeError("缺少 ISO9660 主卷描述符")

        def entries(record):
            """读取 ISO9660 子项，按文件名索引原始记录。"""
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
                raise RuntimeError("引导 ISO 内容被截断")
            (images / filename).write_bytes(data)
    return image


def bootstrap(output, offline=False, disk_gib=8):
    """校验材料、离线安装、重启验证，再发布环境清单。"""
    existing = module.environment_metadata(output)
    if existing:
        if existing["kernel_release"] != kernel.BASE_RELEASE:
            raise RuntimeError("已有环境版本不同，请另选目录")
        print("复用已校验基础环境：" + str(output))
        return
    if (output / "developer.raw").exists():
        raise RuntimeError("developer.raw 未通过校验，拒绝覆盖；"
                           "请另选输出目录")
    image = prepare_inputs(output, offline)
    images = output / "downloads"
    extract_images(image, images)
    module.ensure_qemu(output)
    stage = Path(tempfile.mkdtemp(prefix=".bootstrap-", dir=output))
    disk_size = disk_gib * 1024 ** 3
    with (stage / "developer.raw").open("wb") as stream:
        stream.truncate(disk_size)
    packages = [path.name for path, _, _ in inputs(output)
                if path.suffix == ".apk"]
    (stage / "bootstrap-packages").write_text(
        "".join(name + "\n" for name in packages))
    command = module.base_command("2G") + [
        "-kernel", str(images / "vmlinuz-bootstrap"),
        "-initrd", str(images / "initramfs-bootstrap"),
        "-append", "console=ttyAMA0 "
        "modules=loop,squashfs,virtio_pci,virtio_blk "
        "alpine_dev=/dev/vda:iso9660 modloop=/boot/modloop-virt",
        "-drive", f"file={image},if=none,id=bootiso,format=raw,readonly=on",
        "-device", "virtio-blk-pci,drive=bootiso",
        "-drive", f"file={stage / 'developer.raw'},"
        "if=none,id=developer,format=raw",
        "-device", "virtio-blk-pci,drive=developer", "-nic", "none"]
    command += module.shared_directory(module.GUEST_SCRIPTS, "gpgpu_src", True)
    command += module.shared_directory(output, "gpgpu_build", False)
    print("基础环境暂存目录：" + str(stage), flush=True)
    guest = module.Guest(command, output / "bootstrap-console.log")
    try:
        guest.wait(rb"localhost login:", timeout=300)
        guest.send("root\n")
        guest.wait(rb"localhost:~# ")
        guest.send(
            "mkdir -p /mnt/gpgpu-src /mnt/gpgpu-build\n"
            "modprobe 9pnet_virtio\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L,ro gpgpu_src /mnt/gpgpu-src\n"
            "mount -t 9p -o trans=virtio,version=9p2000.L gpgpu_build /mnt/gpgpu-build\n"
            "sh /mnt/gpgpu-src/bootstrap-env.sh " + shlex.quote(stage.name) +
            " > /mnt/gpgpu-build/bootstrap-install.log 2>&1\n"
            "printf 'GPGPU_BOOTSTRAP_RC=%s\\n' \"$?\"\n")
        result = guest.wait(rb"^GPGPU_BOOTSTRAP_RC=(\d+)$", timeout=1800)
        if result.group(1) != b"0":
            raise RuntimeError("离线安装失败，见 bootstrap-install.log")
    finally:
        guest.close()
    release = (stage / "kernel-release").read_text().strip()
    if release != kernel.BASE_RELEASE:
        raise RuntimeError("引导产物内核版本不符")
    guest = module.Guest(module.developer_command(stage),
                         output / "bootstrap-verify.log")
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$", timeout=300)
        guest.send("printf 'BOOTSTRAP_RELEASE=%s\\n' \"$(uname -r)\"\n")
        guest.wait(rb"^BOOTSTRAP_RELEASE=6\.18\.57-0-virt$")
        module.check_kernel_log(guest)
    finally:
        guest.close()
    metadata = {
        "kernel_release": release, "disk_size": disk_size,
        "source_hash": module.environment_hash(),
        "bootstrap_lock_sha256": module.sha256(
            MATERIALS / "bootstrap-packages.json"),
        "files": {name: module.sha256(stage / name)
                  for name in module.ENVIRONMENT_FILES},
    }
    for name in module.ENVIRONMENT_FILES + ("developer.raw",):
        (stage / name).replace(output / name)
    module.write_metadata(output / "environment.json", metadata)
    shutil.rmtree(stage)
    print("基础环境已重启验证：" + release, flush=True)


def main():
    """查询材料或持锁引导；不构建旧驱动、旧同步栈。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=module.ROOT / "build/gpgpu-linux-module")
    parser.add_argument("--offline", action="store_true",
                        help="只接受散列匹配的本地缓存")
    parser.add_argument("--list-inputs", action="store_true",
                        help="列出文件、URL 和 SHA-256")
    parser.add_argument("--disk-gib", type=int, default=8)
    args = parser.parse_args()
    if args.disk_gib < 8:
        parser.error("开发盘至少为 8 GiB")
    output = args.output_dir.resolve()
    if args.list_inputs:
        records = [{"path": str(p), "url": u, "sha256": h}
                   for p, u, h in inputs(output)]
        print(json.dumps(records, ensure_ascii=False, indent=2))
        return
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        bootstrap(output, args.offline, args.disk_gib)


if __name__ == "__main__":
    main()
