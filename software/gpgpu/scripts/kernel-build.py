#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""构建固定 ARM64 实验内核；默认宿主机交叉编译，保留客体原生路径。"""

import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import urllib.request

import gpgpu_linux as MODULE


MATERIALS = MODULE.SOURCE / "kernel"
# 作为构建起点的 Alpine 发行版开发环境的内核版本。
BASE_RELEASE = "6.18.57-0-virt"


checksum = MODULE.file_digest


def download_checked(path, url, expected, algorithm="sha256", timeout=120):
    """下载到 path 并核对散列；已有缓存也重新核对。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file():
        temporary = path.with_suffix(".download")
        with urllib.request.urlopen(url, timeout=timeout) as response:
            with temporary.open("wb") as stream:
                shutil.copyfileobj(response, stream)
        if checksum(temporary, algorithm) != expected:
            raise RuntimeError(f"下载校验失败：{path.name}")
        temporary.replace(path)
    if checksum(path, algorithm) != expected:
        raise RuntimeError(f"缓存校验失败：{path.name}")


def cross_environment():
    """工具链和补充依赖只解压到 build/，不修改宿主机软件包。"""
    toolchain = json.loads((MATERIALS / "cross-toolchain.json").read_text())
    cache = MODULE.ROOT / "build/gpgpu-kernel-toolchain"
    archive = cache / toolchain["archive"]
    download_checked(archive, toolchain["url"], toolchain["sha256"])
    directory = cache / toolchain["directory"]
    if not directory.is_dir():
        subprocess.run(["tar", "-xJf", str(archive), "-C", str(cache)],
                       check=True)
    prefix = str(directory / toolchain["prefix"])
    version = subprocess.check_output([prefix + "gcc", "-dumpfullversion"],
                                      text=True).strip()
    if version != toolchain["gcc_version"]:
        raise RuntimeError("交叉编译器版本与固定清单不符")
    plugin = Path(subprocess.check_output(
        [prefix + "gcc", "-print-file-name=plugin"], text=True).strip())
    if not (plugin / "include/plugin-version.h").is_file():
        raise RuntimeError("交叉工具链缺少 GCC 插件开发文件")

    dependencies = json.loads(
        (MATERIALS / "cross-host-dependencies.json").read_text())
    host_tools = MODULE.ROOT / "build/gpgpu-kernel-host-tools"
    host_root = host_tools / "root"
    host_root.mkdir(parents=True, exist_ok=True)
    for item in dependencies:
        package = host_tools / "downloads" / item["file"]
        download_checked(package, item["url"], item["sha256"])
        subprocess.run(["dpkg-deb", "-x", str(package), str(host_root)],
                       check=True)
    environment = os.environ.copy()
    for name in ("CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS"):
        environment.pop(name, None)
    environment["PATH"] = str(host_root / "usr/bin") + os.pathsep + environment["PATH"]
    library = host_root / "usr/lib/x86_64-linux-gnu"
    environment["LD_LIBRARY_PATH"] = str(library)
    environment["HOSTCFLAGS"] = "-O2 -I" + str(host_root / "usr/include")
    environment["HOSTCXXFLAGS"] = "-O2 -I" + str(host_root / "usr/include") + \
        " -I" + str(host_root / "usr/include/x86_64-linux-gnu")
    environment["HOSTLDFLAGS"] = "-L" + str(library)
    return prefix, environment, toolchain


def config_values(path):
    """将内核配置中的启用值和未启用项解析为统一字典。"""
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
    return values


def check_cross_config(config, output):
    """编译器身份可以不同；功能选项必须与已确认的原生配置一致。"""
    expected = config_values(MATERIALS / "gpgpu.arm64.config")
    actual = config_values(config)
    differences = {key: {"native": expected.get(key, "n"),
                         "cross": actual.get(key, "n")}
                   for key in sorted(expected.keys() | actual.keys())
                   if expected.get(key, "n") != actual.get(key, "n")}
    allowed = {"CONFIG_CC_VERSION_TEXT", "CONFIG_GCC_VERSION",
               "CONFIG_AS_VERSION", "CONFIG_LD_VERSION"}
    MODULE.write_metadata(output / "cross-config-differences.json", differences)
    unexpected = differences.keys() - allowed
    if unexpected:
        raise RuntimeError(f"交叉配置改变了功能选项：{sorted(unexpected)}")


def compressed_tar(command, destination, environment):
    """同时检查 tar 和 zstd 的退出状态，防止只得到部分开发文件。"""
    producer = subprocess.Popen(command, stdout=subprocess.PIPE)
    try:
        result = subprocess.run(["zstd", "-T0", "-q", "-f", "-o", str(destination)],
                                stdin=producer.stdout, env=environment)
    finally:
        producer.stdout.close()
    if producer.wait() or result.returncode:
        raise RuntimeError(f"导入归档生成失败：{destination.name}")


def run_cross_make(command, environment, log, timeout):
    """超时时终止整组 make/编译器，等待其清理不完整目标后再交还输出目录。"""
    process = subprocess.Popen(command, env=environment, stdout=log,
                               stderr=log, start_new_session=True)
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        raise RuntimeError("交叉编译超时，已停止编译进程组并保留中间产物") from None
    if result:
        raise subprocess.CalledProcessError(result, command)


def cross_compile(args, output, manifest):
    """按固定工具链及配置交叉构建内核，校验版本后导出客体开发材料。"""
    prefix, environment, toolchain = cross_environment()
    source = output / "source/linux-6.18"
    build_dir = output / "host-output"
    build_dir.mkdir(exist_ok=True)
    config = build_dir / ".config"
    if not config.exists():
        config.write_bytes((MATERIALS / "virt.aarch64.config").read_bytes()
                           + (MATERIALS / "gpgpu.config").read_bytes())
    epoch = str(manifest["source_date_epoch"])
    environment.update(SOURCE_DATE_EPOCH=epoch, KBUILD_BUILD_USER="gpgpu",
                       KBUILD_BUILD_HOST="cross-x86_64", KBUILD_BUILD_VERSION="1",
                       KBUILD_BUILD_TIMESTAMP=datetime.datetime.fromtimestamp(
                           int(epoch), datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S"))
    command = ["make", "-C", str(source), "O=" + str(build_dir), "ARCH=arm64",
               "CROSS_COMPILE=" + prefix, "HOSTCC=gcc", "HOSTCXX=g++", "AWK=mawk"]
    MODULE.write_metadata(output / "cross-build-command.json", command)
    with (output / "kernel-build.log").open("w") as log:
        run_cross_make(command + ["olddefconfig"], environment, log, args.timeout)
        check_cross_config(config, output)
        print(f"开始宿主机交叉编译：{args.jobs} 个任务，日志 {output / 'kernel-build.log'}",
              flush=True)
        run_cross_make(command + [f"-j{args.jobs}", "Image", "modules"],
                       environment, log, args.timeout)
        release = subprocess.check_output(command + ["-s", "kernelrelease"],
                                          env=environment, text=True).strip()
        if release != manifest["kernel_release"]:
            raise RuntimeError("交叉内核版本不符")
        install = output / "install"
        run_cross_make(command + ["INSTALL_MOD_PATH=" + str(install),
                                  "INSTALL_MOD_STRIP=1", "modules_install"],
                       environment, log, args.timeout)
    shutil.copy2(build_dir / "arch/arm64/boot/Image", output / "vmlinuz-virt")
    shutil.copy2(config, output / "kernel-config")
    shutil.copy2(build_dir / "Module.symvers", output / "kernel-Module.symvers")
    (output / "kernel-release").write_text(release + "\n")
    (output / "compiler-version").write_text(subprocess.check_output(
        [prefix + "gcc", "--version"], text=True))
    MODULE.write_metadata(output / "cross-build.json", {
        "toolchain": toolchain, "jobs": args.jobs,
        "source_hash": (output / "source.sha256").read_text().strip(),
        "config_sha256": checksum(config), "module_symvers_sha256": checksum(
            build_dir / "Module.symvers")})
    compressed_tar(["tar", "-C", str(install), "-cf", "-", "lib/modules/" + release],
                   output / "cross-modules.tar.zst", environment)
    compressed_tar(["tar", "-C", str(output), "--exclude=*.o", "--exclude=*.a",
                    "--exclude=*.ko", "--exclude=vmlinux*", "--exclude=Image*",
                    "--exclude=signing_key.pem",
                    "--transform=s,^source/linux-6.18,gpgpu-kernel-source,",
                    "--transform=s,^host-output,gpgpu-kernel-output,",
                    "-cf", "-", "source/linux-6.18", "host-output"],
                   output / "cross-development.tar.zst", environment)
    (output / "cross-import.sha256").write_text(
        checksum(output / "cross-development.tar.zst") + "\n")


def status(output):
    """只读查询真实 QEMU 状态和累计块设备访问，避免仅凭旧日志判断。"""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(str(output / "kernel-qmp.sock"))
        stream = connection.makefile("rwb")
        json.loads(stream.readline())

        def query(command):
            """发送一条 QMP 命令，跳过异步事件并等待返回或错误。"""
            stream.write((json.dumps({"execute": command}) + "\n").encode())
            stream.flush()
            while True:
                result = json.loads(stream.readline())
                if "return" in result:
                    return result["return"]
                if "error" in result:
                    raise RuntimeError(result["error"])

        query("qmp_capabilities")
        state = query("query-status")
        state["cpus"] = len(query("query-cpus-fast"))
        state["blocks"] = [{"device": item.get("device"),
                            "read_bytes": item["stats"]["rd_bytes"],
                            "written_bytes": item["stats"]["wr_bytes"]}
                           for item in query("query-blockstats")]
        print(json.dumps(state, ensure_ascii=False, indent=2))


def prepare_sources(output):
    """验证固定材料后，依次应用 stable、Alpine 和唯一实验补丁。"""
    manifest = json.loads((MATERIALS / "sources.json").read_text())
    if checksum(MATERIALS / "APKBUILD") != manifest["apkbuild_sha256"]:
        raise RuntimeError("Alpine APKBUILD 校验失败")
    for name, expected in manifest["sha512"].items():
        if checksum(MATERIALS / name, "sha512") != expected:
            raise RuntimeError(f"固定材料校验失败：{name}")
    downloads = output / "downloads"
    downloads.mkdir(exist_ok=True)
    for item in manifest["sources"]:
        path = downloads / item["name"]
        retained = MODULE.ROOT / "build/gpgpu-kernel/downloads" / item["name"]
        if not path.exists() and retained.is_file():
            shutil.copy2(retained, path)
        download_checked(path, item["url"], item["sha512"], "sha512", timeout=60)

    digest = hashlib.sha256()
    source_inputs = ["sources.json", "APKBUILD", "virt.aarch64.config",
                     "gpgpu.config", "0008-gpgpu-expose-buddy-config.patch"]
    for name in sorted(source_inputs + manifest["patches"]):
        path = MATERIALS / name
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    fingerprint = digest.hexdigest()
    source = output / "source"
    expanded = source / "linux-6.18"
    marker = output / "source.sha256"
    if (marker.is_file() and marker.read_text().strip() == fingerprint
            and (output / "source.tar").is_file() and expanded.is_dir()):
        return manifest
    with tempfile.TemporaryDirectory(prefix=".kernel-source-",
                                     dir=output) as temporary:
        stage = Path(temporary)
        tree = stage / expanded.name
        archive = stage / "source.tar"
        with (output / "source-prepare.log").open("w") as log:
            subprocess.run(["tar", "-xJf", str(downloads / "linux-6.18.tar.xz"),
                            "-C", str(stage)], check=True,
                           stdout=log, stderr=log)
            stable = stage / "stable.patch"
            with stable.open("wb") as stream:
                subprocess.run(["xz", "-dc", str(downloads /
                                                 "patch-6.18.57.patch.xz")],
                               stdout=stream, check=True)
            patches = [stable] + [MATERIALS / name for name in
                                  manifest["patches"]] + [MATERIALS /
                                  "0008-gpgpu-expose-buddy-config.patch"]
            for patch in patches:
                # Alpine 原补丁按默认 fuzz=2 应用。
                subprocess.run(["patch", "-p1", "--batch", "-i",
                                str(patch)], cwd=tree, check=True,
                               stdout=log, stderr=log)
            for path in tree.glob("localversion*"):
                path.unlink()
            subprocess.run(["tar", "-cf", str(archive),
                            "-C", str(stage), tree.name], check=True)
        # 新源码及归档准备成功后，才替换旧展开目录。
        if source.exists():
            shutil.rmtree(source)
        source.mkdir()
        tree.rename(expanded)
        archive.replace(output / "source.tar")
        (output / "source-date-epoch").write_text(
            str(manifest["source_date_epoch"]) + "\n")
        marker.write_text(fingerprint + "\n")
    return manifest


def build(args, output):
    """校验固定材料并选择交叉或客体原生构建，保存可复核的产物与日志。"""
    base = args.base_environment.resolve()
    if output == base:
        raise RuntimeError("构建目录必须与原发行版环境分开")
    base_metadata = MODULE.environment_metadata(base)
    if not base_metadata:
        raise RuntimeError(f"缺少 Alpine {BASE_RELEASE} 发行版开发环境：{base}")
    if base_metadata["kernel_release"] != BASE_RELEASE:
        raise RuntimeError(f"本配方要求保留的 Alpine {BASE_RELEASE} 环境")
    packages = json.loads((MATERIALS / "cached-packages.json").read_text())
    for name, record in packages.items():
        if checksum(base / "apk-cache" / name) != record["sha256"]:
            raise RuntimeError(f"发行版缓存包校验失败：{name}")
    manifest = prepare_sources(output)
    if args.prepare_only:
        return
    if args.build_mode == "cross":
        cross_compile(args, output, manifest)
    disk = output / "developer.raw"
    if not disk.exists():
        with (base / ".build.lock").open("a") as base_lock:
            fcntl.flock(base_lock, fcntl.LOCK_EX)
            subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                            str(base / "developer.raw"), str(disk)], check=True)
        with disk.open("r+b") as stream:
            stream.truncate(args.disk_gib * 1024 ** 3)
    elif disk.stat().st_size != args.disk_gib * 1024 ** 3:
        raise RuntimeError("已有构建盘容量不符；用原容量续建或另选输出目录")
    # 文件容量不代表文件系统容量；续建盘也可能只做过 truncate。
    result = subprocess.run(["e2fsck", "-pf", str(disk)])
    if result.returncode not in (0, 1):
        raise RuntimeError("构建盘文件系统检查失败")
    subprocess.run(["resize2fs", str(disk)], check=True)
    cache = output / "apk-cache"
    if not cache.exists():
        shutil.copytree(base / "apk-cache", cache)
    dependencies = json.loads((MATERIALS / "build-dependencies.json").read_text())
    for item in dependencies:
        download_checked(cache / item["cache_name"], item["url"], item["sha256"], timeout=60)
    (output / "build-dependencies").write_text(
        "".join(item["cache_name"] + "\n" for item in dependencies))
    command = MODULE.base_command(args.memory)
    command[command.index("-smp") + 1] = str(args.cpus)
    qmp = output / "kernel-qmp.sock"
    qmp.unlink(missing_ok=True)
    command += ["-qmp", f"unix:{qmp},server=on,wait=off"]
    # 续建也使用发行版内核启动，它仍有原盘中的匹配模块。
    command += ["-kernel", str(base / "vmlinuz-virt"),
                "-initrd", str(base / "initramfs-developer"),
                "-append", "console=ttyAMA0",
                "-drive", f"file={disk},if=none,id=developer,format=raw",
                "-device", "virtio-blk-pci,drive=developer", "-nic", "none"]
    command += MODULE.shared_directory(MODULE.GUEST_SCRIPTS, "gpgpu_src", True)
    command += MODULE.shared_directory(MODULE.STACK, "gpgpu_stack", True)
    command += MODULE.shared_directory(output, "gpgpu_build", False)
    MODULE.write_metadata(output / "build-command.json", command)
    print(f"启动 ARM64 构建客体：{args.cpus} CPU / {args.memory} / "
          f"{args.disk_gib} GiB，日志 {output / 'kernel-build.log'}", flush=True)
    guest = MODULE.Guest(command, output / "kernel-console.log")
    try:
        guest.wait(rb"^GPGPU_DEVELOPER_READY$")
        script = "guest-cross.sh" if args.build_mode == "cross" else "native-build.sh"
        guest_log = "kernel-export.log" if args.build_mode == "cross" else "kernel-build.log"
        # 旧开发盘的 init 继续挂载脚本与产物；固定内核材料由软件栈共享提供。
        guest.send("mkdir -p /mnt/gpgpu-stack\n"
                   "mount -t 9p -o trans=virtio,version=9p2000.L,ro "
                   "gpgpu_stack /mnt/gpgpu-stack\n"
                   "sh /mnt/gpgpu-src/kernel/" + script + " "
                   + shlex.quote(str(args.cpus))
                   + " > /mnt/gpgpu-build/" + guest_log + " 2>&1\n"
                   "printf 'GPGPU_KERNEL_RC=%s\\n' \"$?\"\n")
        result = guest.wait(rb"^GPGPU_KERNEL_RC=(\d+)$", args.timeout)
        if result.group(1) != b"0":
            raise RuntimeError("内核构建失败，请检查 kernel-build.log")
    finally:
        guest.close()
    release = (output / "kernel-release").read_text().strip()
    if release != manifest["kernel_release"]:
        raise RuntimeError("内核产物版本不符")
    config = (output / "kernel-config").read_text().splitlines()
    for option in ("CONFIG_GPU_BUDDY=y", "CONFIG_FW_LOADER=y",
                   "CONFIG_ARM_SMMU_V3=y", "CONFIG_IOMMU_DMA=y"):
        if option not in config:
            raise RuntimeError(f"构建配置缺少 {option}")
    environment = {
        "source_hash": MODULE.environment_hash(),
        "kernel_source_hash": (output / "source.sha256").read_text().strip(),
        "kernel_release": release,
        "disk_size": disk.stat().st_size,
        "files": {name: checksum(output / name) for name in
                  MODULE.ENVIRONMENT_FILES},
    }
    MODULE.write_metadata(output / "environment.json", environment)
    MODULE.write_metadata(output / "kernel-build.json", {
        "kernel_release": release, "alpine_commit": manifest["alpine_commit"],
        "gpu_buddy": "y", "fw_loader": "y", "target_arch": "aarch64",
        "build_mode": args.build_mode,
        "jobs": args.jobs if args.build_mode == "cross" else args.cpus,
        "cpus": args.cpus,
        "memory": args.memory, "disk_size": disk.stat().st_size,
        "source_hash": environment["kernel_source_hash"],
        "files": {name: checksum(output / name) for name in
                  ("vmlinuz-virt", "kernel-config", "kernel-Module.symvers",
                   "build-packages", "compiler-version",
                   "kernel-modules.tar.zst", "kernel-development.tar.zst")},
    })
    if not args.skip_module:
        MODULE.compile_module(output, MODULE.source_hash(), environment)
    print(f"已保存独立内核环境：{output}", flush=True)


def main():
    """解析构建或状态查询参数，持文件锁避免同时操作同一输出目录。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-environment", type=Path,
                        default=MODULE.ROOT / "build/gpgpu-linux-module")
    parser.add_argument("--build-mode", choices=("cross", "native"), default="cross")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=16,
                        help="宿主机交叉编译并发数，默认 16")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--disk-gib", type=int, default=32)
    parser.add_argument("--timeout", type=int, default=43200,
                        help="各构建阶段超时秒数，默认 12 小时")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-module", action="store_true")
    parser.add_argument("--status", action="store_true",
                        help="通过本机 QMP 查询运行中的构建客体，不修改状态")
    args = parser.parse_args()
    if args.cpus < 1 or args.jobs < 1 or args.disk_gib < 8 or args.timeout < 1:
        parser.error("CPU 和超时必须为正，构建盘至少为 8 GiB")
    default = "gpgpu-kernel-cross" if args.build_mode == "cross" else "gpgpu-kernel"
    output = (args.output_dir or MODULE.ROOT / "build" / default).resolve()
    if args.status:
        status(output)
        return
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        build(args, output)


if __name__ == "__main__":
    main()
