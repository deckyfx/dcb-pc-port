#!/usr/bin/env python3
"""List or unpack the *.DRV archives of Digimon Card Battle (SLPS-03101 / SLUS-01328).

Every DRV (A-G, P, W-Z) uses the same layout: a tree of tables of contents (TOC), one per
directory, each an array of 32-byte entries terminated by an entry whose type byte is 0::

    +0x00  u8          type: 0x80 = directory, 0x01 = file, 0x00 = end of table
    +0x01  char[3]     file extension (PAK, TIM, ARC, MSD, ...); zero for directories
    +0x04  u32         start sector inside the DRV (x 0x800 = byte offset); for a directory,
                       the sector of its own TOC
    +0x08  u32         size in bytes (0 for directories)
    +0x0C  u32         timestamp (Unix time)
    +0x10  char[16]    name, NUL-padded (ASCII; a few US names are Shift-JIS)

The root TOC is at sector 0. There is no compression at this level; files start on sector
boundaries and the space up to the next sector is zero padding. (Some contained formats, e.g. PAK,
are containers of their own; see tools/assets/dcb_containers.py.)

The game resolves every file by name at run time ("B:\\CARD2.CDD" -> CdSearchFile("\\B.DRV;1") ->
walk the TOC; SLPS-03101 FUN_80015c24), so an archive can be rebuilt with entries of other sizes.

Usage:
    drv_unpack.py list  B.DRV                      # path, sector, size, sha1 per entry
    drv_unpack.py unpack B.DRV out/B               # write every entry as out/B/<path>
    drv_unpack.py json  B.DRV > B.json             # manifest (for drv_diff.py)
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

SECTOR = 0x800
ENTRY = 32
TYPE_DIR = 0x80
TYPE_FILE = 0x01


@dataclass
class Entry:
    path: str        #: "DIR/NAME.EXT"
    sector: int      #: start sector inside the DRV
    size: int        #: bytes
    stamp: int       #: Unix timestamp from the TOC
    toc_offset: int  #: byte offset of the 32-byte TOC record inside the DRV
    sha1: str = ""

    @property
    def offset(self) -> int:
        return self.sector * SECTOR


def _name(raw: bytes) -> str:
    raw = raw.split(b"\0", 1)[0]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return raw.decode("cp932", errors="replace")


def read_toc(drv: bytes) -> tuple[list[Entry], list[str]]:
    """Walk the TOC tree; return (files in TOC order, directory paths)."""
    files: list[Entry] = []
    dirs: list[str] = []

    def walk(sector: int, prefix: str, depth: int) -> None:
        if depth > 8:
            raise ValueError("TOC nesting too deep; not a DRV?")
        off = sector * SECTOR
        while off + ENTRY <= len(drv):
            kind = drv[off]
            if kind == 0:
                break
            ext = drv[off + 1: off + 4].split(b"\0", 1)[0].decode("latin1")
            start, size, stamp = struct.unpack_from("<III", drv, off + 4)
            name = _name(drv[off + 16: off + 32])
            if kind == TYPE_DIR:
                dirs.append(prefix + name)
                walk(start, prefix + name + "/", depth + 1)
            elif kind == TYPE_FILE:
                if start * SECTOR + size > len(drv):
                    # Stale record: US B.DRV lists "コピー ～ CARD2.CDD" (a Windows "copy of"
                    # file) whose data was never written into the archive. The game never opens it.
                    print(f"warning: {prefix}{name}.{ext} points past the end of the archive; skipped",
                          file=sys.stderr)
                else:
                    files.append(Entry(f"{prefix}{name}.{ext}", start, size, stamp, off))
            else:
                raise ValueError(f"unknown TOC entry type {kind:#x} at {off:#x}")
            off += ENTRY

    walk(0, "", 0)
    return files, dirs


def load(path: Path, hashes: bool = True) -> tuple[bytes, list[Entry], list[str]]:
    drv = path.read_bytes()
    files, dirs = read_toc(drv)
    if hashes:
        for e in files:
            e.sha1 = hashlib.sha1(drv[e.offset: e.offset + e.size]).hexdigest()
    return drv, files, dirs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("cmd", choices=("list", "unpack", "json"))
    ap.add_argument("drv", type=Path)
    ap.add_argument("out", type=Path, nargs="?", help="output directory (unpack)")
    args = ap.parse_args()

    drv, files, dirs = load(args.drv)
    if args.cmd == "list":
        for e in files:
            day = datetime.datetime.fromtimestamp(e.stamp, datetime.timezone.utc).strftime("%Y-%m-%d")
            print(f"{e.path:<28} sec {e.sector:>6}  size {e.size:>9}  {day}  {e.sha1[:12]}")
        print(f"{len(files)} files in {len(dirs)} directories", file=sys.stderr)
    elif args.cmd == "json":
        json.dump({"archive": args.drv.name, "size": len(drv), "dirs": dirs,
                   "files": [asdict(e) for e in files]}, sys.stdout, indent=1, ensure_ascii=False)
        print()
    else:
        if args.out is None:
            ap.error("unpack needs an output directory")
        for e in files:
            dst = args.out / e.path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(drv[e.offset: e.offset + e.size])
        print(f"unpacked {len(files)} files to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
