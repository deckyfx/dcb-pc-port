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
        # One palette changed shape (and is not a reshape of the same colours); the rest still swaps.
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


def pack4(rows):
    """4-bit pixel rows (lists of indices) as TIM image bytes."""
    return b"".join(bytes(r[x] | (r[x + 1] << 4) for x in range(0, len(r), 2)) for r in rows)


def unpack4(data, width):
    return [[(data[r + x // 2] >> 4) if x & 1 else (data[r + x // 2] & 15) for x in range(width)]
            for r in range(0, len(data), width // 2)]


# An 88x80 plate: background index 1, an 11x11 icon of index `a` at (13, 26), one of `b` at (13, 41).
def plate(a, b):
    rows = [[1] * 88 for _ in range(80)]
    for y in range(11):
        for x in range(11):
            rows[26 + y][13 + x] = a
            rows[41 + y][13 + x] = b
    return pack4(rows)


PLATE_PAL_JP = struct.pack("<16H", 0, 0x1484, 0x35DC, 0x6F7B, *range(12))
PLATE_PAL_US = struct.pack("<16H", 0, 0x1484, 0x2129, 0x4969, 0x37C0, *range(11))


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
        same = tim_bytes((512, 0, 12, 48), b"same")
        jp_tis = swap.write_tis([same, tim_bytes(self.NAMES, b"jp-names")])
        us_tis = swap.write_tis([tim_bytes(self.NAMES, b"us-names"), same])
        jp_pak = write_pak([chunk(2, 0xC9, 0, b"MSCD jp script"), chunk(5, 0xFA1, 0, jp_tis)])
        us_pak = write_pak([chunk(2, 0xC9, 0, b"MSCD us script"), chunk(5, 0xFA1, 0, us_tis)])
        out = swap.dcb_containers.read_tis(swap.area_image_chunk(jp_pak, us_pak))
        self.assertEqual(out, [same, tim_bytes(self.NAMES, b"us-names")])

    def test_graft_recomposes_the_help_plate(self):
        # The city HELP MENU plate (COMPOSE): the US plate with the icons for the JP controls.
        jp_px, us_px = plate(2, 3), plate(3, 4)
        jp = [tim_bytes(self.HELP, jp_px, palette=PLATE_PAL_JP)]
        us = [tim_bytes(self.HELP, us_px, palette=PLATE_PAL_US)]
        out = swap.read_tim(swap.dcb_containers.read_tis(
            swap.graft_tis(swap.write_tis(jp), swap.write_tis(us), drv_name="C.DRV")[0])[0], 0)
        ut, jt = swap.read_tim(us[0], 0), swap.read_tim(jp[0], 0)
        self.assertEqual((out.pixels, out.palette), swap.compose_image(ut, jt, swap.COMPOSE[("C.DRV", self.HELP)]))
        self.assertNotEqual(out.pixels, us_px)

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

    def test_graft_prewarps_the_sub_menu_labels(self):
        # The JP code reads 68 texels per sub-menu label onto 64 pixels (the US code reads 64).
        label = bytes((2 * x % 16) | ((2 * x + 1) % 16) << 4 for x in range(32)) * 2  # 64x2, 4-bit
        jp = [tim_bytes(self.BUTTONS, bytes(64)), tim_bytes(self.NAMES, b"jp-names")]
        us = [tim_bytes(self.NAMES, b"us-names"), tim_bytes(self.BUTTONS, label)]
        out = swap.dcb_containers.read_tis(swap.graft_tis(swap.write_tis(jp), swap.write_tis(us), drv_name="C.DRV")[0])
        self.assertEqual(out[0], tim_bytes(self.BUTTONS, swap.prewarp_columns(label, 4, 64, 68, 64)))
        self.assertNotEqual(out[0], us[1])
        self.assertEqual(out[1], us[0])  # other images go in as they are
        plain = swap.dcb_containers.read_tis(swap.graft_tis(swap.write_tis(jp), swap.write_tis(us))[0])
        self.assertEqual(plain[0], us[1])  # no DRV named, nothing warped

    def test_tis_entry_is_named_like_the_ripper(self):
        t = swap.Tim(0x1000 + 0x1268C, 4, (682, 256, 21, 123), (432, 495, 16, 1))
        e = swap.tis_entry(t, "f2a8df6a3e668b14", "C.DRV", "OBJECT/UNIT.TIS", 0x1000)
        self.assertEqual((e["w"], e["h"], e["bpp"]), (84, 123, 4))
        self.assertEqual(e["path"], "C_OBJECT_UNIT_off0001268c_84x123.png")


class PrewarpTest(unittest.TestCase):
    @staticmethod
    def row(values):
        return bytes(values[x] | (values[x + 1] << 4) for x in range(0, len(values), 2))

    @staticmethod
    def unpack(data, width):
        return [[(data[r + x // 2] >> 4) if x & 1 else (data[r + x // 2] & 15) for x in range(width)]
                for r in range(0, len(data), width // 2)]

    def test_screen_column_i_shows_source_column_i(self):
        # The GPU steps u by 68/64 per pixel: pixel i samples texel floor(i * 68 / 64).
        src = [i % 15 + 1 for i in range(64)]
        out = self.unpack(swap.prewarp_columns(self.row(src) * 3, 4, 64, 68, 64), 64)
        self.assertEqual(len(out), 3)
        shown = [out[0][i * 68 // 64] for i in range(64) if i * 68 // 64 < 64]
        self.assertEqual(shown, src[:61])  # the 3 rightmost columns have no texel left

    def test_unsampled_texels_repeat_their_left_neighbour(self):
        src = list(range(16)) * 4
        out = self.unpack(swap.prewarp_columns(self.row(src), 4, 64, 68, 64), 64)[0]
        for t in (16, 33, 50):  # the texels no pixel samples
            self.assertEqual(out[t], out[t - 1])

    def test_one_to_one_is_unchanged(self):
        data = bytes(range(32)) * 4
        self.assertEqual(swap.prewarp_columns(data, 4, 64, 64, 64), data)

    def test_refuses_other_depths(self):
        with self.assertRaises(ValueError):
            swap.prewarp_columns(bytes(64), 8, 64, 68, 64)

    def test_only_the_listed_images_are_warped(self):
        label = self.row([i % 16 for i in range(64)]) * 144
        buttons = swap.Tim(0, 4, (792, 0, 16, 144), (512, 239, 16, 2), label)
        self.assertNotEqual(swap.pixels_for_jp("C.DRV", buttons), label)
        self.assertEqual(swap.pixels_for_jp("B.DRV", buttons), label)
        city_menu = swap.Tim(0, 4, (736, 0, 29, 144), (512, 237, 16, 1), b"x" * 58 * 144)
        self.assertEqual(swap.pixels_for_jp("C.DRV", city_menu), city_menu.pixels)  # drawn 1:1 by both

    def test_with_pixels_replaces_the_image_payload_only(self):
        t = tim_bytes((792, 0, 2, 2), b"ABCDEFGH")
        self.assertEqual(swap.with_pixels(t, b"abcdefgh"), tim_bytes((792, 0, 2, 2), b"abcdefgh"))
        with self.assertRaises(ValueError):
            swap.with_pixels(t, b"abc")


class ComposeTest(unittest.TestCase):
    HELP = (808, 0, 22, 80)

    def tims(self, jp_px, us_px, jp_pal=PLATE_PAL_JP, us_pal=PLATE_PAL_US):
        return (swap.Tim(0, 4, self.HELP, (528, 242, 16, 1), us_px, us_pal),
                swap.Tim(0, 4, self.HELP, (528, 242, 16, 1), jp_px, jp_pal))

    def test_help_plate_gets_the_jp_icons_in_the_us_palette(self):
        # JP: O (red, index 2) on the Enter row, X (index 3) on the Menu row.
        # US: X (index 3) on the Enter row, triangle (green, index 4) on the Menu row.
        us, jp = self.tims(plate(2, 3), plate(3, 4))
        px, pal = swap.compose_image(us, jp, swap.COMPOSE[("C.DRV", self.HELP)])
        rows = unpack4(px, 88)
        colours = struct.unpack("<16H", pal)
        self.assertEqual(colours[rows[26][13]], 0x35DC)  # the JP O, red, in a slot the US no longer uses
        self.assertEqual(rows[26][13], 2)               # slot 2 (0x2129): no US pixel uses it
        self.assertEqual(rows[41][13], 3)               # the US X, moved down a row
        self.assertEqual(rows[0][0], 1)                 # the rest is the US plate
        self.assertEqual(colours[3], 0x4969)            # colours still in use are kept

    def test_exact_colours_reuse_the_us_index(self):
        us, jp = self.tims(plate(2, 3), plate(3, 4), jp_pal=struct.pack("<16H", 0, 0x1484, 0x4969, *range(13)))
        px, pal = swap.compose_image(us, jp, [("jp", (13, 26, 11, 11), (13, 26))])
        self.assertEqual(unpack4(px, 88)[26][13], 3)  # 0x4969 is US index 3
        self.assertEqual(pal, PLATE_PAL_US)

    def test_nearest_colour_when_no_slot_is_free(self):
        full = pack4([[x % 16 for x in range(88)] for _ in range(80)])  # every index in use
        us, jp = self.tims(plate(2, 3), full)
        px, pal = swap.compose_image(us, jp, [("jp", (13, 26, 11, 11), (13, 26))])
        self.assertEqual(pal, PLATE_PAL_US)
        nearest = min(range(16), key=lambda k: swap._rgb_distance(0x35DC, struct.unpack("<16H", PLATE_PAL_US)[k]))
        self.assertEqual(unpack4(px, 88)[26][13], nearest)

    def test_refuses_other_shapes(self):
        us, jp = self.tims(plate(2, 3), plate(3, 4))
        with self.assertRaises(ValueError):
            swap.compose_image(us, swap.Tim(0, 8, self.HELP, None, jp.pixels, b""), [])

    def test_with_palette_replaces_the_clut_payload_only(self):
        t = tim_bytes((792, 0, 2, 2), b"ABCDEFGH")
        new = b"\x02\x00" * 16
        self.assertEqual(swap.with_palette(t, new), tim_bytes((792, 0, 2, 2), b"ABCDEFGH", palette=new))
        with self.assertRaises(ValueError):
            swap.with_palette(t, b"\0\0")


class MatchArchiveTest(unittest.TestCase):
    """B:\\MATCH / WIN archives: ARC offset tables, the record strip cut to the JP width."""
    STRIP_JP, STRIP_US = (464, 184, 34, 18), (464, 184, 48, 18)

    def test_narrow_keeps_the_left_part_and_the_right_edge(self):
        row = list(range(16)) * 12  # 192 texels
        out = unpack4(swap.narrow_columns(pack4([row, row]), 4, 192, 136, 10), 136)
        self.assertEqual(out, [row[:126] + row[-10:]] * 2)

    def test_narrow_refuses_a_wider_result(self):
        with self.assertRaises(ValueError):
            swap.narrow_columns(bytes(68), 4, 136, 192, 10)

    def test_us_strip_pairs_with_the_jp_strip(self):
        blank = [5] * 192
        us = [(swap.Tim(0, 4, self.STRIP_US, (400, 249, 16, 1), pack4([blank] * 18), b"p"), [])]
        jp = [(swap.Tim(0, 4, self.STRIP_JP, (400, 249, 16, 1), pack4([[5] * 40 + [9] * 96] * 18), b"p"), [])]
        cut = swap.narrowed("B.DRV", us)
        self.assertEqual(cut[0][0].image, self.STRIP_JP)
        self.assertEqual(cut[0][0].pixels, pack4([[5] * 136] * 18))
        self.assertEqual(swap.narrowed("C.DRV", us), us)  # only the listed DRV
        pairs, _ = swap.pair(jp, cut, partial=True)
        self.assertEqual(len(pairs), 1)

    def test_arc_offsets_drop_the_end_of_file_entry(self):
        a, b = tim_bytes((384, 112, 2, 2), b"cardcard"), tim_bytes((424, 0, 2, 2), b"turnturn")
        data = struct.pack("<3I", 12, 12 + len(a), 12 + len(a) + len(b)) + a + b
        self.assertEqual(swap.arc_offsets(data), [12, 12 + len(a)])
        self.assertEqual(swap.container_offsets(data, "MATCH/004.ARC"), [12, 12 + len(a)])
        self.assertIsNone(swap.container_offsets(b"\x03\0\0\0", "WIN/004.ARC"))
        drv = b"\0" * 8 + data
        toc = [SimpleNamespace(path="MATCH/004.ARC", offset=8, size=len(data))]
        out = swap.tis_entry_tims(drv, toc, re.compile(r"(MATCH|WIN)/\d+\.ARC$"))
        self.assertEqual([t.image for t, _ in out["MATCH/004.ARC"]], [(384, 112, 2, 2), (424, 0, 2, 2)])

    def test_match_archives_swap_partially(self):
        self.assertTrue(swap.PARTIAL.search("B.DRV:MATCH/004.ARC"))
        self.assertTrue(swap.PARTIAL.search("B.DRV:WIN/141.ARC"))
        self.assertFalse(swap.PARTIAL.search("B.DRV:CBTL_SYS.ARC"))  # pairs whole (reshaped)
        self.assertFalse(swap.PARTIAL.search("B.DRV:M_CARD.ARC"))


class PaletteReshapeTest(unittest.TestCase):
    """CBTL_SYS's phase banner: the JP palette is uploaded 16x2, the US one 32x1, same colours."""
    BANNER = (948, 304, 11, 72)
    ROW0 = [0x0000, 0xB58D, 0x0000, 0x8C63, 0x9084, 0x8C63, 0x94A5, 0x4E53] * 2
    ROW1 = [0x0000, 0xB58D, 0x0000, 0x8C63, 0x9084, 0x8C63, 0x94A5, 0xCE53] * 2

    def banner(self, clut, entries, pixels=b"jp"):
        return swap.Tim(0, 4, self.BANNER, clut, pixels, struct.pack(f"<{len(entries)}H", *entries))

    def variants(self, n):
        return [{"img": "", "path": f"banner_pal{i}.png", "w": 44, "h": 72} for i in range(n)]

    def test_same_colours_in_another_shape(self):
        jp = self.banner((816, 497, 16, 2), self.ROW0 + self.ROW1)
        # The US build clears the semi-transparency bit of two row-0 colours: still a reshape.
        us_row0 = [c & 0x7FFF if i in (3, 4) else c for i, c in enumerate(self.ROW0)]
        us = self.banner((816, 497, 32, 1), us_row0 + self.ROW1, b"en")
        self.assertTrue(swap.is_palette_reshape(jp, us))

    def test_other_colours_are_not_a_reshape(self):
        jp = self.banner((816, 497, 16, 2), self.ROW0 + self.ROW1)
        recoloured = self.ROW0[:7] + [0x7FFF] + self.ROW0[8:] + self.ROW1  # one colour redrawn
        self.assertFalse(swap.is_palette_reshape(jp, self.banner((816, 497, 32, 1), recoloured)))

    def test_other_corner_size_or_image_is_not_a_reshape(self):
        jp = self.banner((816, 497, 16, 2), self.ROW0 + self.ROW1)
        both = self.ROW0 + self.ROW1
        self.assertFalse(swap.is_palette_reshape(jp, self.banner((832, 497, 32, 1), both)))    # moved
        self.assertFalse(swap.is_palette_reshape(jp, self.banner((816, 497, 16, 2), both)))    # same shape
        self.assertFalse(swap.is_palette_reshape(jp, self.banner((816, 497, 48, 1), both * 2)))  # more entries
        other = swap.Tim(0, 4, (948, 256, 12, 48), (816, 497, 32, 1), b"en", jp.palette)
        self.assertFalse(swap.is_palette_reshape(jp, other))

    def test_reshaped_us_banner_pairs_with_the_jp_one(self):
        both = self.ROW0 + self.ROW1
        jp = [(self.banner((816, 497, 16, 2), both), self.variants(2)),
              (swap.Tim(1, 4, (948, 256, 12, 48), (816, 497, 16, 2), b"p", b""), self.variants(2))]
        us_banner = self.banner((816, 497, 32, 1), both, b"en")
        us = [(us_banner, self.variants(1)), (jp[1][0], self.variants(2))]
        self.assertIsNone(swap.pair(jp, us))  # as the US disc has it: another layout
        out = swap.reshaped(jp, us)
        self.assertEqual(out[0][0].clut, (816, 497, 16, 2))
        self.assertEqual((out[0][0].pixels, out[0][0].palette), (b"en", us_banner.palette))  # US data kept
        self.assertEqual(len(out[0][1]), 2)
        self.assertIs(out[1][0], us[1][0])  # the other TIM as it was
        pairs, _ = swap.pair(jp, out)
        self.assertEqual(len(pairs), 2)

    def test_disc_read_images_without_variants(self):
        both = self.ROW0 + self.ROW1
        jp = [(self.banner((816, 497, 16, 2), both), [])]
        out = swap.reshaped(jp, [(self.banner((816, 497, 32, 1), both, b"en"), [])])
        self.assertEqual((out[0][0].clut, out[0][1]), ((816, 497, 16, 2), []))


class FitTest(unittest.TestCase):
    """FIT (the title): US art fitted into a JP image and palette like the texture replacer does
    (mirrors test_patch_art fit)."""
    PAL = [0x0000, 0x001F, 0x03E0, 0x7C00, 0x001F] + [0x7FFF] * 11  # red twice (1 and 4)
    ROW = [(255, 0, 0, 255), (255, 0, 0, 255), (0, 200, 0, 255), (0, 0, 255, 255), (0, 0, 0, 0),
           (255, 255, 255, 255), (255, 0, 0, 255), (255, 0, 0, 255)]

    def jp(self):
        idx = [4, 1, 2, 3, 0, 0, 0, 0] * 2
        pixels = bytes(idx[i] | idx[i + 1] << 4 for i in range(0, 16, 2))
        return swap.Tim(0, 4, (704, 128, 2, 2), (704, 250, 16, 1), pixels, struct.pack("<16H", *self.PAL))

    @staticmethod
    def unpack(px, width):
        idx = [(b >> s) & 15 for b in px for s in (0, 4)]
        return [idx[i:i + width] for i in range(0, len(idx), width)]

    def test_fit(self):
        art = b"".join(bytes(p) for p in self.ROW * 2)
        want = [[4, 1, 2, 3, 0, 5, 1, 1]] * 2
        self.assertEqual(self.unpack(swap.fit_image(self.jp(), 8, 2, art), 8), want)  # JP reds kept
        wide = b"".join(bytes(p) * 2 for p in self.ROW * 2)
        self.assertEqual(self.unpack(swap.fit_image(self.jp(), 16, 2, wide), 8), want)  # box-downsampled
        slot = self.unpack(swap.fit_image(self.jp(), 16, 2, wide, 16), 16)
        self.assertEqual((len(slot[0]), slot[0][0], slot[0][9]), (16, 1, 0))  # no JP index to keep
        self.assertIsNone(swap.fit_image(self.jp(), 4, 2, art))  # never upscaled
        self.assertIsNone(swap.fit_image(self.jp(), 16, 2, wide, 6))  # slot narrower than the image

    def test_fit_table_is_the_title(self):
        for (drv, entry, jp), (us, slot) in swap.FIT.items():
            self.assertEqual((drv, entry), ("B.DRV", "TITLE.ARC"))
            self.assertTrue(slot == 0 or slot >= jp[2] * 4)


if __name__ == "__main__":
    unittest.main()
