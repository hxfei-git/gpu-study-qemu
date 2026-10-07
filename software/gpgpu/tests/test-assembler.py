#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encoding regressions use independently specified RV32 instruction words."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
MODULE_PATH = Path(__file__).resolve().parents[1] / "kernels" / "assemble.py"
SPEC = importlib.util.spec_from_file_location("assembler", MODULE_PATH)
ASSEMBLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSEMBLER)


class EncodingTests(unittest.TestCase):
    def test_standard_encodings(self):
        examples = {
            "addi a0, zero, -1": 0xfff00513,
            "lui t0, 0x80000": 0x800002b7,
            "slli t1, t1, 2": 0x00231313,
            "mul t1, t1, t2": 0x02730333,
            "lw t2, 12(a0)": 0x00c52383,
            "sw t3, 0(t2)": 0x01c3a023,
            "flw f1, 0(t2)": 0x0003a087,
            "fsw f2, 0(t2)": 0x0023a027,
            "fadd.s f2, f0, f1": 0x00100153,
            "fmul.s f3, f1, f2": 0x102081d3,
            "bgeu t1, t2, target": 0x00737463,
            "jal zero, target": 0x0080006f,
            "ebreak": 0x00100073,
        }
        for line, expected in examples.items():
            with self.subTest(line=line):
                self.assertEqual(ASSEMBLER.encode(line, 0, {"target": 8}),
                                 expected)

    def test_negative_immediate_and_branch(self):
        self.assertEqual(ASSEMBLER.encode("jal zero, loop", 4, {"loop": 0}),
                         0xffdff06f)
        self.assertEqual(ASSEMBLER.encode("beq zero, zero, loop", 4,
                                         {"loop": 0}), 0xfe000ee3)

    def test_reject_invalid_instructions(self):
        for line in ("addi x32, x0, 0", "addi x1, x0, 2048",
                     "slli x1, x0, 32", "flw x1, 0(x0)",
                     "jal x0, 2", "li a0, 3", "lui x0, -1",
                     "lw x1, 0x80000000(x0)", "addi a0, zero"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                ASSEMBLER.encode(line, 0, {})

    def test_labels_and_little_endian(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.S"
            path.write_text("start:\naddi a0, zero, 7\njal zero, start\n")
            self.assertEqual(ASSEMBLER.assemble(path),
                             struct.pack("<II", 0x00700513, 0xffdff06f))
            path.write_text("start:\nstart:\nebreak\n")
            with self.assertRaises(ValueError):
                ASSEMBLER.assemble(path)


if __name__ == "__main__":
    unittest.main()
