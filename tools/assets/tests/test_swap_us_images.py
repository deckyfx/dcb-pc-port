"""Unit tests for swap_us_images.py (no game data needed)."""
from __future__ import annotations

import re
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

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


def tim_bytes(image, pixels, clut=(512, 228, 16, 1), palette=b"\x01\x00" * 16):
    """A 4-bit TIM: CLUT block, then image block (both u32 length, u16 x, y, w, h, data)."""
    return (struct.pack("<II", 0x10, 8)
            + struct.pack("<IHHHH", 12 + len(palette), *clut) + palette
            + struct.pack("<IHHHH", 12 + len(pixels), *image) + pixels)


class TisTest(unittest.TestCase):
    NAMES, BUTTONS, HELP = (832, 0, 48, 120), (792, 0, 16, 144), (808, 0, 22, 80)

    def test_write_tis_is_the_inverse_of_read_tis(self):
        tims = [tim_bytes(self.NAMES, b"a" * 8), tim_bytes(self.BUTTONS, b"b" * 4)]
        data = swap.write_tis(tims)
        self.assertEqual(data[:4], b"Tp\x02\x00")
        self.assertEqual(swap.dcb_containers.read_tis(data), tims)
        self.assertEqual(swap.tis_offsets(data), [12, 12 + len(tims[0])])

    def test_write_tis_refuses_partial_words(self):
        with self.assertRaises(ValueError):
            swap.write_tis([b"abc"])

    def test_graft_keeps_jp_order_and_takes_us_images(self):
        # The US build stores the TIMs in another order; the JP code may index them.
        same = tim_bytes((512, 0, 12, 48), b"same")
        jp = [tim_bytes(self.NAMES, b"jp-names"), same, tim_bytes(self.BUTTONS, b"jpbt")]
        us = [tim_bytes(self.BUTTONS, b"usbt", palette=b"\x02\x00" * 16), same, tim_bytes(self.NAMES, b"us-names")]
        data, n = swap.graft_tis(swap.write_tis(jp), swap.write_tis(us))
        self.assertEqual(n, 2)
        self.assertEqual(swap.dcb_containers.read_tis(data), [us[2], same, us[0]])

    def test_graft_leaves_kept_rects_jp(self):
        jp = [tim_bytes(self.HELP, b"jp-help!"), tim_bytes(self.NAMES, b"jp-names")]
        us = [tim_bytes(self.NAMES, b"us-names"), tim_bytes(self.HELP, b"us-help!")]
        data, n = swap.graft_tis(swap.write_tis(jp), swap.write_tis(us), frozenset({self.HELP}))
        self.assertEqual(n, 1)
        self.assertEqual(swap.dcb_containers.read_tis(data), [jp[0], us[0]])

    def test_graft_refuses_other_shapes(self):
        jp = swap.write_tis([tim_bytes(self.NAMES, b"jp-names")])
        us = swap.write_tis([tim_bytes((832, 0, 64, 120), b"us-names")])
        with self.assertRaises(ValueError):
            swap.graft_tis(jp, us)

    def test_area_image_chunk_touches_only_the_tis(self):
        write_pak, chunk = swap.dcb_containers.write_pak, swap.dcb_containers.Chunk
        jp_tis = swap.write_tis([tim_bytes(self.HELP, b"jp-help!"), tim_bytes(self.NAMES, b"jp-names")])
        us_tis = swap.write_tis([tim_bytes(self.NAMES, b"us-names"), tim_bytes(self.HELP, b"us-help!")])
        jp_pak = write_pak([chunk(2, 0xC9, 0, b"MSCD jp script"), chunk(5, 0xFA1, 0, jp_tis)])
        us_pak = write_pak([chunk(2, 0xC9, 0, b"MSCD us script"), chunk(5, 0xFA1, 0, us_tis)])
        out = swap.dcb_containers.read_tis(swap.area_image_chunk(jp_pak, us_pak))
        # KEEP_JP holds the city HELP MENU plate: its US button names do not match the JP code.
        self.assertEqual(out, [tim_bytes(self.HELP, b"jp-help!"), tim_bytes(self.NAMES, b"us-names")])

    def test_tis_start(self):
        tis = swap.write_tis([tim_bytes(self.NAMES, b"n" * 4)])
        pak = swap.dcb_containers.write_pak([swap.dcb_containers.Chunk(2, 0xC8, 0, b"MSCD1234"),
                                             swap.dcb_containers.Chunk(5, 0xFA0, 0, tis)])
        self.assertEqual(swap.tis_start(pak, "AREA00.PAK"), 8 + 8 + 8)
        self.assertEqual(swap.tis_start(tis, "OBJECT/WORLD.TIS"), 0)
        self.assertIsNone(swap.tis_start(b"\x10\0\0\0", "OBJECT/A_1.TIM"))

    def test_tis_entry_tims_reads_the_disc_not_the_manifest(self):
        tis = swap.write_tis([tim_bytes(self.NAMES, b"n" * 4), tim_bytes(self.BUTTONS, b"b" * 4)])
        drv = b"\0" * 16 + tis
        toc = [SimpleNamespace(path="OBJECT/WORLD.TIS", offset=16, size=len(tis)),
               SimpleNamespace(path="OBJECT/MAP.TIS", offset=16, size=len(tis))]
        out = swap.tis_entry_tims(drv, toc, re.compile(r"OBJECT/WORLD\.TIS$"))
        self.assertEqual(list(out), ["OBJECT/WORLD.TIS"])
        self.assertEqual([(t.offset, t.image, v) for t, v in out["OBJECT/WORLD.TIS"]],
                         [(16 + 12, self.NAMES, []), (16 + 12 + 68, self.BUTTONS, [])])  # a TIM is 68 bytes

    def test_tis_entry_is_named_like_the_ripper(self):
        t = swap.Tim(0x1000 + 0x1268C, 4, (682, 256, 21, 123), (432, 495, 16, 1))
        e = swap.tis_entry(t, "f2a8df6a3e668b14", "C.DRV", "OBJECT/UNIT.TIS", 0x1000)
        self.assertEqual((e["w"], e["h"], e["bpp"]), (84, 123, 4))
        self.assertEqual(e["path"], "C_OBJECT_UNIT_off0001268c_84x123.png")


if __name__ == "__main__":
    unittest.main()
