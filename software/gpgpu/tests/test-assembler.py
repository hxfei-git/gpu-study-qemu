#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""使用独立指定的 RV32 指令字验证汇编编码。"""
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
    """用独立指令字和错误案例检查教学汇编器。"""
    def test_standard_encodings(self):
        """将各类教学指令与预先指定的 RV32 指令字逐项比较。"""
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
        """核对向后跳转及分支的有符号位段拼装。"""
        self.assertEqual(ASSEMBLER.encode("jal zero, loop", 4, {"loop": 0}),
                         0xffdff06f)
        self.assertEqual(ASSEMBLER.encode("beq zero, zero, loop", 4,
                                         {"loop": 0}), 0xfe000ee3)

    def test_reject_invalid_instructions(self):
        """确认非法寄存器、立即数、对齐与未支持伪指令被拒绝。"""
        for line in ("addi x32, x0, 0", "addi x1, x0, 2048",
                     "slli x1, x0, 32", "flw x1, 0(x0)",
                     "jal x0, 2", "li a0, 3", "lui x0, -1",
                     "lw x1, 0x80000000(x0)", "addi a0, zero"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                ASSEMBLER.encode(line, 0, {})

    def test_labels_and_little_endian(self):
        """用临时源码核对标签回填、小端输出和重复标签错误。"""
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
