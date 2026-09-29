"""MSD scripts ("MSCD" VM bytecode, docs/re/text-engine.md §5.1): walk records, compare versions.

Record layout (all 4-byte aligned, u16 op first):
  8        text: u16 8, u16 reg, u16 len, char[len] (NUL included), padded to 4
  6        raw block: u16 6, u16 len, len bytes, padded to 4
  5        jump (8 B), 7 arithmetic (12 B), 9 conditional skip (12 B)
  0x0A-0x0E host command: u16 op, u16 cmd, {u16 is_reg, u16 value} x (op - 0x0A)
"""
from __future__ import annotations

import difflib
import struct
from dataclasses import dataclass

_FIXED = {5: 8, 7: 12, 9: 12, 0x0A: 4, 0x0B: 8, 0x0C: 12, 0x0D: 16, 0x0E: 20}
TEXT = 8
JUMP = 5
# Host commands that only page through / show text: the US re-flowed its dialogue, adding and
# removing these along with text records.
SHOW_TEXT = {(0x0A, 4), (0x0A, 5)}


@dataclass
class Record:
    offset: int
    op: int
    raw: bytes            # the whole record (text: header only, 6 bytes)
    text: bytes | None    # op 8: the string (NUL included)


def walk(script: bytes) -> list[Record]:
    """Every record after the 16-byte header; raises ValueError on an unknown op."""
    if script[:4] != b"MSCD":
        raise ValueError("not an MSD script")
    out: list[Record] = []
    off = 16
    while off < len(script):
        (op,) = struct.unpack_from("<H", script, off)
        if op == TEXT:
            (length,) = struct.unpack_from("<H", script, off + 4)
            size = (6 + length + 3) & ~3
            out.append(Record(off, op, script[off:off + 6], script[off + 6:off + 6 + length]))
        elif op == 6:
            (length,) = struct.unpack_from("<H", script, off + 2)
            size = (4 + length + 3) & ~3
            out.append(Record(off, op, script[off:off + 4 + length], None))
        elif op in _FIXED:
            size = _FIXED[op]
            out.append(Record(off, op, script[off:off + size], None))
        else:
            raise ValueError(f"unknown op {op:#x} at {off:#x}")
        off += size
    return out


def _shape(rec: Record) -> tuple:
    """What must match between versions: text by register, jumps by kind, the rest byte for byte."""
    if rec.op == TEXT:
        return ("text", rec.raw[2:4])
    if rec.op == JUMP:
        return ("jump", rec.raw[:4])
    return ("rec", rec.raw)


def _is_text_side(rec: Record) -> bool:
    if rec.op == TEXT:
        return True
    return rec.op == 0x0A and (rec.op, struct.unpack_from("<H", rec.raw, 2)[0]) in SHOW_TEXT


def same_program(jp: bytes, us: bytes) -> tuple[bool, str]:
    """True when the two scripts differ only in text records and show-text commands (the US
    re-flowed its dialogue), i.e. the US script can run on the JP game in place of the JP one."""
    a_recs, b_recs = walk(jp), walk(us)
    a = [_shape(r) for r in a_recs]
    b = [_shape(r) for r in b_recs]
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        for rec in a_recs[i1:i2] + b_recs[j1:j2]:
            if not _is_text_side(rec):
                return False, f"record op {rec.op:#x} at {rec.offset:#x} differs"
    return True, ""
