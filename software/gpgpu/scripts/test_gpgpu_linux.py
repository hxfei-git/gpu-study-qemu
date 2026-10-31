#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""验证独立 QEMU 配置入口与 Linux 运行入口共用配置参数。"""
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import ANY, patch

import gpgpu_linux as linux


class QemuConfigureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / "qemu build"
        self.output = self.root / "guest-output"

    def test_configure_does_not_require_kernel(self):
        with (patch("sys.argv", ["gpgpu_linux.py", "configure",
                                 "--qemu-dir", str(self.build),
                                 "--output-dir", str(self.output)]),
              patch.object(linux, "configure_qemu") as configure,
              patch.object(linux, "environment_metadata") as environment,
              patch.object(linux.fcntl, "flock") as lock):
            linux.main()
            configure.assert_called_once_with(self.build.resolve())
            environment.assert_not_called()
            lock.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_configure_defaults_to_qemu_directory(self):
        with (patch("sys.argv", ["gpgpu_linux.py", "configure"]),
              patch.object(linux, "QEMU_DIRECTORY", self.build),
              patch.object(linux, "configure_qemu") as configure):
            linux.main()
            configure.assert_called_once_with(self.build.resolve())

    def test_configure_uses_shared_parameters(self):
        log = io.StringIO()
        with patch.object(linux.subprocess, "run") as run:
            linux.configure_qemu(self.build, log)
            run.assert_called_once_with(
                [str(linux.ROOT / "configure"), *linux.QEMU_CONFIGURE_ARGS],
                cwd=self.build, stdout=log, stderr=log, check=True)
        self.assertTrue(self.build.is_dir())

    def test_configure_propagates_failure(self):
        error = subprocess.CalledProcessError(1, "configure")
        with patch.object(linux.subprocess, "run", side_effect=error):
            with self.assertRaises(subprocess.CalledProcessError):
                linux.configure_qemu(self.build)

    def test_ensure_reuses_configure(self):
        self.output.mkdir()
        with (patch.object(linux, "QEMU_DIRECTORY", self.build),
              patch.object(linux, "qemu_configured", return_value=False),
              patch.object(linux, "configure_qemu") as configure,
              patch.object(linux.subprocess, "run") as run):
            linux.ensure_qemu(self.output)
            configure.assert_called_once_with(self.build, ANY)
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args_list[0].args[0],
                             ["ninja", "-j4", "qemu-system-aarch64",
                              "qemu-system-riscv64"])

    def test_ensure_keeps_existing_configuration(self):
        self.output.mkdir()
        with (patch.object(linux, "QEMU_DIRECTORY", self.build),
              patch.object(linux, "qemu_configured", return_value=True),
              patch.object(linux, "configure_qemu") as configure,
              patch.object(linux.subprocess, "run") as run):
            linux.ensure_qemu(self.output)
            configure.assert_not_called()
            self.assertEqual(run.call_count, 2)

    def test_qemu_directory_is_rejected_for_guest_commands(self):
        with (patch("sys.argv", ["gpgpu_linux.py", "module",
                                 "--qemu-dir", str(self.build)]),
              patch.object(linux, "environment_metadata") as environment,
              contextlib.redirect_stderr(io.StringIO()),
              self.assertRaises(SystemExit) as error):
            linux.main()
        self.assertEqual(error.exception.code, 2)
        environment.assert_not_called()


if __name__ == "__main__":
    unittest.main()
