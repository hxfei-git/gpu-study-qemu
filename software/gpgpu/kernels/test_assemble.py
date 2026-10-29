#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""RV32 教学汇编器的编码回归。

用 GNU 汇编器给出的标准编码核对每类指令，并检查 CSR 和寄存器的非法输入。
运行：python3 software/gpgpu/kernels/test_assemble.py
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import assemble  # noqa: E402


def encode(line, pc=0, symbols=None):
    """编码单条指令，默认符号表为空。"""
    return assemble.encode(line, pc, symbols or {})


class EncodeTest(unittest.TestCase):
    """期望值来自 riscv64-unknown-elf-as -march=rv32imf，浮点舍入位除外。"""

    def test_integer(self):
        self.assertEqual(encode("addi t0, zero, 5"), 0x00500293)
        self.assertEqual(encode("andi a1, a2, -1"), 0xFFF67593)
        self.assertEqual(encode("slli t1, t1, 2"), 0x00231313)
        self.assertEqual(encode("add t1, t1, t2"), 0x00730333)
        self.assertEqual(encode("sub t1, t1, t2"), 0x40730333)
        self.assertEqual(encode("mul t1, t1, t2"), 0x02730333)
        self.assertEqual(encode("lui t0, 0x12345"), 0x123452B7)

    def test_memory(self):
        self.assertEqual(encode("lw t0, 0(a0)"), 0x00052283)
        self.assertEqual(encode("sw t1, 0(t0)"), 0x0062A023)
        self.assertEqual(encode("flw f0, 0(t2)"), 0x0003A007)
        self.assertEqual(encode("fsw f2, 0(t2)"), 0x0023A027)

    def test_float(self):
        # GNU as 默认写动态舍入 rm=7；本汇编器固定写 RNE（rm=0）。
        # 执行器复位后 frm 为 0，两种编码的计算结果相同。
        self.assertEqual(encode("fadd.s f2, f0, f1"), 0x00100153)
        self.assertEqual(encode("fmul.s f2, f0, f1"), 0x10100153)

    def test_control(self):
        self.assertEqual(encode("ebreak"), 0x00100073)
        self.assertEqual(encode("bgeu t1, t2, done", 0, {"done": 8}), 0x00737463)
        self.assertEqual(encode("jal zero, back", 8, {"back": 0}), 0xFF9FF06F)

    def test_csr(self):
        self.assertEqual(encode("csrr t1, 0xCC3"), 0xCC302373)
        self.assertEqual(encode("csrr t1, BLOCK_ID_X", 0, {"BLOCK_ID_X": 0xCC3}), 0xCC302373)
        with self.assertRaises(ValueError):
            encode("csrr t1, 0x1000")

    def test_operands(self):
        with self.assertRaises(ValueError):
            encode("add t1, t1")
        with self.assertRaises(ValueError):
            encode("add t1, t1, x32")
        with self.assertRaises(ValueError):
            encode("lw t0, 2048(a0)")


class KernelTest(unittest.TestCase):
    """仓库中的三个 Kernel 都能完整汇编，且以 ebreak 结束。"""

    def test_kernels(self):
        kernels = Path(__file__).resolve().parent
        for name in ("vector_add", "matmul", "relu"):
            code = assemble.assemble(kernels / f"{name}.S")
            self.assertEqual(len(code) % 4, 0, name)
            self.assertEqual(code[-4:], (0x00100073).to_bytes(4, "little"), name)


if __name__ == "__main__":
    unittest.main()
