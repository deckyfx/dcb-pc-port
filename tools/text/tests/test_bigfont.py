"""tools/text/bigfont.py: US big-name font repack and the match-archive name graft (synthetic data)."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bigfont  # noqa: E402


def tim(w: int, h: int, fill: int, clut: bytes = bytes(range(32)), pos=(0, 0), clut_pos=(0, 0)) -> bytes:
    """A 4-bpp TIM with a 16-entry CLUT, w halfwords x h rows of `fill`."""
    px = bytes([fill]) * (w * 2 * h)
    return (struct.pack("<II", 0x10, 8)
            + struct.pack("<IHHHH", 12 + 32, *clut_pos, 16, 1) + clut
            + struct.pack("<IHHHH", 12 + len(px), *pos, w, h) + px)


def font_arc(glyphs: dict[str, int]) -> bytes:
    """FONT.ARC: 92 offsets for 0x20..0x7B; codes without a glyph point at the next glyph (like
    the US file), codes after the last glyph at the end of the file."""
    order = sorted(glyphs)
    tims = [tim(4, 32, glyphs[c]) for c in order]
    base = 92 * 4
    offs = []
    for code in range(0x20, 0x20 + 92):
        nxt = next((i for i, c in enumerate(order) if ord(c) >= code), len(order))
        offs.append(base + nxt * bigfont.TIM_SIZE)
    return struct.pack("<92I", *offs) + b"".join(tims)


class TestFont(unittest.TestCase):
    def test_glyph_owner_is_last_code_of_run(self) -> None:
        arc = font_arc({" ": 0x00, "-": 0x11, "0": 0x22, "A": 0x33, "a": 0x44})
        clut, glyphs = bigfont.parse_font_arc(arc)
        self.assertEqual(clut, bytes(range(32)))
        # '!'..',' share '-'s TIM, '.' and '/' share '0': only the run's last code owns it.
        self.assertEqual(sorted(glyphs), [ord(c) for c in " -0Aa"])
        self.assertEqual(glyphs[ord("A")], bytes([0x33]) * 256)

    def test_build_layout(self) -> None:
        blob = bigfont.build_bigfont(font_arc({" ": 0x00, "Z": 0x5A}))
        self.assertEqual(blob[:8], b"BGF1" + bytes([0x20, 92, 4, 32]))
        present = blob[40:40 + 92]
        self.assertEqual([i for i, p in enumerate(present) if p], [0, ord("Z") - 0x20])
        pixels = blob[40 + 92:]
        self.assertEqual(len(pixels), 92 * 256)
        z = ord("Z") - 0x20
        self.assertEqual(pixels[z * 256:(z + 1) * 256], bytes([0x5A]) * 256)
        self.assertEqual(pixels[256:512], bytes(256))  # '!' has no glyph: zeros

    def test_rejects_other_geometry(self) -> None:
        arc = struct.pack("<92I", *([368] * 92)) + tim(8, 32, 0)[:bigfont.TIM_SIZE]
        with self.assertRaises(AssertionError):
            bigfont.parse_font_arc(arc)


def match_arc(name_w: int, fill: int, extra: int = 2) -> bytes:
    others = [tim(4, 4, i, pos=(400 + i, 0)) for i in range(extra)]
    name = tim(name_w, 32, fill, pos=bigfont.NAME_POS, clut_pos=bigfont.NAME_CLUT)
    return bigfont.write_arc(others + [name])


class TestMatch(unittest.TestCase):
    def test_roundtrip(self) -> None:
        arc = match_arc(32, 1)
        entries = bigfont.arc_entries(arc)
        self.assertEqual(len(entries), 3)
        self.assertEqual(bigfont.write_arc(entries), arc)
        # the offset table ends with the end of the file, as on the disc
        n = struct.unpack_from("<I", arc, 0)[0] // 4
        self.assertEqual(struct.unpack_from(f"<{n}I", arc, 0)[-1], len(arc))

    def test_graft_takes_only_the_name(self) -> None:
        jp, us = match_arc(32, 0x11), match_arc(48, 0x22)
        us_entries = bigfont.arc_entries(us)
        us = bigfont.write_arc([tim(4, 4, 0x77)] * 2 + [us_entries[-1]])  # US art differs too
        out = bigfont.graft_match_name(jp, us)
        je, oe = bigfont.arc_entries(jp), bigfont.arc_entries(out)
        self.assertEqual(oe[:-1], je[:-1])
        self.assertEqual(oe[-1], us_entries[-1])

    def test_graft_refuses_other_layouts(self) -> None:
        jp = match_arc(32, 0)
        no_name = bigfont.write_arc([tim(4, 4, 0)] * 3)  # like MATCH\999.ARC: no name picture
        self.assertIsNone(bigfont.graft_match_name(jp, no_name))
        self.assertIsNone(bigfont.graft_match_name(jp, match_arc(80, 0)))  # wider than 256 px
        self.assertIsNone(bigfont.graft_match_name(jp, match_arc(48, 0, extra=3)))
        self.assertIsNone(bigfont.graft_match_name(b"junk", jp))


class TestWriteAssets(unittest.TestCase):
    def test_writes_font_and_archives(self) -> None:
        jp_files = {"MATCH/004.ARC": match_arc(32, 1), "MATCH/999.ARC": bigfont.write_arc([tim(4, 4, 0)] * 3)}
        us_files = {"MATCH/004.ARC": match_arc(48, 2), "MATCH/999.ARC": bigfont.write_arc([tim(4, 4, 0)] * 3),
                    "FONT.ARC": font_arc({" ": 0, "A": 1})}
        orig = bigfont.drv_files
        try:
            bigfont.drv_files = lambda drv: jp_files if drv == b"jp" else us_files
            with tempfile.TemporaryDirectory() as d:
                out = Path(d)
                stale = out / "files" / "B" / "MATCH" / "007.ARC"
                stale.parent.mkdir(parents=True)
                stale.write_bytes(b"old")
                line = bigfont.write_assets(b"jp", b"us", out)
                self.assertIn("1 match archives", line)
                self.assertTrue((out / "en_bigfont.bin").read_bytes().startswith(b"BGF1"))
                self.assertEqual(sorted(p.name for p in stale.parent.iterdir()), ["004.ARC"])
        finally:
            bigfont.drv_files = orig


if __name__ == "__main__":
    unittest.main()
