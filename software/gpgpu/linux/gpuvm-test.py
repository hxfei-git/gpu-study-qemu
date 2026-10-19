#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""在独立 A57 客体中验证 GPUVM，复用既有内核、编译和日志核对入口。"""
import argparse
import fcntl
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import time

spec = importlib.util.spec_from_file_location("module_test", Path(__file__).with_name("module-test.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def main():
    """序列化客体磁盘访问，运行五组测试并保存实际结果。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=m.ROOT / "build/gpgpu-kernel-gpuvm")
    parser.add_argument("--iommu", choices=("none", "smmuv3"), default="none")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    with (output / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not m.environment_metadata(output):
            raise RuntimeError("先完成启用 DRM 的独立内核构建与导入")
        m.prepare(output, False, remote=True)
        m.check_iommu_kernel(output, args.iommu)
        subprocess.run(["make", "-C", str(m.SOURCE.parent / "firmware")], check=True)
        shutil.copy2(m.ROOT / "build/gpgpu-firmware/gpgpu-cp.bin", output / "gpgpu-cp.bin")
        stack = output / "gpuvm-stack"
        subprocess.run(["make", "-C", str(m.SOURCE.parent), f"BUILD_DIR={stack}", "kernels"], check=True)
        results = output / "gpuvm" / args.iommu
        results.mkdir(parents=True, exist_ok=True)
        (results / "result.json").unlink(missing_ok=True)
        remote = m.RemoteDevice(m.DEVICE_QEMU_BINARY, results, gpuvm=True)
        guest = m.Guest(m.developer_command(output, args.iommu, remote=True), results / "console.log", remote)
        try:
            guest.wait(rb"^GPGPU_DEVELOPER_READY$")
            guest.send("mkdir -p /mnt/gpgpu-stack\n"
                       "mount -t 9p -o trans=virtio,version=9p2000.L,ro gpgpu_stack /mnt/gpgpu-stack\n"
                       f"sh /mnt/gpgpu-src/guest-gpuvm-test.sh > /mnt/gpgpu-build/gpuvm/{args.iommu}/test.log 2>&1\n"
                       "printf 'GPGPU_GPUVM_RC=%s\\n' \"$?\"\n")
            result = guest.wait(rb"^GPGPU_GPUVM_RC=(\d+)$", timeout=1200)
            log = (results / "test.log").read_text(errors="replace")
            print(log, end="", flush=True)
            groups = re.findall(r"^PASS gpuvm-group-([1-5])$", log, re.M)
            if result.group(1) != b"0" or groups != list("12345") or "GPUVM unload: PASS" not in log:
                raise RuntimeError("GPUVM 验收不完整，请检查 test.log")
            domain = m.check_iommu_domain(guest, args.iommu)
            m.check_kernel_log(guest)
            m.write_metadata(results / "result.json", {
                "completed_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "groups_passed": len(groups), "iommu": domain, "transport": "vfio-user",
                "kernel_release": (output / "kernel-release").read_text().strip(),
                "module_sha256": m.sha256(output / "gpgpu_pci.ko"),
                "firmware_sha256": m.sha256(output / "gpgpu-cp.bin"),
                "test_sha256": m.sha256(stack / "test-gpuvm"),
                "kernel_faults": 0,
            })
        finally:
            guest.close()


if __name__ == "__main__":
    main()
