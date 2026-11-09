#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""汇编文档中约定的教学指令子集，支持只读 CSR 的 csrr 伪指令。

每条指令编码成 32 位指令字；tmat/tvec 是项目的 custom-0 扩展，
不是 RISC-V 标准指令。本工具不编译 C，也不实现完整的 GNU 汇编语言。
"""
import argparse
from functools import lru_cache
from pathlib import Path
import re
import struct


ABI_REGISTERS = (
    "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 "
    "s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6"
).split()

# 浮点算术和转换的可选舍入模式；省略时沿用已有 RNE 编码。
ROUNDING_MODES = {"rne": 0, "rtz": 1, "rdn": 2, "rup": 3, "rmm": 4, "dyn": 7}

# 自定义指令只登记通用头中的宏名，不复制 opcode、kind 或 fmt 数值。
TENSOR_INSTRUCTIONS = {
    "tmat.f32": ("GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_LINEAR_F32"),
    "tmat.w8": ("GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_LINEAR_W8"),
    "tmat.bmm": ("GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_BMM"),
    "tmat.conv": ("GPGPU_TENSOR_KIND_MATRIX", "GPGPU_TENSOR_CONV"),
    "tvec.copy": ("GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_COPY"),
    "tvec.concat": ("GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_CONCAT"),
    "tvec.nearest": ("GPGPU_TENSOR_KIND_LAYOUT", "GPGPU_TENSOR_NEAREST"),
}


@lru_cache(maxsize=1)
def tensor_constants():
    """首次编码张量指令时读取同层共享头，仅接收纯数值宏。"""
    path = Path(__file__).resolve().parent.parent / "include/gpgpu/gpgpu_tensor_abi.h"
    definitions = re.findall(
        r"^#define[ \t]+(GPGPU_TENSOR_\w+)[ \t]+(0[xX][0-9a-fA-F]+|[0-9]+)[ \t]*$",
        path.read_text(), re.MULTILINE)
    return {name: int(value, 0) for name, value in definitions}


def register(name, floating=False):
    """解析整数或浮点寄存器名称，核对编号范围后返回编码。"""
    prefix = "f" if floating else "x"
    if re.fullmatch(prefix + r"\d+", name):
        value = int(name[1:])
        if value < 32:
            return value
    if not floating and name in ABI_REGISTERS:
        return ABI_REGISTERS.index(name)
    kind = "FP" if floating else "integer"
    raise ValueError(f"invalid {kind} register {name}")


def immediate(text, symbols):
    """从符号表或数值字面量取得立即数，未知名称作为汇编错误返回。"""
    if text in symbols:
        return symbols[text]
    try:
        return int(text, 0)
    except ValueError as exc:
        raise ValueError(f"unknown immediate or label {text}") from exc


def signed(value, bits, alignment=1):
    """检查有符号立即数的位宽和对齐，再返回对应补码位段。"""
    if value % alignment or not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
        raise ValueError(
            f"immediate {value} outside aligned signed {bits} bits")
    return value & ((1 << bits) - 1)


def memory(text, symbols):
    """解析 offset(base) 操作数，分别校验 12 位偏移和基址寄存器。"""
    match = re.fullmatch(r"([^()]+)\(([^()]+)\)", text)
    if not match:
        raise ValueError(f"expected offset(base), got {text}")
    return signed(immediate(match[1], symbols), 12), register(match[2])


def encode(line, pc, symbols):
    """按助记符校验操作数，并将一条教学指令编码成 32 位的 RV64 指令字。"""
    fields = line.replace(",", " ").split()
    op, operands = fields[0], fields[1:]
    counts = {"ebreak": 0, "lui": 2, "auipc": 2, "jal": 2, "jalr": 2,
              "csrr": 2, "fsqrt.s": 2, "fcvt.s.w": 2, "fcvt.w.s": 2,
              "fmv.x.w": 2, "fmv.w.x": 2, "fmadd.s": 4}
    expected = counts.get(op, 3)
    if op in TENSOR_INSTRUCTIONS:
        expected = 3 if op.startswith("tmat.") else 2
    if op in ("lb", "lbu", "lw", "ld", "sb", "sw", "sd", "flw", "fsw"):
        expected = 2
    if op == "jalr" and len(operands) == 3:
        expected = 3
    rm = 0
    if op in ("fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fsqrt.s", "fmadd.s",
              "fcvt.s.w", "fcvt.w.s") and len(operands) == expected + 1:
        name = operands.pop()
        if name not in ROUNDING_MODES:
            raise ValueError(f"unsupported rounding mode {name}")
        rm = ROUNDING_MODES[name]
    if len(operands) != expected:
        raise ValueError(f"{op} expects {expected} operands")
    if op == "ebreak":
        return 0x00100073
    if op in TENSOR_INSTRUCTIONS:
        # custom-0 R4：rd 固定 x0；布局隐含 rs3=x0，不接受 rm 操作数。
        abi = tensor_constants()
        kind, fmt = TENSOR_INSTRUCTIONS[op]
        rs1, rs2 = map(register, operands[:2])
        rs3 = register(operands[2]) if expected == 3 else 0
        return (rs3 << 27 | abi[fmt] << 25 | rs2 << 20 | rs1 << 15 |
                abi[kind] << 12 | abi["GPGPU_TENSOR_OPCODE"])
    if op in ("csrr", "csrrs"):
        rd = register(operands[0])
        csr = immediate(operands[1], symbols)
        if not 0 <= csr < 1 << 12:
            raise ValueError("CSR operand must be in 0..4095")
        rs1 = 0 if op == "csrr" else register(operands[2])
        return (csr << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x73
    if op in ("lui", "auipc"):
        rd = register(operands[0])
        imm = immediate(operands[1], symbols)
        if not 0 <= imm < 1 << 20:
            raise ValueError(f"{op} requires an unsigned 20-bit immediate")
        return (imm << 12) | (rd << 7) | (0x17 if op == "auipc" else 0x37)
    if op in ("addi", "andi", "ori", "xori", "slli", "srli", "srai"):
        rd, rs1 = map(register, operands[:2])
        imm = immediate(operands[2], symbols)
        if op in ("slli", "srli", "srai"):
            if not 0 <= imm < 64:
                raise ValueError(f"{op} shift must be in 0..63")
            if op == "srai":
                imm |= 0x400
        else:
            imm = signed(imm, 12)
        funct3 = {"addi": 0, "slli": 1, "xori": 4, "srli": 5, "srai": 5,
                  "ori": 6, "andi": 7}[op]
        return (imm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x13
    if op in ("add", "sub", "mul", "and", "or", "xor", "divu", "remu"):
        rd, rs1, rs2 = map(register, operands)
        funct7 = {"sub": 0x20, "mul": 1, "divu": 1, "remu": 1}.get(op, 0)
        funct3 = {"xor": 4, "divu": 5, "or": 6, "and": 7, "remu": 7}.get(op, 0)
        return ((funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
                (funct3 << 12) | (rd << 7) | 0x33)
    if op in ("fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fmin.s", "fmax.s"):
        rd, rs1, rs2 = [register(arg, True) for arg in operands]
        funct7 = {"fadd.s": 0, "fsub.s": 4, "fmul.s": 8, "fdiv.s": 12,
                  "fmin.s": 20, "fmax.s": 20}[op]
        funct3 = 1 if op == "fmax.s" else rm
        return ((funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
                (funct3 << 12) | (rd << 7) | 0x53)
    if op in ("feq.s", "flt.s", "fle.s"):
        rd = register(operands[0])
        rs1, rs2 = [register(arg, True) for arg in operands[1:]]
        funct3 = {"fle.s": 0, "flt.s": 1, "feq.s": 2}[op]
        return (0x50 << 25 | rs2 << 20 | rs1 << 15 |
                funct3 << 12 | rd << 7 | 0x53)
    if op in ("fsqrt.s", "fcvt.s.w", "fcvt.w.s", "fmv.x.w", "fmv.w.x"):
        rd = register(operands[0], op not in ("fcvt.w.s", "fmv.x.w"))
        rs1 = register(operands[1], op not in ("fcvt.s.w", "fmv.w.x"))
        funct7 = {"fsqrt.s": 0x2c, "fcvt.s.w": 0x68, "fcvt.w.s": 0x60,
                  "fmv.x.w": 0x70, "fmv.w.x": 0x78}[op]
        return funct7 << 25 | rs1 << 15 | rm << 12 | rd << 7 | 0x53
    if op == "fmadd.s":
        rd, rs1, rs2, rs3 = [register(arg, True) for arg in operands]
        return (rs3 << 27 | rs2 << 20 | rs1 << 15 |
                rm << 12 | rd << 7 | 0x43)
    if op in ("lb", "lbu", "lw", "ld", "sb", "sw", "sd", "flw", "fsw"):
        reg = register(operands[0], op.startswith("f"))
        imm, base = memory(operands[1], symbols)
        opcode = {"lb": 0x03, "lbu": 0x03, "lw": 0x03, "ld": 0x03,
                  "sb": 0x23, "sw": 0x23, "sd": 0x23,
                  "flw": 0x07, "fsw": 0x27}[op]
        funct3 = {"lb": 0, "lbu": 4, "sb": 0, "ld": 3, "sd": 3}.get(op, 2)
        if op in ("sb", "sw", "sd", "fsw"):
            return ((imm >> 5) << 25 | reg << 20 | base << 15 |
                    funct3 << 12 | (imm & 31) << 7 | opcode)
        return imm << 20 | base << 15 | funct3 << 12 | reg << 7 | opcode
    if op in ("beq", "bne", "blt", "bge", "bltu", "bgeu"):
        rs1, rs2 = map(register, operands[:2])
        imm = signed(immediate(operands[2], symbols) - pc, 13, 4)
        funct3 = {"beq": 0, "bne": 1, "blt": 4,
                  "bge": 5, "bltu": 6, "bgeu": 7}[op]
        return ((imm >> 12) << 31 | ((imm >> 5) & 63) << 25 |
                rs2 << 20 | rs1 << 15 | funct3 << 12 |
                ((imm >> 1) & 15) << 8 | ((imm >> 11) & 1) << 7 | 0x63)
    if op == "jalr":
        rd = register(operands[0])
        if len(operands) == 2:
            imm, base = memory(operands[1], symbols)
        else:
            base = register(operands[1])
            imm = signed(immediate(operands[2], symbols), 12)
        return imm << 20 | base << 15 | rd << 7 | 0x67
    if op == "jal":
        rd = register(operands[0])
        imm = signed(immediate(operands[1], symbols) - pc, 21, 4)
        return ((imm >> 20) << 31 | ((imm >> 1) & 1023) << 21 |
                ((imm >> 11) & 1) << 20 | ((imm >> 12) & 255) << 12 |
                rd << 7 | 0x6f)
    raise ValueError(f"unsupported instruction {op}")


def source_lines(path, visited=None):
    """展开相对 include 并保留源码位置，拒绝递归包含。"""
    visited = set() if visited is None else visited
    path = path.resolve()
    if path in visited:
        raise ValueError(f"recursive include {path}")
    visited.add(path)
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r'\.include\s+"([^"]+)"', line)
        if match:
            yield from source_lines(path.parent / match[1], visited)
        else:
            yield f"{path.name}:{number}", line
    visited.remove(path)


def assemble(path):
    """先收集符号和 PC，再编码指令，输出连续的小端字节串。"""
    symbols, instructions = {}, []
    pc = 0
    for location, line in source_lines(path):
        try:
            if line.startswith(".equ "):
                name, value = [part.strip() for part in line[5:].split(",")]
                if name in symbols:
                    raise ValueError(f"duplicate symbol {name}")
                symbols[name] = immediate(value, symbols)
            elif line.endswith(":"):
                name = line[:-1]
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                    raise ValueError(f"invalid label {name}")
                if name in symbols:
                    raise ValueError(f"duplicate symbol {name}")
                symbols[name] = pc
            else:
                instructions.append((location, line, pc))
                pc += 4
        except ValueError as exc:
            raise ValueError(f"{location}: {exc}") from exc
    output = bytearray()
    for location, line, pc in instructions:
        try:
            output.extend(struct.pack("<I", encode(line, pc, symbols)))
        except ValueError as exc:
            raise ValueError(f"{location}: {exc}") from exc
    if not output:
        raise ValueError("kernel has no instructions")
    return bytes(output)


def main():
    """解析输出路径并汇编源文件，同时生成二进制和用户态 C 数组头文件。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", args.symbol):
        parser.error("symbol must be a C identifier")
    try:
        output = assemble(args.source)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    args.bin.write_bytes(output)
    rows = ["    " + ", ".join(f"0x{byte:02x}" for byte in output[i:i + 12])
            + "," for i in range(0, len(output), 12)]
    args.header.write_text(
        "/* 由 kernels/assemble.py 生成，请勿编辑。 */\n"
        "#include <stdint.h>\n"
        f"static const uint8_t {args.symbol}[] = {{\n"
        + "\n".join(rows) + "\n};\n"
    )


if __name__ == "__main__":
    main()
