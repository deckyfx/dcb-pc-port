"""tools/text/catalog.py: catalog parsing, runs, slot placeholders, the port's own entries."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import catalog  # noqa: E402

SLOT = "スロット".encode("cp932")


def blob(*strings: bytes) -> bytes:
    """Strings NUL-terminated and padded to 4 bytes, like the overlays' data."""
    out = b""
    for s in strings:
        s += b"\0"
        out += s + b"\0" * (-len(s) % 4)
    return out


class TestCatalog(unittest.TestCase):
    def test_parse(self) -> None:
        rows = catalog.parse("# c\npair EXE:828 EXE:828\nrun OPENSEG:0 OPENSEG:10 2  # two\npair A:4 -\n")
        self.assertEqual(rows, [("EXE:828", "EXE:828", 1), ("OPENSEG:0", "OPENSEG:10", 2), ("A:4", None, 1)])
        with self.assertRaises(ValueError):
            catalog.parse("pear EXE:1 EXE:2")

    def test_build_run_placeholders_and_own(self) -> None:
        jp = blob(SLOT + b"S\x82\xcc", "はい".encode("cp932"), "%3d時間".encode("cp932"))
        us = blob(b"*s0MEMORY CARD slot *S.", b"Yes")
        text = "run SEG:0 SEG:0 2\npair SEG:%x -\n" % len(blob(SLOT + b"S\x82\xcc", "はい".encode("cp932")))
        source, en, problems = catalog.build(text, lambda n: jp, lambda n: us, {"SEG:14": b"%3dh"})
        self.assertEqual(problems, [])
        self.assertEqual(source[0], ("SEG:0", SLOT + b"%c\x82\xcc"))
        self.assertEqual(en[0], ("SEG:0", b"*s0MEMORY CARD slot %c."))
        self.assertEqual(en[1], ("SEG:c", b"Yes"))
        self.assertEqual(en[2], ("SEG:14", b"%3dh"))

    def test_own_entry_inside_a_run_keeps_the_us_side_in_step(self) -> None:
        jp = blob(b"a", b"b", b"c")
        us = blob(b"A", b"B", b"C")
        _, en, _ = catalog.build("run SEG:0 SEG:0 3\n", lambda n: jp, lambda n: us, {"SEG:4": b"own"})
        self.assertEqual(en, [("SEG:0", b"A"), ("SEG:4", b"own"), ("SEG:8", b"C")])

    def test_missing_english_is_reported(self) -> None:
        _, en, problems = catalog.build("pair SEG:0 -\n", lambda n: blob(b"x"), lambda n: b"", {})
        self.assertEqual(en, [])
        self.assertEqual(len(problems), 1)

    def test_card_count_over_question_marks(self) -> None:
        # The game writes " 5" / "12" over "??" before 枚; other "?" stay literal.
        raw = "??枚の修復に成功\nご覧になりますか？".encode("cp932")
        self.assertEqual(catalog.jp_template(raw), "%2d枚の修復に成功\nご覧になりますか？".encode("cp932"))
        self.assertEqual(catalog.jp_template(b"??"), b"??")

    def test_banner_player_name(self) -> None:
        # The battle banner writes a player's name over "P0"/"P1" (US "*P0"/"*P1").
        self.assertEqual(catalog.jp_template(b"\x90\xed\x93\xac P1\x82\xcc"), b"\x90\xed\x93\xac %s\x82\xcc")
        raw = b"P0\x82\xcc xP1\x82\xcc P2\x82\xcc P0"
        self.assertEqual(catalog.jp_template(raw), raw)
        self.assertEqual(catalog.us_template(b"Battle: *P0's *P1 *P2"), b"Battle: %s's %s *P2")

    def test_escape(self) -> None:
        self.assertEqual(catalog.escape(b"a\nb\\c\td"), b"a\\nb\\\\c\\td")


if __name__ == "__main__":
    unittest.main()
