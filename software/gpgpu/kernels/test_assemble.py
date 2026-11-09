#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""RV64 教学汇编器的编码回归。

标准指令核对 GNU 汇编器编码，自定义张量指令核对共享 ABI 的 R4 契约；
检查 CSR、寄存器和操作数个数的非法输入。
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
    """标准指令核对 GNU as 编码；自定义张量指令核对通用 ABI 的 R4 位段。"""

    def test_integer(self):
        self.assertEqual(encode("addi t0, zero, 5"), 0x00500293)
        self.assertEqual(encode("andi a1, a2, -1"), 0xFFF67593)
        self.assertEqual(encode("slli t1, t1, 2"), 0x00231313)
        self.assertEqual(encode("slli t1, t1, 33"), 0x02131313)
        self.assertEqual(encode("add t1, t1, t2"), 0x00730333)
        self.assertEqual(encode("sub t1, t1, t2"), 0x40730333)
        self.assertEqual(encode("mul t1, t1, t2"), 0x02730333)
        self.assertEqual(encode("lui t0, 0x12345"), 0x123452B7)
        for line, word in (
            ("ori a1, a2, -1", 0xFFF66593),
            ("xori a1, a2, -1", 0xFFF64593),
            ("srli t1, t1, 33", 0x02135313),
            ("srai t1, t1, 63", 0x43F35313),
            ("and t1, t1, t2", 0x00737333),
            ("or t1, t1, t2", 0x00736333),
            ("xor t1, t1, t2", 0x00734333),
            ("divu t1, t1, t2", 0x02735333),
            ("remu t1, t1, t2", 0x02737333),
            ("auipc t0, 0x12345", 0x12345297),
        ):
            with self.subTest(line=line):
                self.assertEqual(encode(line), word)

    def test_memory(self):
        self.assertEqual(encode("lb t0, -1(a0)"), 0xFFF50283)
        self.assertEqual(encode("lbu t0, 3(a0)"), 0x00354283)
        self.assertEqual(encode("sb t1, -1(t0)"), 0xFE628FA3)
        self.assertEqual(encode("lw t0, 0(a0)"), 0x00052283)
        self.assertEqual(encode("ld t2, 24(a0)"), 0x01853383)
        self.assertEqual(encode("sd t1, 8(t0)"), 0x0062B423)
        self.assertEqual(encode("sw t1, 0(t0)"), 0x0062A023)
        self.assertEqual(encode("flw f0, 0(t2)"), 0x0003A007)
        self.assertEqual(encode("fsw f2, 0(t2)"), 0x0023A027)

    def test_float(self):
        # 省略 rm 保持 RNE；转换的 RTZ 与显式动态舍入分别检查编码。
        for line, word in (
            ("fadd.s f2, f0, f1", 0x00100153),
            ("fsub.s f2, f0, f1", 0x08100153),
            ("fmul.s f2, f0, f1", 0x10100153),
            ("fdiv.s f2, f0, f1", 0x18100153),
            ("fsqrt.s f2, f0", 0x58000153),
            ("fmin.s f2, f0, f1", 0x28100153),
            ("fmax.s f2, f0, f1", 0x28101153),
            ("feq.s t0, f0, f1", 0xA01022D3),
            ("flt.s t0, f0, f1", 0xA01012D3),
            ("fle.s t0, f0, f1", 0xA01002D3),
            ("fmv.x.w t0, f1", 0xE00082D3),
            ("fmv.w.x f2, t0", 0xF0028153),
            ("fcvt.s.w f2, t0", 0xD0028153),
            ("fcvt.w.s t0, f2, rtz", 0xC00112D3),
            ("fmadd.s f2, f0, f1, f3", 0x18100143),
            ("fmadd.s f31, f30, f29, f28, dyn", 0xE1DF7FC3),
        ):
            with self.subTest(line=line):
                self.assertEqual(encode(line), word)

    def test_tensor(self):
        abi = assemble.tensor_constants()
        for op, kind, fmt in (
            ("tmat.f32", "GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_LINEAR_F32"),
            ("tmat.w8", "GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_LINEAR_W8"),
            ("tmat.bmm", "GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_BMM"),
            ("tmat.conv", "GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_CONV"),
            ("tvec.copy", "GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_COPY"),
            ("tvec.concat", "GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_CONCAT"),
            ("tvec.nearest", "GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_NEAREST"),
        ):
            matrix = op.startswith("tmat.")
            operands = "a0, s0, t6" if matrix else "a0, s0"
            rs3 = 31 if matrix else 0
            word = (rs3 << 27 | abi[fmt] << 25 | 8 << 20 | 10 << 15 |
                    abi[kind] << 12 | abi["GPGPU_TENSOR_OPCODE"])
            with self.subTest(op=op):
                self.assertEqual(encode(f"{op} {operands}"), word)
        self.assertEqual(encode("tmat.f32 x10, x8, x31"),
                         encode("tmat.f32 a0, s0, t6"))

    def test_control(self):
        self.assertEqual(encode("ebreak"), 0x00100073)
        self.assertEqual(encode("bgeu t1, t2, done", 0, {"done": 8}), 0x00737463)
        self.assertEqual(encode("jal zero, back", 8, {"back": 0}), 0xFF9FF06F)
        self.assertEqual(encode("jalr ra, -4(t0)"), 0xFFC280E7)
        self.assertEqual(encode("jalr ra, t0, -4"), 0xFFC280E7)

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
        for line in (
            "srli t1, t1, 64",
            "fadd.s f2, f0, f1, 5",
            "fmin.s f2, f0, f1, rne",
            "fcvt.w.s f0, f1",
            "fmadd.s f0, f1, f2, f32",
            "tmat.f32 a0, s0",
            "tmat.f32 f0, s0, t1",
            "tmat.w8 a0, x32, t1",
            "tmat.bmm a0, s0, f1",
            "tmat.conv a0, s0, t1, rne",
            "tvec.copy a0, s0, x0",
            "tvec.nearest a0, f1",
        ):
            with self.subTest(line=line), self.assertRaises(ValueError):
                encode(line)


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
