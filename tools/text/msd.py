"""MSD scripts ("MSCD" VM bytecode, docs/re/text-engine.md §5.1): walk records, compare versions.

Record layout (all 4-byte aligned, u16 op first):
  8        text: u16 8, u16 reg, u16 len, char[len] (NUL included), padded to 4
  6        raw block: u16 6, u16 len, len bytes, padded to 4
  5        jump (8 B), 7 arithmetic (12 B), 9 conditional skip (12 B)
  0x0A-0x0E host command: u16 op, u16 cmd, {u16 is_reg, u16 value} x (op - 0x0A)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

_FIXED = {5: 8, 7: 12, 9: 12, 0x0A: 4, 0x0B: 8, 0x0C: 12, 0x0D: 16, 0x0E: 20}
TEXT = 8
JUMP = 5
# Host commands that only page through / show text: the US re-flowed its dialogue, adding and
# removing these along with text records.
SHOW_TEXT = frozenset({(0x0A, 4), (0x0A, 5)})


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
    """What must match between versions: jumps by kind (their offsets move with the text), the
    rest byte for byte."""
    if rec.op == JUMP:
        return ("jump", rec.raw[:4])
    return ("rec", rec.raw)


def _is_text_side(rec: Record, show_text: frozenset = SHOW_TEXT) -> bool:
    if rec.op == TEXT:
        return True
    return rec.op == 0x0A and (rec.op, struct.unpack_from("<H", rec.raw, 2)[0]) in show_text


def is_button_test(rec: Record, button_regs: frozenset) -> bool:
    """A conditional skip (op 9) on one of the host's pad registers."""
    return rec.op == 9 and struct.unpack_from("<H", rec.raw, 2)[0] in button_regs


def skeleton(script: bytes, show_text: frozenset = SHOW_TEXT) -> list[Record]:
    """The records that are not text-side: the program with its dialogue taken out."""
    return [r for r in walk(script) if not _is_text_side(r, show_text)]


def same_program(jp: bytes, us: bytes, show_text: frozenset = SHOW_TEXT,
                 button_regs: frozenset = frozenset()) -> tuple[bool, str]:
    """True when the two scripts differ only in text records and show-text commands (the US
    re-flowed its dialogue), i.e. the US script can run on the JP game in place of the JP one.

    Compared as skeletons: with every text-side record taken out, the two record lists must be
    equal (jumps by kind), so text may grow, shrink, move to other pages or gain pages anywhere.
    `show_text` is the host's set of (op, cmd) that only show / page text (the city host: 0x0A
    cmd 4/5). `button_regs`: pad registers the host fills; a test of one (op 9) may test another
    pad register at the same place (the US moved confirm/cancel), see `jp_button_tests`. Text
    records must load the same registers on both sides.
    """
    a, b = skeleton(jp, show_text), skeleton(us, show_text)
    for ra, rb in zip(a, b):
        if _shape(ra) == _shape(rb):
            continue
        if (is_button_test(ra, button_regs) and is_button_test(rb, button_regs)
                and ra.raw[4:] == rb.raw[4:]):
            continue
        return False, f"record op {rb.op:#x} at {rb.offset:#x} differs"
    if len(a) != len(b):
        extra = (a if len(a) > len(b) else b)[min(len(a), len(b))]
        return False, f"record op {extra.op:#x} at {extra.offset:#x} differs"
    regs = [{struct.unpack_from("<H", r.raw, 2)[0] for r in walk(s) if r.op == TEXT} for s in (jp, us)]
    if regs[0] != regs[1]:
        return False, f"text registers differ: {sorted(regs[0])} / {sorted(regs[1])}"
    return True, ""
