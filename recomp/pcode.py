#!/usr/bin/env python3
"""Parser for the TSV produced by recomp/ghidra_scripts/DumpPcode.java."""
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class Varnode:
    space: str
    value: str
    size: int

    @property
    def is_const(self) -> bool:
        return self.space == "const"

    @property
    def is_reg(self) -> bool:
        return self.space == "reg"

    @property
    def is_unique(self) -> bool:
        return self.space == "uniq"

    @property
    def is_ram(self) -> bool:
        return self.space == "ram"

    def __str__(self) -> str:
        return f"{self.space}:{self.value}:{self.size}"


@dataclass
class Op:
    opcode: str
    out: Optional[Varnode]
    ins: List[Varnode]
    userop: Optional[str] = None


@dataclass
class Insn:
    addr: int
    length: int
    mnemonic: str
    ops: List[Op] = field(default_factory=list)


@dataclass
class Func:
    entry: int
    name: str
    size: int
    external: bool
    thunk: bool
    insns: List[Insn] = field(default_factory=list)

    @property
    def addr_to_insn(self):
        return {i.addr: i for i in self.insns}


def parse_varnode(text: str, image_base: int = 0) -> Optional[Varnode]:
    if text == "-":
        return None
    parts = text.split(":")
    if len(parts) != 3:
        raise ValueError(f"bad varnode {text!r}")
    space, value, size = parts
    if space == "ram":
        value = str(int(value, 10) - image_base)
    return Varnode(space, value, int(size))


def _addr(text: str) -> int:
    return int(text.strip(), 16)


def parse(path: Path):
    funcs = []
    header = None
    image_base = 0
    cur_func = None
    cur_insn = None
    with path.open("r", encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            tag, _, rest = line.partition("\t")
            if tag == "H":
                header = rest.split("\t")
                image_base = int(header[1], 16)
            elif tag == "F":
                entry_s, name, size, external, thunk = rest.split("\t")
                cur_func = Func(_addr(entry_s) - image_base,
                                name, int(size), external == "1", thunk == "1")
                funcs.append(cur_func)
                cur_insn = None
            elif tag == "I":
                addr_s, length, mnemonic = rest.split("\t")
                cur_insn = Insn(_addr(addr_s) - image_base, int(length), mnemonic)
                cur_func.insns.append(cur_insn)
            elif tag == "P":
                fields = rest.split("\t")
                opcode_field = fields[0]
                userop = None
                if ":" in opcode_field:
                    opcode_field, userop = opcode_field.split(":", 1)
                out = parse_varnode(fields[1], image_base)
                ins = [parse_varnode(x, image_base) for x in fields[2:]]
                cur_insn.ops.append(Op(opcode_field, out, ins, userop))
            elif tag == "X":
                break
    return header, funcs
