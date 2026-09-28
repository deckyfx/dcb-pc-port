"""Data-free tests for en_text.py: font slicing, width table, CDD/DEK graft.

Everything runs against synthetic containers (no game data in git).
The font-row test guards the cell-alignment regression: US cells start at
sheet row 48 (draw formula v = row*12 + 0x30); row 42 slices every glyph
into the bottom half of the previous row plus the top half of the next.
"""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import en_text  # noqa: E402


def make_tim(rows: int = 256, w_words: int = 64) -> bytes:
    """Minimal TIM: 16x16 CLUT block + one image block (SYSTEM.TIM shape)."""
    clut_payload = bytes(512)
    clut = struct.pack("<IHHHH", 12 + 512, 960, 511, 16, 16) + clut_payload
    payload = b"".join(bytes([(r + k) & 0xFF for k in range(128)]) for r in range(rows))
    assert len(payload) == w_words * 2 * rows
    img = struct.pack("<IHHHH", 12 + len(payload), 960, 0, w_words, rows) + payload
    return b"\x10\x00\x00\x00" + struct.pack("<I", 8) + clut + img


def make_cdd(magic: bytes, jp: bool) -> bytes:
    n_dig, n_item, n_opt = 191, 102, 8
    dig, item, opt = (0x134, 0xDA, 0x68) if jp else (0x13C, 0xE2, 0x70)
    size = 8 + n_dig * dig + n_item * item + n_opt * opt
    buf = bytearray([0xD0 if jp else 0x60]) * size
    buf[:4] = magic
    struct.pack_into("<HBB", buf, 4, n_dig, n_item, n_opt)
    return bytes(buf)


def cdd_sections(jp: bool):
    """(record_base, stride, count, name_off, effect_off) per section, offsets
    after the record start."""
    dig, item, opt = (0x134, 0xDA, 0x68) if jp else (0x13C, 0xE2, 0x70)
    n_dig, n_item, n_opt = 191, 102, 8
    return [
        (8, dig, n_dig, 0xE7),
        (8 + n_dig * dig, item, n_item, 0x8D),
        (8 + n_dig * dig + n_item * item, opt, n_opt, 0x1B),
    ]


class TestFont(unittest.TestCase):
    def test_cell_row_constant(self) -> None:
        # US draw: v = row*12 + 0x30 -- cells start at sheet row 48, not 42.
        self.assertEqual(en_text.FONT_FIRST_ROW, 0x30)
        self.assertEqual(en_text.FONT_LAST_ROW - en_text.FONT_FIRST_ROW + 1, 176)

    def test_tim_blocks(self) -> None:
        blocks = en_text.tim_blocks(make_tim())
        self.assertEqual(len(blocks), 2)
        _, x, y, w, h, payload = blocks[0]
        self.assertEqual((x, y, w, h), (960, 511, 16, 16))
        self.assertEqual(len(payload), 512)
        _, _, _, w, h, payload = blocks[1]
        self.assertEqual((w, h), (64, 256))
        self.assertEqual(payload[0:2], bytes([0, 1]))  # row 0

    def test_build_font_rows_and_widths(self) -> None:
        tim = make_tim()
        t_addr = 0x80010000
        woff = en_text.WIDTH_VA - t_addr
        exe = bytearray(woff + en_text.WIDTH_COUNT)
        exe[woff:woff + 96] = bytes(range(96))
        blob = en_text.build_font(tim, bytes(exe), t_addr)
        first, last = struct.unpack_from("<HH", blob, 0)
        self.assertEqual((first, last), (48, 223))
        rows = blob[4:4 + 176 * 128]
        self.assertEqual(len(rows), 176 * 128)
        # row r of the output is sheet row 48 + r of the synthetic TIM
        for r in (0, 1, 47, 175):
            sheet_row = 48 + r
            expect = bytes([(sheet_row + k) & 0xFF for k in range(128)])
            self.assertEqual(rows[r * 128:(r + 1) * 128], expect, f"row {r}")
        self.assertEqual(blob[4 + 176 * 128:], bytes(range(96)))

    def test_build_font_rejects_short_width_table(self) -> None:
        t_addr = 0x80010000
        woff = en_text.WIDTH_VA - t_addr
        with self.assertRaises(AssertionError):
            en_text.build_font(make_tim(), bytes(woff + 10), t_addr)


class TestGraftCdd(unittest.TestCase):
    def setUp(self) -> None:
        self.jp = bytearray(make_cdd(b"ADCD", jp=True))
        self.us = bytearray(make_cdd(b"0ACD", jp=False))
        # every record gets valid names/effects; only the first record per
        # section carries a deliberately overlong effect line
        for sec, (base, stride, n, eff) in enumerate(cdd_sections(jp=False)):
            for i in range(n):
                rec = base + i * stride
                self.us[rec + 3:rec + 24] = f"EN{sec}{i}".encode().ljust(21, b"\0")
                if sec == 0:
                    for k, ao in enumerate((0x26, 0x42, 0x5E)):
                        self.us[rec + ao:rec + ao + 22] = f"ATK{i}{k}".encode().ljust(22, b"\0")
                long_line = b"B" * 19 + b"\0" if i == 0 else b"SHORT\0".ljust(21, b"\0")
                self.us[rec + eff:rec + eff + 21] = b"SHORT\0".ljust(21, b"\0")
                self.us[rec + eff + 21:rec + eff + 42] = long_line   # 19 + NUL > 19
                self.us[rec + eff + 42:rec + eff + 63] = b"C" * 18 + b"\0"  # fits exactly
                self.us[rec + eff + 63:rec + eff + 84] = b"\0" * 21
        self.report: list[str] = []

    def test_names_and_attacks_copied(self) -> None:
        out, stats = en_text.graft_cdd(bytes(self.jp), bytes(self.us), self.report)
        for (jbase, jstride, n, _), (ubase, ustride, _un, _ue) in zip(
            cdd_sections(jp=True), cdd_sections(jp=False)
        ):
            for i in range(n):
                jo, uo = jbase + i * jstride, ubase + i * ustride
                self.assertEqual(out[jo + 3:jo + 24], bytes(self.us[uo + 3:uo + 24]))
        self.assertEqual(stats["names"], 191 + 102 + 8)
        self.assertEqual(stats["attacks"], 191 * 3)

    def test_effect_slots(self) -> None:
        out, stats = en_text.graft_cdd(bytes(self.jp), bytes(self.us), self.report)
        jbase, jstride, _, jeff = cdd_sections(jp=True)[0]
        rec = jbase
        self.assertEqual(out[rec + jeff:rec + jeff + 19], b"SHORT\0".ljust(19, b"\0"))
        self.assertEqual(out[rec + jeff + 19:rec + jeff + 38], b"B" * 18 + b"\0")
        self.assertEqual(out[rec + jeff + 38:rec + jeff + 57], b"C" * 18 + b"\0")
        self.assertEqual(out[rec + jeff + 57:rec + jeff + 76], b"\0" * 19)
        self.assertEqual(len(stats["effects_long"]), 3)  # one per section (record 0)
        self.assertIn("19 chars", self.report[0])
        # untouched gaps keep the JP filler
        self.assertEqual(out[rec + 0x74:rec + jeff], bytes(self.jp[rec + 0x74:rec + jeff]))
        self.assertEqual(out[rec + 0x133:rec + 0x134], bytes(self.jp[rec + 0x133:rec + 0x134]))


class TestGraftDek(unittest.TestCase):
    JP_STRIDE, US_STRIDE = 104, 110
    N = 159

    def make(self, deck: bytes = b"Deck%03d", owner: bytes = b"Owner%03d") -> tuple[bytearray, bytearray]:
        jp = bytearray(b"20KD" + bytes([0xC0]) * (8 + self.N * self.JP_STRIDE - 4))
        us = bytearray(b"30KD" + bytes([0x60]) * (8 + self.N * self.US_STRIDE - 4))
        struct.pack_into("<I", jp, 4, 0)
        struct.pack_into("<I", us, 4, 0)
        for i in range(self.N):
            jo, uo = 8 + i * self.JP_STRIDE, 8 + i * self.US_STRIDE
            cards = bytes([(i + k) & 0xFF for k in range(60)])
            jp[jo:jo + 60] = cards
            us[uo:uo + 60] = cards
            if b"%" in deck:
                us[uo + 60:uo + 79] = (deck % i).ljust(19, b"\0")
            else:
                us[uo + 60:uo + 79] = deck[:19].ljust(19, b"\0")
            if b"%" in owner:
                us[uo + 79:uo + 100] = (owner % i).ljust(21, b"\0")
            else:
                us[uo + 79:uo + 100] = owner[:21].ljust(21, b"\0")
        return jp, us

    def test_full_record_partition(self) -> None:
        jp, us = self.make()
        out, stats = en_text.graft_dek(bytes(jp), bytes(us), [])
        for i in range(self.N):
            jo, uo = 8 + i * self.JP_STRIDE, 8 + i * self.US_STRIDE
            self.assertEqual(out[jo:jo + 60], bytes(us[uo:uo + 60]), f"card list {i}")
            self.assertEqual(out[jo + 60:jo + 73], bytes(us[uo + 60:uo + 73]), f"deck {i}")
            self.assertEqual(out[jo + 73:jo + 94], bytes(us[uo + 79:uo + 100]), f"owner {i}")
            self.assertEqual(out[jo + 94:jo + 104], bytes(jp[jo + 94:jo + 104]), f"tail {i}")
        self.assertEqual(stats["names"], self.N)
        self.assertEqual(stats["owners"], self.N)

    def test_overlong_truncates_and_reports(self) -> None:
        jp, us = self.make(deck=b"XXXXXXXXXXXXXXXX", owner=b"Y" * 21)
        report: list[str] = []
        out, stats = en_text.graft_dek(bytes(jp), bytes(us), report)
        jo = 8 + 0 * self.JP_STRIDE
        self.assertEqual(out[jo + 60:jo + 73], b"X" * 12 + b"\0")
        self.assertEqual(out[jo + 73:jo + 94], b"Y" * 20 + b"\0")
        self.assertEqual(len(stats["overlong"]), 2 * self.N)
        self.assertEqual(len(report), 2 * self.N)
        self.assertIn("deck 0 name", report[0])
        self.assertIn("deck 0 owner", report[1])

    def test_card_list_mismatch_rejected(self) -> None:
        jp, us = self.make()
        us[8 + 5 * self.US_STRIDE + 10] ^= 0xFF
        with self.assertRaises(AssertionError):
            en_text.graft_dek(bytes(jp), bytes(us), [])


if __name__ == "__main__":
    unittest.main()
