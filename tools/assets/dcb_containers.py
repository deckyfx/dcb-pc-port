#!/usr/bin/env python3
"""Readers for the containers inside the DCB *.DRV archives, plus JP/US comparison helpers.

Formats (all little-endian; see docs/HYBRID_EN_ASSETS.md for the evidence):

PAK  chunk list ``u16 kind, u16 id, u32 size, u8 data[size]`` ended by u32 0xFFFFFFFF.
     Kinds seen: 0 name/header table, 1/3/4 model and animation data, 2 MSD script ("MSCD"),
     5 image (TIM, or a TIS in C:\\AREAxx.PAK), 6 SEQ ("pQES"), 7 VAB header ("pBAV"), 8 VAB body.
     Chunk order differs between JP and US builds of the same PAK, so the game finds chunks by
     (kind, id), not by position (inferred, not yet traced in code).
ARC  ``u32 offset[n]`` (offset[0] == 4*n, so n = offset[0] / 4), entries are back to back; the
     entries seen are TIMs. Entry i spans offset[i] .. offset[i+1] (last one: end of file).
TIS  ``"Tp", u16 n, u32 word_offset[n]`` (byte offset = 4 * word_offset) then n TIM entries.
CDD  card database ``char[4] magic ("ADCD" JP, "0ACD" US), u16 n_digimon, u8 n_item, u8 n_option``
     then fixed-size records. JP strides 0x134/0xDA/0x68, US 0x13C/0xE2/0x70: the US records are the
     JP records with the four effect-text lines widened from 19 to 21 bytes (+8 bytes per record).

Commands (no game data is written anywhere unless you pass an output path):
    dcb_containers.py pak FILE.PAK            list chunks
    dcb_containers.py arc FILE.ARC            list entries
    dcb_containers.py tis FILE.TIS            list entries
    dcb_containers.py cdd-diff JP.CDD US.CDD  compare non-text card fields
    dcb_containers.py pak-diff JP.PAK US.PAK  compare chunks by (kind, id)
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


# ---------------------------------------------------------------- PAK / ARC / TIS

@dataclass
class Chunk:
    kind: int
    id: int
    offset: int  #: offset of the 8-byte chunk header
    data: bytes


def read_pak(data: bytes) -> list[Chunk]:
    """Split a PAK into chunks; raises ValueError if the chain does not end exactly at EOF."""
    out, off = [], 0
    while off + 4 <= len(data):
        if data[off: off + 4] == b"\xff\xff\xff\xff":  # terminator
            return out
        kind, cid, size = struct.unpack_from("<HHI", data, off)
        if off + 8 + size > len(data):
            raise ValueError(f"chunk at {off:#x} overruns the file")
        out.append(Chunk(kind, cid, off, data[off + 8: off + 8 + size]))
        off += 8 + size
    if off != len(data) and any(data[off:]):
        raise ValueError(f"trailing bytes at {off:#x}")
    return out


def write_pak(chunks: list[Chunk]) -> bytes:
    body = b"".join(struct.pack("<HHI", c.kind, c.id, len(c.data)) + c.data for c in chunks)
    return body + b"\xff\xff\xff\xff"


def read_arc(data: bytes) -> list[bytes]:
    first = struct.unpack_from("<I", data, 0)[0]
    if first % 4 or not 4 <= first <= len(data):
        raise ValueError("not an ARC offset table")
    offs = list(struct.unpack_from(f"<{first // 4}I", data, 0)) + [len(data)]
    return [data[offs[i]: offs[i + 1]] for i in range(len(offs) - 1)]


def read_tis(data: bytes) -> list[bytes]:
    magic, n = struct.unpack_from("<2sH", data, 0)
    if magic != b"Tp":
        raise ValueError("not a TIS")
    offs = [4 * w for w in struct.unpack_from(f"<{n}I", data, 4)] + [len(data)]
    return [data[offs[i]: offs[i + 1]] for i in range(n)]


# ---------------------------------------------------------------- CDD

JP_STRIDES = (0x134, 0xDA, 0x68)
US_STRIDES = (0x13C, 0xE2, 0x70)
#: text fields per record kind in the JP layout: (start, end) byte ranges, then effect-text start
JP_TEXT = {
    "digimon": ([(0x03, 0x18), (0x26, 0x3C), (0x42, 0x58), (0x5E, 0x74)], 0xE7),
    "item": ([(0x03, 0x18)], 0x8D),
    "option": ([(0x03, 0x18)], 0x1B),
}
EFFECT_LINES = 4


def read_cdd(data: bytes) -> list[tuple[str, bytes]]:
    n_dig = struct.unpack_from("<H", data, 4)[0]
    n_item, n_opt = data[6], data[7]
    strides = US_STRIDES if data[:4] == b"0ACD" else JP_STRIDES
    out, off = [], 8
    for kind, n, stride in zip(("digimon", "item", "option"), (n_dig, n_item, n_opt), strides):
        for _ in range(n):
            out.append((kind, data[off: off + stride]))
            off += stride
    return out


def cdd_diff(jp: bytes, us: bytes) -> None:
    rj, ru = read_cdd(jp), read_cdd(us)
    print(f"records JP {len(rj)} US {len(ru)}")
    diffs = []
    for i, ((kind, a), (_, b)) in enumerate(zip(rj, ru)):
        fields, text = JP_TEXT[kind]
        masked = {p for s, e in fields for p in range(s, e)}
        for p in range(2, text):  # bytes 0-1: card number, rewritten by the loader
            if p not in masked and a[p] != b[p]:
                diffs.append((i, kind, p, a[p], b[p]))
    print(f"non-text byte differences: {len(diffs)}")
    for d in diffs:
        print(f"  card {d[0]:3} ({d[1]}) +{d[2]:#04x}: JP {d[3]:#04x} US {d[4]:#04x}")
    over = sum(1 for kind, b in ru for i in range(EFFECT_LINES)
               if len(b[JP_TEXT[kind][1] + 21 * i: JP_TEXT[kind][1] + 21 * (i + 1)].split(b"\0")[0]) > 18)
    print(f"US effect-text lines longer than the JP 19-byte slot allows: {over}")


def pak_diff(jp: bytes, us: bytes) -> None:
    ca = {(c.kind, c.id): c.data for c in read_pak(jp)}
    cb = {(c.kind, c.id): c.data for c in read_pak(us)}
    for key in sorted(set(ca) | set(cb)):
        a, b = ca.get(key), cb.get(key)
        state = "only-JP" if b is None else "only-US" if a is None else "same" if a == b else "changed"
        print(f"  kind {key[0]:2} id {key[1]:#06x}: {state:8} {len(a) if a else '-':>7} -> {len(b) if b else '-':>7}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("cmd", choices=("pak", "arc", "tis", "cdd-diff", "pak-diff"))
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args()
    data = [f.read_bytes() for f in args.files]
    if args.cmd == "pak":
        for c in read_pak(data[0]):
            print(f"@{c.offset:#08x} kind {c.kind:2} id {c.id:#06x} size {len(c.data):8} {c.data[:4].hex()}")
    elif args.cmd in ("arc", "tis"):
        for i, e in enumerate((read_arc if args.cmd == "arc" else read_tis)(data[0])):
            print(f"#{i:3} size {len(e):8} {e[:8].hex()}")
    elif args.cmd == "cdd-diff":
        cdd_diff(data[0], data[1])
    else:
        pak_diff(data[0], data[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
