"""Unit tests for swap_us_images.py pairing (no game data needed)."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import swap_us_images as swap  # noqa: E402


def tim(offset, image=(320, 0, 64, 40), clut=(640, 80, 16, 1), bpp=4):
    return swap.Tim(offset, bpp, image, clut)


def var(img, path="a.png", w=256, h=40):
    return [{"img": img, "path": path, "w": w, "h": h}]


def paths(pairs):
    return [(a[0]["path"], b[0]["path"]) for a, b in pairs]


class PairTest(unittest.TestCase):
    def test_reordered_container_pairs_by_shape(self):
        # The US tool moved the attack-name TIM to the end of every E PAK.
        name, other = (640, 0, 64, 40), (320, 0, 48, 48)
        jp = [(tim(0, name), var("j1", "jp_name")), (tim(1, other), var("j2", "jp_other", 192, 48))]
        us = [(tim(0, other), var("u2", "us_other", 192, 48)), (tim(1, name), var("u1", "us_name"))]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(sorted(paths(pairs)), [("jp_name", "us_name"), ("jp_other", "us_other")])
        self.assertEqual(shared, 0)

    def test_identical_content_pairs_first(self):
        # Two same-shape TIMs swapped in the US PAK; the unchanged one must not be crossed.
        jp = [(tim(0), var("same", "jp_a")), (tim(1), var("jp_only", "jp_b"))]
        us = [(tim(0), var("en", "us_b")), (tim(1), var("same", "us_a"))]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(sorted(paths(pairs)), [("jp_a", "us_a"), ("jp_b", "us_b")])
        self.assertEqual(shared, 0)

    def test_same_shape_without_match_pairs_in_order(self):
        jp = [(tim(0), var("j1", "jp_1")), (tim(1), var("j2", "jp_2"))]
        us = [(tim(0), var("u1", "us_1")), (tim(1), var("u2", "us_2"))]
        pairs, shared = swap.pair(jp, us)
        self.assertEqual(paths(pairs), [("jp_1", "us_1"), ("jp_2", "us_2")])
        self.assertEqual(shared, 2)

    def test_different_layout_is_refused(self):
        jp = [(tim(0), var("j1"))]
        us = [(tim(0, clut=(640, 80, 32, 1)), var("u1"))]
        self.assertIsNone(swap.pair(jp, us))

    def test_partial_keeps_matching_shapes_only(self):
        # CBTL_SYS: one palette changed shape; the rest still swaps.
        jp = [(tim(0), var("j1", "jp_1")), (tim(1, clut=(816, 497, 16, 2)), var("j2", "jp_2"))]
        us = [(tim(0), var("u1", "us_1")), (tim(1, clut=(816, 497, 32, 1)), var("u2", "us_2"))]
        pairs, _ = swap.pair(jp, us, partial=True)
        self.assertEqual(paths(pairs), [("jp_1", "us_1")])


class ReadTimTest(unittest.TestCase):
    def test_header_with_clut(self):
        clut = struct.pack("<IHHHH", 12 + 32, 640, 80, 16, 1) + bytes(32)
        image = struct.pack("<IHHHH", 12 + 8, 320, 0, 2, 2) + bytes(8)
        data = b"\0" * 4 + struct.pack("<II", 0x10, 8) + clut + image
        t = swap.read_tim(data, 4)
        self.assertEqual((t.bpp, t.image, t.clut), (4, (320, 0, 2, 2), (640, 80, 16, 1)))

    def test_not_a_tim(self):
        self.assertIsNone(swap.read_tim(b"\0" * 32, 0))


if __name__ == "__main__":
    unittest.main()
