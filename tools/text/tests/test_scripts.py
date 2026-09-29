"""tools/text/scripts.py: grafting the tutorial / Fusion Shop scripts (synthetic MSD, no game data)."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import msd  # noqa: E402
import scripts  # noqa: E402

UNIT = scripts.SCRIPTS[1]      # C:\EVENT\UNIT00.MSD: show text 0x0A cmd 0, page break cmd 11
TUTORIAL = scripts.SCRIPTS[0]  # B:\BETA.MSD: show text 0x0A cmd 0, pad registers 4-7


def text(reg: int, s: bytes) -> bytes:
    s += b"\0"
    rec = struct.pack("<HHH", 8, reg, len(s)) + s
    return rec + b"\0" * (-len(rec) % 4)


def cmd(op: int, c: int, *args: int) -> bytes:
    return struct.pack("<HH", op, c) + b"".join(struct.pack("<HH", 0, a) for a in args)


def jump(label: int, target: int) -> bytes:
    return struct.pack("<HHI", 5, label, target)


def button(reg: int) -> bytes:
    """op 9: skip unless pad register `reg` is 1 (the tutorial's button tests)."""
    return struct.pack("<HHHHI", 9, reg, 3, 0, 1)


def script(*records: bytes) -> bytes:
    body = b"".join(records)
    return b"MSCD" + struct.pack("<III", 3, 16 + len(body), 0) + body


def texts(data: bytes) -> list[bytes]:
    return [r.text.split(b"\0", 1)[0] for r in msd.walk(data) if r.op == msd.TEXT]


class TestSameProgram(unittest.TestCase):
    def test_unit_pages_may_be_reflowed(self) -> None:
        """The US adds lines and whole pages (text, cmd 0, cmd 11) in other places than the JP."""
        page = cmd(0x0A, 0x0B)
        jp = script(text(4, b"\x82\xa0"), cmd(0x0A, 0), text(4, b"\x82\xa2"), cmd(0x0A, 0), page,
                    cmd(0x0B, 5, 9), text(4, b"\x82\xa4"), cmd(0x0A, 0), page)
        us = script(text(4, b"A"), cmd(0x0A, 0), page, text(4, b"B"), cmd(0x0A, 0), text(4, b"C"),
                    cmd(0x0A, 0), page, cmd(0x0B, 5, 9), page, text(4, b"D"), cmd(0x0A, 0), page)
        self.assertEqual(msd.same_program(jp, us, UNIT.show_text), (True, ""))
        # The city host's show-text commands are others: there cmd 0 / 11 are program.
        self.assertFalse(msd.same_program(jp, us)[0])

    def test_program_change_is_refused(self) -> None:
        jp = script(text(4, b"x"), cmd(0x0A, 0), cmd(0x0B, 5, 9))
        us = script(text(4, b"x"), cmd(0x0A, 0), cmd(0x0B, 5, 8))
        ok, why = msd.same_program(jp, us, UNIT.show_text)
        self.assertFalse(ok)
        self.assertIn("0xb", why)
        ok, _ = msd.same_program(jp, script(text(4, b"x"), cmd(0x0A, 0)), UNIT.show_text)
        self.assertFalse(ok)

    def test_text_registers_must_match(self) -> None:
        jp = script(text(4, b"x"), cmd(0x0A, 0))
        us = script(text(5, b"x"), cmd(0x0A, 0))
        self.assertFalse(msd.same_program(jp, us, UNIT.show_text)[0])

    def test_button_tests_only_with_pad_registers(self) -> None:
        jp = script(button(4), jump(3, 0x40), button(7), jump(4, 0x40))
        us = script(button(7), jump(3, 0x40), button(6), jump(4, 0x40))
        self.assertFalse(msd.same_program(jp, us, TUTORIAL.show_text)[0])
        self.assertTrue(msd.same_program(jp, us, TUTORIAL.show_text, TUTORIAL.button_regs)[0])
        # ... but the jumps after them must still match.
        us_other = script(button(7), jump(4, 0x40), button(6), jump(3, 0x40))
        self.assertFalse(msd.same_program(jp, us_other, TUTORIAL.show_text, TUTORIAL.button_regs)[0])


class TestGraft(unittest.TestCase):
    def test_tutorial_takes_jp_tests_and_icons(self) -> None:
        jp = script(text(0, b"b0\x82\xf0\x89\x9f\x82\xb5\x82\xc4"), cmd(0x0A, 0), button(7), jump(3, 0x40),
                    button(4), jump(4, 0x50),
                    text(0, b"b1\x82\xcd\x81\x41b0\x82\xe6\x82\xe8"), cmd(0x0A, 0),
                    text(0, b"\x82\xda\x82\xad\x82\xcdb0"), cmd(0x0A, 0), button(4), jump(5, 0x60))
        us = script(text(0, b"Press *b2 to use it, *b1 to discard."), cmd(0x0A, 0), button(6), jump(3, 0x40),
                    button(7), jump(4, 0x50),
                    text(0, b"*b1 has less Attack Power than *b0."), cmd(0x0A, 0),
                    text(0, b"I chose *b0."), cmd(0x0A, 0), button(4), jump(5, 0x60))
        out, why, notes = scripts.graft(jp, us, TUTORIAL)
        self.assertEqual(why, "")
        self.assertEqual(len(out), len(us))
        self.assertEqual(texts(out), [b"Press *b0 to use it, *b2 to discard.",   # confirm / redraw
                                      b"*b1 has less Attack Power than *b0.",    # attack buttons
                                      b"I chose *b0."])                          # same as the JP
        regs = [struct.unpack_from("<H", r.raw, 2)[0] for r in msd.walk(out) if r.op == 9]
        self.assertEqual(regs, [7, 4, 4])
        self.assertIn("2 button tests take the JP buttons", notes)

    def test_unit_quotes_in_place(self) -> None:
        jp = script(text(4, b"\x82\xa0"), cmd(0x0A, 0))
        us = script(text(4, b'called \\0x22Partner Fusion.\\0x22'), cmd(0x0A, 0))
        out, why, _ = scripts.graft(jp, us, UNIT)
        self.assertEqual(why, "")
        self.assertEqual(len(out), len(us))
        self.assertEqual(texts(out), [b'called "Partner Fusion."'])
        self.assertEqual([r.offset for r in msd.walk(out)], [r.offset for r in msd.walk(us)])

    def test_refused_script_is_not_written(self) -> None:
        jp = script(text(4, b"x"), cmd(0x0A, 0), cmd(0x0B, 5, 9))
        us = script(text(4, b"x"), cmd(0x0A, 0), cmd(0x0B, 5, 8))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            stale = out / "files" / "C" / "EVENT" / "UNIT00.MSD"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old")
            written = scripts.write_all(lambda d, p: jp, lambda d, p: us, out)
            self.assertEqual(written, 0)
            self.assertFalse(stale.exists())

    def test_written_under_the_loose_file_key(self) -> None:
        jp = script(text(4, b"x"), cmd(0x0A, 0))
        us = script(text(4, b"y"), cmd(0x0A, 0))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            written = scripts.write_all(lambda d, p: jp, lambda d, p: us, out)
            # BETA.MSD has no button tests here, so all four pass.
            self.assertEqual(written, 4)
            self.assertTrue((out / "files" / "B" / "BETA.MSD").is_file())
            for n in range(3):
                self.assertTrue((out / "files" / "C" / "EVENT" / f"UNIT0{n}.MSD").is_file())


if __name__ == "__main__":
    unittest.main()
