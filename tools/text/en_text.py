#!/usr/bin/env python3
"""Build English text assets for the JP build from the player's own dumps.

Reads extracted/SLPS-03101 (JP) and extracted/SLUS-01328 (US), writes into
gitignored assets/SLPS-03101/ (never into git):

  en_font.bin    US ASCII font rows (TIM pixel rows 48..223 of B:SYSTEM.TIM)
                 + 96-byte width table (US EXE VA 0x8006df9c)
  files/B/CARD2.CDD   JP file with US names, attack names, effect text
                 (effect lines re-slotted 21 -> 19 bytes; overlong lines
                 listed in en_text_report.txt for hand-shortening)
  files/C/AREAnn.PAK  the 12 city PAKs with the US city script (MSD chunk);
                 the image chunk stays JP
  files/B/DECK2.DEK   same graft for deck/owner names; a deck name too long
                 for its 13-byte slot is stored as 11 letters + a tag byte
  text/source.tsv, text/en.tsv
                 the text catalog (config/SLPS-03101/text/catalog.txt): JP
                 templates the renderer matches, and their English
  en_names.txt   those long names: "<11 letters>\t<tag>\t<full name>" per
                 line; the renderer (src/game/overrides/text.cpp) draws the
                 full name wherever the key shows up

Usage: en_text.py --jp <extracted/SLPS-03101> --us <extracted/SLUS-01328>
                  --out <assets/SLPS-03101>
Nothing copyrighted is printed; only counts, offsets and the overlong list.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "disc"))

# --- container readers (reuse tools/disc/drv_unpack.py; TIM walk is local) ---

import drv_unpack as _drv  # noqa: E402

import catalog as _catalog  # noqa: E402  (tools/text/catalog.py)
import msd as _msd  # noqa: E402  (tools/text/msd.py)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "assets"))
import dcb_containers as _containers  # noqa: E402  (PAK reader/writer)

REPO = Path(__file__).resolve().parents[2]


def drv_file(drv: bytes, want: str) -> bytes:
    files, _ = _drv.read_toc(drv)
    for e in files:
        if e.path == want:
            return drv[e.offset:e.offset + e.size]
    raise KeyError(f"{want} not in DRV")


def tim_blocks(tim: bytes) -> list[tuple[int, int, int, int, int, bytes]]:
    """(off, x, y, w, h, payload) per block of a TIM file (CLUT block first)."""
    assert tim[:4] == b"\x10\x00\x00\x00", "not a TIM"
    out = []
    off = 8
    if struct.unpack_from("<I", tim, 4)[0] & 8:
        ln = struct.unpack_from("<I", tim, off)[0]
        x, y, w, h = struct.unpack_from("<HHHH", tim, off + 4)
        out.append((off, x, y, w, h, tim[off + 12:off + ln]))
        off += ln
    ln = struct.unpack_from("<I", tim, off)[0]
    x, y, w, h = struct.unpack_from("<HHHH", tim, off + 4)
    out.append((off, x, y, w, h, tim[off + 12:off + ln]))
    return out


# --- font rows + width table (docs/re/text-engine.md section 4.2, option B inputs) ---

FONT_FIRST_ROW = 48
FONT_LAST_ROW = 223  # inclusive; 176 rows starting at the first cell row (US draw v = row*12 + 0x30)
WIDTH_VA = 0x8006DF9C  # US draw: lui 0x8007; addiu -8292 -> 0x8006DF9C; [0x20]=0x04 (space adv 4)
WIDTH_COUNT = 96  # c - 0x20 for 0x20..0x7F


def build_font(us_tim: bytes, us_exe_text: bytes, us_t_addr: int) -> bytes:
    """en_font.bin: u16 rows_first, u16 rows_last, pixel rows, width table."""
    blocks = tim_blocks(us_tim)
    # Image block = the large payload (CLUT block is 16x16 = 512 B payload).
    img = max(blocks, key=lambda b: len(b[5]))
    _, _, _, w_words, h, payload = img
    assert w_words == 64 and h == 256, f"unexpected SYSTEM.TIM image {w_words}x{h}"
    rows = b"".join(payload[r * 128:(r + 1) * 128] for r in range(FONT_FIRST_ROW, FONT_LAST_ROW + 1))
    woff = WIDTH_VA - us_t_addr
    widths = us_exe_text[woff:woff + WIDTH_COUNT]
    assert len(widths) == WIDTH_COUNT, "width table runs past text end"
    return struct.pack("<HH", FONT_FIRST_ROW, FONT_LAST_ROW) + rows + bytes(widths)


# --- card / deck graft (HYBRID section 6) ---

def graft_cdd(jp: bytes, us: bytes, report: list[str]) -> tuple[bytes, dict]:
    assert jp[:4] == b"ADCD" and us[:4] == b"0ACD"
    n_dig, n_item, n_opt = struct.unpack_from("<HBB", jp, 4)
    assert (n_dig, n_item, n_opt) == (191, 102, 8)
    jp_stride, us_stride = 0x134, 0x13C
    out = bytearray(jp)
    stats = {"names": 0, "attacks": 0, "effects_fit": 0, "effects_long": []}
    for i in range(n_dig):
        jo, uo = 8 + i * jp_stride, 8 + i * us_stride
        # name[21] at +3
        out[jo + 3:jo + 3 + 21] = us[uo + 3:uo + 3 + 21]
        stats["names"] += 1
        # attack names[22] at +26/+42/+5E
        for ao in (0x26, 0x42, 0x5E):
            out[jo + ao:jo + ao + 22] = us[uo + ao:uo + ao + 22]
            stats["attacks"] += 1
        # effect text: 4 lines US 21B -> JP 19B slots at +E7 (incl. NUL)
        for li in range(4):
            src = us[uo + 0xE7 + li * 21:uo + 0xE7 + li * 21 + 21].split(b"\0", 1)[0]
            if len(src) + 1 > 19:
                stats["effects_long"].append((f"digimon {i} line {li}", src))
                report.append(f"digimon {i} line {li} ({len(src)} chars): {src!r}")
            out[jo + 0xE7 + li * 19:jo + 0xE7 + li * 19 + 19] = src[:18].ljust(19, b"\0")
            stats["effects_fit"] += 1
    # items: stride JP 0xDA / US 0xE2, name +3[21], effects 4x19 at +8D/+21
    jbase = 8 + n_dig * jp_stride
    ubase = 8 + n_dig * us_stride
    for i in range(n_item):
        jo, uo = jbase + i * 0xDA, ubase + i * 0xE2
        out[jo + 3:jo + 3 + 21] = us[uo + 3:uo + 3 + 21]
        stats["names"] += 1
        for li in range(4):
            src = us[uo + 0x8D + li * 21:uo + 0x8D + li * 21 + 21].split(b"\0", 1)[0]
            if len(src) + 1 > 19:
                stats["effects_long"].append((f"item {i} line {li}", src))
                report.append(f"item {i} line {li} ({len(src)} chars): {src!r}")
            out[jo + 0x8D + li * 19:jo + 0x8D + li * 19 + 19] = src[:18].ljust(19, b"\0")
            stats["effects_fit"] += 1
    # options: stride JP 0x68 / US 0x70, name +3[21], effects 4x19 at +1B/+21
    jbase += n_item * 0xDA
    ubase += n_item * 0xE2
    for i in range(n_opt):
        jo, uo = jbase + i * 0x68, ubase + i * 0x70
        out[jo + 3:jo + 3 + 21] = us[uo + 3:uo + 3 + 21]
        stats["names"] += 1
        for li in range(4):
            src = us[uo + 0x1B + li * 21:uo + 0x1B + li * 21 + 21].split(b"\0", 1)[0]
            if len(src) + 1 > 19:
                stats["effects_long"].append((f"option {i} line {li}", src))
                report.append(f"option {i} line {li} ({len(src)} chars): {src!r}")
            out[jo + 0x1B + li * 19:jo + 0x1B + li * 19 + 19] = src[:18].ljust(19, b"\0")
            stats["effects_fit"] += 1
    return bytes(out), stats


def graft_dek(jp: bytes, us: bytes, report: list[str]) -> tuple[bytes, dict]:
    """DEK2.DEK: 159 records, JP stride 104 / US 110.

    Card-ID lists (+0..+59) are byte-identical. Field boundaries measured from
    both dumps: deck name is JP 13 B / US 19 B at +60 (owner starts at JP +73,
    US +79), owner is 21 B on both sides (tail: JP +94 / US +100). The tail and
    the `\xa0\x82\xe9` SJIS leftovers inside the name fields stay JP.
    """
    assert jp[:4] == b"20KD" and us[:4] == b"30KD", "bad DEK magic"
    assert len(jp) == 8 + 159 * 104 and len(us) == 8 + 159 * 110, "bad DEK size"
    out = bytearray(jp)
    stats = {"names": 0, "owners": 0, "overlong": [], "long_names": []}
    tags: dict[bytes, int] = {}    # prefix -> tags used
    tag_of: dict[bytes, int] = {}  # full name -> its tag
    for i in range(159):
        jo, uo = 8 + i * 104, 8 + i * 110
        assert bytes(out[jo:jo + 60]) == us[uo:uo + 60], f"deck {i}: card list differs"
        # deck: US field [60, 79) -> JP field [60, 73)
        src = us[uo + 60:uo + 79].split(b"\0", 1)[0]
        if len(src) + 1 > 13:
            # Too long for the slot: the first 11 letters and a tag byte (1, 2... per shared
            # prefix) key the full name in en_names.txt; the renderer draws the full name.
            prefix = src[:11]
            if src not in tag_of:
                tags[prefix] = tags.get(prefix, 0) + 1
                tag_of[src] = tags[prefix]
                stats["long_names"].append((prefix, tag_of[src], src))
            tag = tag_of[src]
            assert tag <= 9, f"deck {i}: more than 9 long names share {prefix!r}"
            out[jo + 60:jo + 73] = (prefix + bytes([tag])).ljust(13, b"\0")
        else:
            out[jo + 60:jo + 73] = src.ljust(13, b"\0")
        stats["names"] += 1
        # owner: US field [79, 100) -> JP field [73, 94)
        src = us[uo + 79:uo + 100].split(b"\0", 1)[0]
        if len(src) + 1 > 21:
            stats["overlong"].append((f"deck {i} owner", src))
            report.append(f"deck {i} owner ({len(src)} chars): {src!r}")
        out[jo + 73:jo + 94] = src[:20].ljust(21, b"\0")
        stats["owners"] += 1
    return bytes(out), stats


def graft_city_script(jp_pak: bytes, us_pak: bytes) -> tuple[bytes | None, str]:
    """The JP city PAK with the US city script (chunk kind 2) in place of the JP one.

    Only when the two scripts are the same program apart from text (msd.same_program); the other
    chunks (the kind-5 image set) stay JP. (None, reason) otherwise.
    """
    jp_chunks = _containers.read_pak(jp_pak)
    us_scripts = {c.id: c.data for c in _containers.read_pak(us_pak) if c.kind == 2}
    out = []
    for c in jp_chunks:
        if c.kind == 2:
            if c.id not in us_scripts:
                return None, f"no US script chunk {c.id:#x}"
            ok, why = _msd.same_program(c.data, us_scripts[c.id])
            if not ok:
                return None, why
            c = _containers.Chunk(c.kind, c.id, c.offset, us_scripts[c.id])
        out.append(c)
    return _containers.write_pak(out), ""


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--jp", required=True, help="extracted/SLPS-03101")
    ap.add_argument("--us", required=True, help="extracted/SLUS-01328")
    ap.add_argument("--out", required=True, help="assets/SLPS-03101")
    args = ap.parse_args(argv)

    jp_fs = Path(args.jp) / "fs"
    us_fs = Path(args.us) / "fs"
    out = Path(args.out)
    (out / "files" / "B").mkdir(parents=True, exist_ok=True)

    jp_b = (jp_fs / "B.DRV").read_bytes()
    us_b = (us_fs / "B.DRV").read_bytes()

    # 1. font rows + width table
    us_tim = drv_file(us_b, "SYSTEM.TIM")
    us_exe = (Path(args.us) / "exe" / "boot.exe").read_bytes()
    us_text = us_exe[0x800:]
    font_blob = build_font(us_tim, us_text, 0x80010000)
    (out / "en_font.bin").write_bytes(font_blob)
    print(f"font: rows {FONT_FIRST_ROW}..{FONT_LAST_ROW}, widths {WIDTH_COUNT}B -> en_font.bin ({len(font_blob)} B)")

    # 2. card + deck graft
    report: list[str] = []
    jp_cdd = drv_file(jp_b, "CARD2.CDD")
    us_cdd = drv_file(us_b, "CARD2.CDD")
    grafted, stats = graft_cdd(jp_cdd, us_cdd, report)
    (out / "files" / "B" / "CARD2.CDD").write_bytes(grafted)
    print(f"cdd: {stats['names']} names, {stats['attacks']} attacks, "
          f"{stats['effects_fit']} effect lines, {len(stats['effects_long'])} overlong")
    diffs = []
    for c in (5, 13, 128):
        a = jp_cdd[8 + c * 0x134 + 0x8E]
        b = us_cdd[8 + c * 0x13C + 0x8E]
        diffs.append(f"card {c} +0x8E: JP {a:#04x} US {b:#04x} (kept JP)")
    print("\n".join(diffs))

    jp_dek = drv_file(jp_b, "DECK2.DEK")
    us_dek = drv_file(us_b, "DECK2.DEK")
    grafted_dek, dek_stats = graft_dek(jp_dek, us_dek, report)
    (out / "files" / "B" / "DECK2.DEK").write_bytes(grafted_dek)
    print(f"dek: {dek_stats['names']} deck names, {dek_stats['owners']} owners, "
          f"{len(dek_stats['long_names'])} long names -> en_names.txt, "
          f"{len(dek_stats['overlong'])} overlong")
    (out / "en_names.txt").write_bytes(b"".join(
        prefix + b"\t" + str(tag).encode() + b"\t" + full + b"\n"
        for prefix, tag, full in dek_stats["long_names"]))

    (out / "en_text_report.txt").write_text(
        "Overlong strings (US text + NUL > JP slot; shorten by hand):\n" +
        "".join(f"  {line}\n" for line in report) + "\n" +
        "Balance bytes kept JP:\n" + "".join(f"  {line}\n" for line in diffs) + "\n")
    print(f"report: {len(report)} overlong strings -> en_text_report.txt")

    # 3. city scripts: C:\AREAnn.PAK with the US script chunk (dialogue, city messages)
    jp_c = (jp_fs / "C.DRV").read_bytes()
    us_c = (us_fs / "C.DRV").read_bytes()
    (out / "files" / "C").mkdir(parents=True, exist_ok=True)
    grafted_cities = 0
    for n in range(12):
        name = f"AREA{n:02d}.PAK"
        pak, why = graft_city_script(drv_file(jp_c, name), drv_file(us_c, name))
        if pak is None:
            print(f"city {name}: kept JP ({why})")
            continue
        (out / "files" / "C" / name).write_bytes(pak)
        grafted_cities += 1
    print(f"city scripts: {grafted_cities}/12 AREAnn.PAK with the US script -> files/C/")

    # 4. text catalog (config/SLPS-03101/text/catalog.txt): source.tsv + en.tsv
    jp_p = (jp_fs / "P.DRV").read_bytes()
    us_p = (us_fs / "P.DRV").read_bytes()
    jp_exe = (Path(args.jp) / "exe" / "boot.exe").read_bytes()

    def loader(exe: bytes, p_drv: bytes):
        return lambda name: exe if name == "EXE" else drv_file(p_drv, name + ".BIN")

    # Every catalog*.txt / en*.tsv in the folder (one file per area keeps edits apart).
    cat_dir = REPO / "config" / "SLPS-03101" / "text"
    own: dict[str, bytes] = {}
    for f in sorted(cat_dir.glob("en*.tsv")):
        own.update(_catalog.read_own(f))
    catalog_text = "".join(f.read_text(encoding="utf-8") + "\n" for f in sorted(cat_dir.glob("catalog*.txt")))
    source, en, problems = _catalog.build(catalog_text, loader(jp_exe, jp_p), loader(us_exe, us_p), own)
    seen: set[str] = set()
    for ident, _ in source:
        if ident in seen:
            problems.append(f"{ident}: listed twice")
        seen.add(ident)
    _catalog.write_rows(out / "text" / "source.tsv", source)
    _catalog.write_rows(out / "text" / "en.tsv", en, escaped=set(own))
    print(f"catalog: {len(source)} strings, {len(en)} English -> text/source.tsv, text/en.tsv")
    for line in problems:
        print(f"  catalog: {line}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
