#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""在独立测试模块中注入未知结果，检查退出后的 VMID 和存储引用仍被保留。"""
import argparse
import fcntl
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess

spec = importlib.util.spec_from_file_location("gpuvm_test", Path(__file__).with_name("gpuvm-test.py"))
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)
m = vm.m


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=m.ROOT / "build/gpgpu-kernel-gpuvm")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    with (output / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not m.environment_metadata(output):
            raise RuntimeError("先完成启用 DRM 的独立内核构建与导入")
        m.prepare(output, False, remote=True)
        subprocess.run(["make", "-C", str(m.SOURCE.parent / "firmware")], check=True)
        shutil.copy2(m.ROOT / "build/gpgpu-firmware/gpgpu-cp.bin", output / "gpgpu-cp.bin")
        results = output / "gpuvm" / "retention"
        results.mkdir(parents=True, exist_ok=True)
        (results / "result.json").unlink(missing_ok=True)
        remote = m.RemoteDevice(m.DEVICE_QEMU_BINARY, results, gpuvm=True)
        guest = m.Guest(m.developer_command(output, "none", remote=True), results / "console.log", remote)
        try:
            guest.wait(rb"^GPGPU_DEVELOPER_READY$")
            guest.send("mkdir -p /mnt/gpgpu-stack\n"
                       "mount -t 9p -o trans=virtio,version=9p2000.L,ro gpgpu_stack /mnt/gpgpu-stack\n"
                       "make -C /mnt/gpgpu-stack BUILD_DIR=/mnt/gpgpu-build/gpuvm-stack all > /mnt/gpgpu-build/gpuvm/retention/build.log 2>&1 &&\n"
                       "sh /mnt/gpgpu-src/guest-gpuvm-fault-test.sh > /mnt/gpgpu-build/gpuvm/retention/test.log 2>&1\n"
                       "printf 'GPGPU_RETAIN_RC=%s\\n' \"$?\"\n")
            result = guest.wait(rb"^GPGPU_RETAIN_RC=(\d+)$", timeout=1200)
            log = (results / "test.log").read_text(errors="replace")
            print(log, end="", flush=True)
            groups = re.findall(r"^PASS gpuvm-retain-(map|unmap|invalidate)$", log, re.M)
            if result.group(1) != b"0" or groups != ["map", "unmap", "invalidate"]:
                raise RuntimeError("未知结果保留测试失败")
            m.check_kernel_log(guest)
            m.write_metadata(results / "result.json", {"tests_passed": groups, "kernel_faults": 0,
                "test_sha256": m.sha256(output / "gpuvm-stack/test-gpuvm"),
                "module_sha256": m.sha256(output / "gpgpu_pci.ko"),
                "fault_modules": {name: m.sha256(results / f"fault-{name}.ko") for name in groups}})
        finally:
            guest.close()


if __name__ == "__main__":
    main()
