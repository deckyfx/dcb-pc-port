"""VS-screen big names in English: assets from the US disc (docs/re/text-engine.md section 7.11).

Two things, both written from the player's own dumps into gitignored assets/SLPS-03101/:

  en_bigfont.bin      the US big-name font (B:\\FONT.ARC, 16x32 4-bpp glyphs) repacked for the
                      runtime override of the JP loader 80044684 (src/game/overrides/bigname.cpp)
  files/B/MATCH/NNN.ARC
                      the JP match archives with the opponent's name picture (the last TIM,
                      JP katakana) replaced by the US one ("Meramon"); every other TIM stays JP

FONT.ARC (US B.DRV, 20848 bytes): 92 u32 offsets for the characters 0x20..0x7B, then 320-byte
TIMs (4 bpp, one 16-entry CLUT, 4 halfwords x 32 rows). Characters without a glyph share the
next glyph's TIM ('!'..',' point at '-', '.' and '/' at '0', ':'..'@' at 'A', '['..'`' at
'a'); '{' points at the end of the file. Only space, '-', digits and letters have their own.

en_bigfont.bin layout (little endian):
  "BGF1"  u8 first (0x20)  u8 count (92)  u8 width in halfwords (4)  u8 height (32)
  32 B    CLUT (16 x u16, the same in every glyph TIM)
  count B 1 when the character has its own glyph, else 0 (the runtime draws a space)
  count x 256 B  glyph pixels, 4 bpp, 8 bytes per row (zeros for characters without a glyph)
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "disc"))
import drv_unpack as _drv  # noqa: E402

MAGIC = b"BGF1"
FIRST = 0x20
COUNT = 92          # 0x20..0x7B
GLYPH_HW = 4        # halfwords per row (16 px at 4 bpp)
GLYPH_H = 32
GLYPH_BYTES = GLYPH_HW * 2 * GLYPH_H
TIM_SIZE = 320      # 8 header + 12 + 32 CLUT block + 12 + 256 pixel block


def _tim_parts(tim: bytes) -> tuple[bytes, bytes]:
    """(CLUT 32 B, pixels 256 B) of one FONT.ARC glyph TIM; asserts the expected geometry."""
    magic, flags = struct.unpack_from("<II", tim, 0)
    assert magic == 0x10 and flags == 8, "glyph is not a 4-bpp TIM with a CLUT"
    cl_len, _, _, cw, ch = struct.unpack_from("<IHHHH", tim, 8)
    assert (cw, ch) == (16, 1), f"unexpected CLUT {cw}x{ch}"
    p = 8 + cl_len
    px_len, _, _, w, h = struct.unpack_from("<IHHHH", tim, p)
    assert (w, h) == (GLYPH_HW, GLYPH_H), f"unexpected glyph {w}x{h}"
    return tim[20:52], tim[p + 12:p + 12 + GLYPH_BYTES]


def parse_font_arc(arc: bytes) -> tuple[bytes, dict[int, bytes]]:
    """(CLUT, {char code: pixels}) for the characters that have their own glyph.

    Several codes share one TIM; the glyph belongs to the last code of each run ('-' for
    '!'..'-', '0' for '.'..'0', ...), which is the character it shows.
    """
    offs = struct.unpack_from(f"<{COUNT}I", arc, 0)
    assert offs[0] == COUNT * 4, "FONT.ARC: bad offset table"
    owner: dict[int, int] = {}  # TIM offset -> the last code pointing at it
    for i, off in enumerate(offs):
        if off + TIM_SIZE <= len(arc):
            owner[off] = FIRST + i
    clut = b""
    glyphs: dict[int, bytes] = {}
    for off, code in owner.items():
        c, px = _tim_parts(arc[off:off + TIM_SIZE])
        if clut and c != clut:
            raise ValueError("FONT.ARC: glyphs with different CLUTs")
        clut = c
        glyphs[code] = px
    return clut, glyphs


def build_bigfont(arc: bytes) -> bytes:
    """en_bigfont.bin from the US FONT.ARC."""
    clut, glyphs = parse_font_arc(arc)
    present = bytes(1 if FIRST + i in glyphs else 0 for i in range(COUNT))
    pixels = b"".join(glyphs.get(FIRST + i, bytes(GLYPH_BYTES)) for i in range(COUNT))
    return MAGIC + bytes([FIRST, COUNT, GLYPH_HW, GLYPH_H]) + clut + present + pixels


# --- match archives (B:\MATCH\NNN.ARC) ---

def arc_entries(arc: bytes) -> list[bytes]:
    """The TIMs of a match archive: u32 offsets (count = first offset / 4), the last one being
    the end of the file (the VS loader walks it too; see docs/re/text-engine.md 7.11)."""
    first = struct.unpack_from("<I", arc, 0)[0]
    assert first % 4 == 0 and 8 <= first < len(arc), "not a match archive"
    offs = list(struct.unpack_from(f"<{first // 4}I", arc, 0))
    assert offs[-1] == len(arc) and offs == sorted(offs), "match archive: bad offset table"
    return [arc[a:b] for a, b in zip(offs, offs[1:])]


def write_arc(entries: list[bytes]) -> bytes:
    """A match archive from its TIMs (offset table + end-of-file entry)."""
    head = 4 * (len(entries) + 1)
    offs, pos = [], head
    for e in entries:
        offs.append(pos)
        pos += len(e)
    offs.append(pos)
    return struct.pack(f"<{len(offs)}I", *offs) + b"".join(entries)


def _tim_rects(tim: bytes) -> tuple[tuple[int, int, int, int] | None, tuple[int, int, int, int]]:
    flags = struct.unpack_from("<I", tim, 4)[0]
    p, clut = 8, None
    if flags & 8:
        ln, *clut = struct.unpack_from("<IHHHH", tim, p)
        clut = tuple(clut)
        p += ln
    _, *img = struct.unpack_from("<IHHHH", tim, p)
    return clut, tuple(img)


# The name picture: 4 bpp at VRAM (704, 480), CLUT (752, 472), JP 32 halfwords wide, US 40..64.
NAME_POS = (704, 480)
NAME_CLUT = (752, 472)


def graft_match_name(jp: bytes, us: bytes) -> bytes | None:
    """The JP archive with its name picture (last TIM) taken from the US archive, or None when
    either archive does not have the expected layout (then the JP file is kept)."""
    try:
        je, ue = arc_entries(jp), arc_entries(us)
    except (AssertionError, struct.error):
        return None
    if len(je) != len(ue) or not je:
        return None
    for tim in (je[-1], ue[-1]):
        clut, img = _tim_rects(tim)
        if clut is None or clut[:2] != NAME_CLUT or img[:2] != NAME_POS or img[2] > 64 or img[3] != 32:
            return None
    return write_arc(je[:-1] + [ue[-1]])


def drv_files(drv: bytes) -> dict[str, bytes]:
    """{path: bytes} of a DRV archive (entries past the end of the archive are left out)."""
    files, _ = _drv.read_toc(drv)
    return {e.path: drv[e.offset:e.offset + e.size] for e in files if e.offset + e.size <= len(drv)}


def write_assets(jp_b: bytes, us_b: bytes, out: Path) -> str:
    """en_bigfont.bin + files/B/MATCH/*.ARC from the two B.DRV archives; a summary line.

    Stale archives from an earlier run (a JP/US pair that no longer grafts) are removed.
    """
    jp_files, us_files = drv_files(jp_b), drv_files(us_b)
    font = build_bigfont(us_files["FONT.ARC"])
    (out / "en_bigfont.bin").write_bytes(font)
    mdir = out / "files" / "B" / "MATCH"
    written: set[str] = set()
    for name in sorted(jp_files):
        if not name.startswith("MATCH/") or not name.endswith(".ARC") or name not in us_files:
            continue
        arc = graft_match_name(jp_files[name], us_files[name])
        if arc is None:
            continue
        mdir.mkdir(parents=True, exist_ok=True)
        base = name.split("/", 1)[1]
        (mdir / base).write_bytes(arc)
        written.add(base)
    for stale in mdir.glob("*.ARC") if mdir.exists() else []:
        if stale.name not in written:
            stale.unlink()
    return (f"bigfont: {len(font)} B -> en_bigfont.bin, {len(written)} match archives with the US "
            f"name picture -> files/B/MATCH/")
