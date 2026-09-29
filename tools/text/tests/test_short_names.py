"""Data-free tests for short_names.py: CDD name listing, config parsing and checks, the
runtime file, and the repo's own short-names.tsv format."""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import short_names  # noqa: E402

REPO = Path(__file__).resolve().parents[3]


def make_us_cdd(names: dict[int, bytes]) -> bytes:
    """A US CARD2.CDD shape (191 Digimon, 102 items, 8 options) with the given names."""
    counts = (191, 102, 8)
    out = bytearray(b"0ACD" + struct.pack("<HBB", *counts))
    number = 0
    for count, stride in zip(counts, (0x13C, 0xE2, 0x70)):
        for _ in range(count):
            rec = bytearray(stride)
            name = names.get(number, f"Card{number}".encode())
            rec[3:3 + len(name)] = name
            out += rec
            number += 1
    return bytes(out)


class CardNames(unittest.TestCase):
    def test_numbers_span_the_three_tables(self):
        cdd = make_us_cdd({0: b"Imperialdramon", 191: b"Mega Def. Disk *b0", 300: b"Last Option"})
        names = short_names.us_card_names(cdd)
        self.assertEqual(len(names), 301)
        self.assertEqual(names[0], b"Imperialdramon")
        self.assertEqual(names[191], b"Mega Def. Disk *b0")
        self.assertEqual(names[300], b"Last Option")

    def test_full_20_letter_name_without_nul(self):
        cdd = make_us_cdd({5: b"ArmorCrush Digivolve"})  # 20 bytes, the NUL is the 21st
        self.assertEqual(short_names.us_card_names(cdd)[5], b"ArmorCrush Digivolve")


class Build(unittest.TestCase):
    names = [b"RealMetalGreymon", b"Agumon", b"Mega Def. Disk *b0"]

    def test_pairs(self):
        text = "# comment\n\n0\tR.MetalGreymon\tRealMetalGreymon\n2\tMega D.Disk *b0\n"
        pairs, problems = short_names.build(text, self.names)
        self.assertEqual(problems, [])
        self.assertEqual(pairs, [(b"RealMetalGreymon", b"R.MetalGreymon"), (b"Mega Def. Disk *b0", b"Mega D.Disk *b0")])

    def test_problems(self):
        text = "\n".join([
            "0\tR.MetalGreymon\tRealMetalGreymonX",  # check column differs
            "7\tNope",                                # no such card
            "1\tAgu",
            "1\tAgu again",                           # twice
            "2\t" + "x" * 21,                         # longer than the slot
            "x\tbad",                                 # not a number
            "1",                                      # no short name
        ])
        pairs, problems = short_names.build(text, self.names)
        self.assertEqual(pairs, [(b"Agumon", b"Agu")])
        self.assertEqual(len(problems), 6, problems)

    def test_write(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = Path(d) / "short-names.tsv"
            cfg.write_text("74\tHrcKabuterimon\tHerculesKabuterimon\n", encoding="utf-8")
            cdd = make_us_cdd({74: b"HerculesKabuterimon"})
            n, problems = short_names.write(cfg, cdd, Path(d))
            self.assertEqual((n, problems), (1, []))
            self.assertEqual((Path(d) / "en_short_names.txt").read_bytes(), b"HerculesKabuterimon\tHrcKabuterimon\n")
            n, problems = short_names.write(Path(d) / "missing.tsv", cdd, Path(d))
            self.assertEqual((n, problems), (0, []))
            self.assertEqual((Path(d) / "en_short_names.txt").read_bytes(), b"")


class RepoConfig(unittest.TestCase):
    def test_repo_file_parses(self):
        text = (REPO / "config" / "SLPS-03101" / "text" / "short-names.tsv").read_text(encoding="utf-8")
        rows, problems = short_names.parse(text)
        self.assertEqual(problems, [])
        self.assertTrue(rows)
        for number, short, full in rows:
            self.assertLess(number, 301)
            self.assertIsNotNone(full, f"card {number}: give the full name for review")
            self.assertLess(len(short), len(full))
            self.assertLessEqual(len(short.encode("ascii")), short_names.NAME_MAX)


if __name__ == "__main__":
    unittest.main()
