"""tools/text/msd.py: walking MSD records, and when a US script may replace a JP one."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import msd  # noqa: E402


def text(reg: int, s: bytes) -> bytes:
    s += b"\0"
    rec = struct.pack("<HHH", 8, reg, len(s)) + s
    return rec + b"\0" * (-len(rec) % 4)


def cmd(op: int, c: int, *args: int) -> bytes:
    return struct.pack("<HH", op, c) + b"".join(struct.pack("<HH", 0, a) for a in args)


def script(*records: bytes) -> bytes:
    body = b"".join(records)
    return b"MSCD" + struct.pack("<III", 3, 16 + len(body), 0) + body


class TestMsd(unittest.TestCase):
    def test_walk(self) -> None:
        recs = msd.walk(script(text(4, b"abc"), cmd(0x0A, 4), cmd(0x0C, 9, 1, 2), struct.pack("<HHi", 5, 0, 16)))
        self.assertEqual([r.op for r in recs], [8, 0x0A, 0x0C, 5])
        self.assertEqual(recs[0].text, b"abc\0")
        with self.assertRaises(ValueError):
            msd.walk(script(struct.pack("<HH", 0x42, 0)))

    def test_same_program_allows_reflowed_text(self) -> None:
        jp = script(text(4, b"\x82\xa0"), cmd(0x0A, 4), cmd(0x0C, 9, 1, 2))
        us = script(text(4, b"Hello"), cmd(0x0A, 4), text(4, b"more"), cmd(0x0A, 4), cmd(0x0C, 9, 1, 2))
        self.assertEqual(msd.same_program(jp, us), (True, ""))

    def test_same_program_rejects_other_changes(self) -> None:
        jp = script(text(4, b"x"), cmd(0x0C, 9, 1, 2))
        us = script(text(4, b"x"), cmd(0x0C, 9, 1, 3))
        ok, why = msd.same_program(jp, us)
        self.assertFalse(ok)
        self.assertIn("0xc", why)


class TestCityButtons(unittest.TestCase):
    def test_map_button_takes_the_jp_icon(self) -> None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import en_text  # noqa: E402
        src = script(text(4, b"*c5Push *c7*b1*c5 to go to map."), cmd(0x0A, 4))
        out = en_text.remap_buttons(src)
        self.assertEqual(len(out), len(src))
        self.assertEqual(msd.walk(out)[0].text, b"*c5Push *c7*b2*c5 to go to map.\0")


if __name__ == "__main__":
    unittest.main()
