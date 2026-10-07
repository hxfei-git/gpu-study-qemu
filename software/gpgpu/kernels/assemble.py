#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble the documented teaching subset, with no pseudo instructions.

Every instruction is a 32-bit RV32 encoding. This is deliberately not a C
compiler or an implementation of the whole GNU assembler language.
"""
import argparse
from pathlib import Path
import re
import struct


ABI_REGISTERS = (
    "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 "
    "s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6"
).split()


def register(name, floating=False):
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
    if text in symbols:
        return symbols[text]
    try:
        return int(text, 0)
    except ValueError as exc:
        raise ValueError(f"unknown immediate or label {text}") from exc


def signed(value, bits, alignment=1):
    if value % alignment or not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
        raise ValueError(
            f"immediate {value} outside aligned signed {bits} bits")
    return value & ((1 << bits) - 1)


def memory(text, symbols):
    match = re.fullmatch(r"([^()]+)\(([^()]+)\)", text)
    if not match:
        raise ValueError(f"expected offset(base), got {text}")
    return signed(immediate(match[1], symbols), 12), register(match[2])


def encode(line, pc, symbols):
    fields = line.replace(",", " ").split()
    op, operands = fields[0], fields[1:]
    counts = {"ebreak": 0, "lui": 2, "jal": 2}
    expected = counts.get(op, 3)
    if op in ("lw", "sw", "flw", "fsw"):
        expected = 2
    if len(operands) != expected:
        raise ValueError(f"{op} expects {expected} operands")
    if op == "ebreak":
        return 0x00100073
    if op == "lui":
        rd = register(operands[0])
        imm = immediate(operands[1], symbols)
        if not 0 <= imm < 1 << 20:
            raise ValueError("lui requires an unsigned 20-bit immediate")
        return (imm << 12) | (rd << 7) | 0x37
    if op in ("addi", "andi", "slli"):
        rd, rs1 = map(register, operands[:2])
        imm = immediate(operands[2], symbols)
        if op == "slli":
            if not 0 <= imm < 32:
                raise ValueError("slli shift must be in 0..31")
        else:
            imm = signed(imm, 12)
        funct3 = {"addi": 0, "andi": 7, "slli": 1}[op]
        return (imm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x13
    if op in ("add", "sub", "mul", "fadd.s", "fmul.s"):
        fp = op.endswith(".s")
        rd, rs1, rs2 = [register(arg, fp) for arg in operands]
        funct7 = {"add": 0, "sub": 0x20, "mul": 1,
                  "fadd.s": 0, "fmul.s": 8}[op]
        return ((funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
                (rd << 7) | (0x53 if fp else 0x33))
    if op in ("lw", "sw", "flw", "fsw"):
        reg = register(operands[0], op.startswith("f"))
        imm, base = memory(operands[1], symbols)
        opcode = {"lw": 0x03, "sw": 0x23, "flw": 0x07, "fsw": 0x27}[op]
        if op.endswith("sw") or op == "sw":
            return ((imm >> 5) << 25 | reg << 20 | base << 15 |
                    2 << 12 | (imm & 31) << 7 | opcode)
        return imm << 20 | base << 15 | 2 << 12 | reg << 7 | opcode
    if op in ("beq", "bne", "blt", "bge", "bltu", "bgeu"):
        rs1, rs2 = map(register, operands[:2])
        imm = signed(immediate(operands[2], symbols) - pc, 13, 4)
        funct3 = {"beq": 0, "bne": 1, "blt": 4,
                  "bge": 5, "bltu": 6, "bgeu": 7}[op]
        return ((imm >> 12) << 31 | ((imm >> 5) & 63) << 25 |
                rs2 << 20 | rs1 << 15 | funct3 << 12 |
                ((imm >> 1) & 15) << 8 | ((imm >> 11) & 1) << 7 | 0x63)
    if op == "jal":
        rd = register(operands[0])
        imm = signed(immediate(operands[1], symbols) - pc, 21, 4)
        return ((imm >> 20) << 31 | ((imm >> 1) & 1023) << 21 |
                ((imm >> 11) & 1) << 20 | ((imm >> 12) & 255) << 12 |
                rd << 7 | 0x6f)
    raise ValueError(f"unsupported instruction {op}")


def source_lines(path, visited=None):
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
        "/* Generated by kernels/assemble.py; do not edit. */\n"
        "#include <stdint.h>\n"
        f"static const uint8_t {args.symbol}[] = {{\n"
        + "\n".join(rows) + "\n};\n"
    )


if __name__ == "__main__":
    main()
