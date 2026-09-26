#!/usr/bin/env python3
"""Convert PlayStation TIM images to PNG for previewing (no third-party dependencies).

    tim2png.py FILE.TIM [-o out.png]          # one TIM at offset 0
    tim2png.py --scan FILE [-o outdir]        # find every TIM embedded in a container (ARC, PAK, ...)
    tim2png.py --info FILE [--scan]           # only print geometry: offset, bpp, image/CLUT rect

TIM layout: u32 0x10, u32 flags (bits 0-1 pixel mode 0=4bpp 1=8bpp 2=16bpp 3=24bpp, bit 3 CLUT
present), then optional CLUT block and the image block, each ``u32 bytes, u16 x, y, w, h, data``
(w in 16-bit VRAM units). Paletted images use CLUT row 0 (``--clut N`` picks another row). Colour
0x0000 is written as transparent, as the GPU treats it.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Tim:
    offset: int
    bpp: int
    clut_rect: tuple[int, int, int, int] | None
    clut: list[int]
    img_rect: tuple[int, int, int, int]
    pixels: bytes
    end: int


def parse(data: bytes, off: int = 0) -> Tim | None:
    """Parse one TIM at `off`; None if the bytes are not a plausible TIM."""
    if off + 20 > len(data):
        return None
    magic, flags = struct.unpack_from("<II", data, off)
    if magic != 0x10 or flags & ~0xB:
        return None
    mode = flags & 3
    bpp = (4, 8, 16, 24)[mode]
    p = off + 8
    clut_rect = None
    clut: list[int] = []
    if flags & 8:
        size, x, y, w, h = struct.unpack_from("<IHHHH", data, p)
        if size != 12 + w * h * 2 or w == 0 or h == 0 or p + size > len(data):
            return None
        clut_rect = (x, y, w, h)
        clut = list(struct.unpack_from(f"<{w * h}H", data, p + 12))
        p += size
    elif mode < 2:
        return None
    if p + 12 > len(data):
        return None
    size, x, y, w, h = struct.unpack_from("<IHHHH", data, p)
    if size != 12 + w * h * 2 or w == 0 or h == 0 or p + size > len(data):
        return None
    return Tim(off, bpp, clut_rect, clut, (x, y, w, h), data[p + 12: p + size], p + size)


def _rgba(c: int) -> bytes:
    r, g, b = (c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3
    return bytes((r, g, b, 0 if c == 0 else 255))


def to_rgba(t: Tim, clut_row: int = 0) -> tuple[int, int, bytes]:
    x, y, w, h = t.img_rect
    rows = []
    if t.bpp in (4, 8):
        ncol = 16 if t.bpp == 4 else 256
        base = clut_row * (t.clut_rect[2] if t.clut_rect else ncol)
        pal = [_rgba(t.clut[base + i]) if base + i < len(t.clut) else b"\0\0\0\0" for i in range(ncol)]
        width = w * (4 if t.bpp == 4 else 2)
        for r in range(h):
            line = t.pixels[r * w * 2: (r + 1) * w * 2]
            if t.bpp == 4:
                rows.append(b"".join(pal[b & 15] + pal[b >> 4] for b in line))
            else:
                rows.append(b"".join(pal[b] for b in line))
    elif t.bpp == 16:
        width = w
        for r in range(h):
            rows.append(b"".join(_rgba(v) for v in struct.unpack_from(f"<{w}H", t.pixels, r * w * 2)))
    else:
        width = w * 2 // 3
        for r in range(h):
            line = t.pixels[r * w * 2: r * w * 2 + width * 3]
            rows.append(b"".join(line[i:i + 3] + b"\xff" for i in range(0, len(line), 3)))
    return width, h, b"".join(rows)


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    stride = width * 4
    raw = b"".join(b"\0" + rgba[r * stride:(r + 1) * stride] for r in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def scan(data: bytes) -> list[Tim]:
    """Every TIM found at a 4-byte aligned offset, skipping over each one found."""
    out, off = [], 0
    while off + 20 <= len(data):
        t = parse(data, off) if data[off] == 0x10 else None
        if t:
            out.append(t)
            off = (t.end + 3) & ~3
        else:
            off += 4
    return out


def describe(t: Tim) -> str:
    clut = f" clut {t.clut_rect}" if t.clut_rect else ""
    return f"@{t.offset:#07x} {t.bpp:>2}bpp img(x,y,w,h)={t.img_rect}{clut}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("file", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--scan", action="store_true", help="find all embedded TIMs")
    ap.add_argument("--info", action="store_true", help="print geometry only")
    ap.add_argument("--clut", type=int, default=0, help="CLUT row for paletted images")
    args = ap.parse_args()

    data = args.file.read_bytes()
    tims = scan(data) if args.scan else [t for t in [parse(data)] if t]
    if not tims:
        print(f"{args.file}: no TIM found", file=sys.stderr)
        return 1
    for i, t in enumerate(tims):
        print(f"{args.file.name} #{i} {describe(t)}")
        if args.info:
            continue
        if args.scan:
            outdir = args.out or args.file.with_suffix(".png.d")
            outdir.mkdir(parents=True, exist_ok=True)
            dst = outdir / f"{i:03d}_{t.offset:06x}.png"
        else:
            dst = args.out or args.file.with_suffix(".png")
        write_png(dst, *to_rgba(t, args.clut))
    return 0


if __name__ == "__main__":
    sys.exit(main())
