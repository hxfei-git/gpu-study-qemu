#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""验证缓存校验、离线缺件报告与已有磁盘保护。"""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "bootstrap_env", Path(__file__).with_name("bootstrap-env.py"))
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.output = Path(self.tmp.name)
        self.package = self.output / "pinned.apk"
        self.digest = hashlib.sha256(b"pinned payload").hexdigest()
        self.input_patch = patch.object(bootstrap, "inputs", return_value=[
            (self.package, "https://example.invalid/pinned.apk", self.digest)])
        self.input_patch.start()
        self.addCleanup(self.input_patch.stop)

    def test_missing_offline_never_downloads(self):
        with patch.object(bootstrap.kernel, "download_checked") as download:
            with (contextlib.redirect_stdout(io.StringIO()),
                  self.assertRaises(RuntimeError)):
                bootstrap.prepare_inputs(self.output, offline=True)
            download.assert_not_called()
        missing = json.loads((self.output / "missing-inputs.json").read_text())
        self.assertEqual(missing[0]["path"], str(self.package))
        self.assertEqual(missing[0]["sha256"], self.digest)

    def test_corrupt_cache_is_rejected(self):
        self.package.write_bytes(b"wrong version")
        with (contextlib.redirect_stdout(io.StringIO()),
              self.assertRaises(RuntimeError)):
            bootstrap.prepare_inputs(self.output, offline=True)
        self.assertFalse((self.output / "environment.json").exists())
        self.assertEqual(self.package.read_bytes(), b"wrong version")

    def test_valid_cache_needs_no_network(self):
        self.package.write_bytes(b"pinned payload")
        with patch.object(bootstrap.kernel.urllib.request,
                          "urlopen") as network:
            bootstrap.prepare_inputs(self.output, offline=True)
            network.assert_not_called()
        missing = json.loads((self.output / "missing-inputs.json").read_text())
        self.assertEqual(missing, [])

    def test_existing_unverified_disk_is_preserved(self):
        disk = self.output / "developer.raw"
        disk.write_bytes(b"existing disk")
        with patch.object(bootstrap, "prepare_inputs") as prepare:
            with self.assertRaisesRegex(RuntimeError, "拒绝覆盖"):
                bootstrap.bootstrap(self.output)
            prepare.assert_not_called()
        self.assertEqual(disk.read_bytes(), b"existing disk")

    def test_reuse_rejects_wrong_kernel(self):
        with patch.object(bootstrap.module, "environment_metadata",
                          return_value={"kernel_release": "other"}):
            with self.assertRaisesRegex(RuntimeError, "版本不同"):
                bootstrap.bootstrap(self.output)

    def test_reuse_does_not_rebuild(self):
        environment = {"kernel_release": bootstrap.kernel.BASE_RELEASE}
        with patch.object(bootstrap.module, "environment_metadata",
                          return_value=environment):
            with patch.object(bootstrap, "prepare_inputs") as prepare:
                with contextlib.redirect_stdout(io.StringIO()):
                    bootstrap.bootstrap(self.output)
                prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
