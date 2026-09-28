"""Unit tests for swap_us_images.py (no game data needed)."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import swap_us_images as swap  # noqa: E402


def tim(offset, pixels, image=(320, 0, 64, 40), clut=(640, 80, 16, 1), bpp=4):
    return swap.Tim(offset, bpp, image, clut, pixels, b"")


def item(offset, pixels, name, **shape):
    return (tim(offset, pixels, **shape), [{"img": "", "path": name, "w": 256, "h": 40}])


def names(pairs):
    return sorted((a[1][0]["path"], b[1][0]["path"]) for a, b in pairs)


class PairTest(unittest.TestCase):
    def test_reordered_container_pairs_by_shape(self):
        # The US tool moved the attack-name TIM to the end of every E PAK.
        name, other = (640, 0, 64, 40), (320, 0, 48, 48)
        jp = [item(0, b"n", "jp_name", image=name), item(1, b"o", "jp_other", image=other)]
        us = [item(0, b"o", "us_other", image=other), item(1, b"N", "us_name", image=name)]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(names(pairs), [("jp_name", "us_name"), ("jp_other", "us_other")])
        self.assertEqual(shared, 0)

    def test_identical_pixels_pair_first(self):
        # Two same-shape TIMs swapped in the US PAK; the unchanged one must not be crossed.
        jp = [item(0, b"same", "jp_a"), item(1, b"jp", "jp_b")]
        us = [item(0, b"en", "us_b"), item(1, b"same", "us_a")]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(names(pairs), [("jp_a", "us_a"), ("jp_b", "us_b")])
        self.assertEqual(shared, 0)

    def test_same_shape_without_match_pairs_in_order(self):
        jp = [item(0, b"j1", "jp_1"), item(1, b"j2", "jp_2")]
        us = [item(0, b"u1", "us_1"), item(1, b"u2", "us_2")]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(names(pairs), [("jp_1", "us_1"), ("jp_2", "us_2")])
        self.assertEqual(shared, 2)

    def test_different_layout_is_refused(self):
        self.assertIsNone(swap.pair([item(0, b"j", "jp")], [item(0, b"u", "us", clut=(640, 80, 32, 1))]))

    def test_partial_keeps_matching_shapes_only(self):
        # CBTL_SYS: one palette changed shape; the rest still swaps.
        jp = [item(0, b"j1", "jp_1"), item(1, b"j2", "jp_2", clut=(816, 497, 16, 2))]
        us = [item(0, b"u1", "us_1"), item(1, b"u2", "us_2", clut=(816, 497, 32, 1))]
        pairs, _ = swap.pair(jp, us, partial=True)
        self.assertEqual(names(pairs), [("jp_1", "us_1")])


class ResolveTest(unittest.TestCase):
    def test_single_candidate(self):
        pick, note = swap.resolve("E_5_off0_256x40.png", [swap.Candidate(b"en", b"jp", "E.DRV:5.PAK")])
        self.assertEqual((pick.us, note), (b"en", ""))

    def test_unchanged_is_skipped(self):
        pick, _ = swap.resolve("x", [swap.Candidate(b"jp", b"jp", "E.DRV:5.PAK")])
        self.assertIsNone(pick)

    def test_untranslated_copy_dropped(self):
        cands = [swap.Candidate(b"jp", b"jp", "E.DRV:829.PAK"), swap.Candidate(b"en", b"jp", "E.DRV:511.PAK")]
        pick, note = swap.resolve("E_829_off0_256x40.png", cands)
        self.assertEqual(pick.us, b"en")
        self.assertIn("untranslated copy dropped", note)

    def test_own_entry_wins_between_translations(self):
        cands = [swap.Candidate(b"poop", b"jp", "E.DRV:804.PAK"), swap.Candidate(b"party", b"jp", "E.DRV:800.PAK")]
        pick, note = swap.resolve("E_800_off000011f0_256x40.png", cands)
        self.assertEqual(pick.label, "E.DRV:800.PAK")
        self.assertIn("2 different translations", note)


class DataTest(unittest.TestCase):
    def test_fnv_matches_the_replacer(self):
        # vfs::fnv1a64: offset basis for no data, and a known single-byte value.
        self.assertEqual(swap.fnv1a64(b""), "cbf29ce484222325")
        self.assertEqual(swap.fnv1a64(b"a"), "af63dc4c8601ec8c")

    def test_read_tim_keeps_payloads(self):
        clut = struct.pack("<IHHHH", 12 + 32, 640, 80, 16, 1) + bytes(range(32))
        image = struct.pack("<IHHHH", 12 + 8, 320, 0, 2, 2) + b"ABCDEFGH"
        data = b"\0" * 4 + struct.pack("<II", 0x10, 8) + clut + image
        t = swap.read_tim(data, 4)
        self.assertEqual((t.bpp, t.image, t.clut), (4, (320, 0, 2, 2), (640, 80, 16, 1)))
        self.assertEqual((t.pixels, t.palette), (b"ABCDEFGH", bytes(range(32))))

    def test_not_a_tim(self):
        self.assertIsNone(swap.read_tim(b"\0" * 32, 0))


if __name__ == "__main__":
    unittest.main()
