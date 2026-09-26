#!/usr/bin/env python3
"""List the code overlays packed in P.DRV and write config/<serial>/overlays.json.

P.DRV starts with a table of 32-byte entries::

    +0x00  b"\\x01BIN"   magic
    +0x04  u32         start sector within P.DRV (x 0x800 = byte offset)
    +0x08  u32         size in bytes
    +0x0C  u32         build timestamp (Unix time)
    +0x10  char[16]    segment name, NUL-padded (ENDSEG, KAWSEG, ...)

Every segment is loaded at the first byte after the boot EXE's .bss, and the game swaps them
per mode. That address differs per build: 0x801E0B30 on SLPS-03101 (default; all 69 boot-EXE call
targets verified) and 0x801DE738 on SLUS-01328 (pass --load-address).
"""
from __future__ import annotations

import argparse
import datetime
import json
import struct
import sys
from pathlib import Path

MAGIC = b"\x01BIN"
ENTRY_SIZE = 32
SECTOR = 0x800
DEFAULT_LOAD = 0x801E0B30


def read_segments(pdrv: bytes) -> list[dict[str, object]]:
    segments = []
    for off in range(0, SECTOR, ENTRY_SIZE):
        magic, sector, size, stamp = struct.unpack_from("<4sIII", pdrv, off)
        if magic != MAGIC:
            break
        start = sector * SECTOR
        if start + size > len(pdrv):
            raise ValueError(f"segment at TOC {off:#x} runs past the end of P.DRV")
        segments.append({
            "name": pdrv[off + 16 : off + 32].split(b"\0", 1)[0].decode("ascii"),
            "file_offset": f"0x{start:X}",
            "size": f"0x{size:X}",
            "built": datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime("%Y-%m-%d"),
        })
    return segments


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("serial", help="disc serial, e.g. SLPS-03101")
    ap.add_argument("--load-address", default=f"0x{DEFAULT_LOAD:08X}")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[2]
    pdrv = root / "extracted" / args.serial / "fs" / "P.DRV"
    if not pdrv.is_file():
        print(f"error: {pdrv} missing; run tools/disc/extract_disc.py first", file=sys.stderr)
        return 1

    segments = read_segments(pdrv.read_bytes())
    out = root / "config" / args.serial / "overlays.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        "container": "P.DRV",
        "load_address": args.load_address,
        "segments": segments,
    }, indent=2) + "\n")
    for s in segments:
        print(f"{s['name']:<8} {s['file_offset']:>8} {s['size']:>8}  built {s['built']}")
    print(f"wrote {out.relative_to(root)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
