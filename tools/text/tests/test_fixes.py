"""tools/text/vcdiff.py and fixes.py: decoding a VCDIFF patch, porting a change to another build."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import en_text  # noqa: E402
import fixes  # noqa: E402
import vcdiff  # noqa: E402


def patch(source_len: int, target: bytes, data: bytes, inst: bytes, addr: bytes) -> bytes:
    """One-window VCDIFF over the whole source (no compression)."""
    body = bytes([len(target), 0, len(data), len(inst), len(addr)]) + data + inst + addr
    return b"\xd6\xc3\xc4\x00\x00" + bytes([0x01, source_len, 0, len(body)]) + body


class TestVcdiff(unittest.TestCase):
    def test_copy_and_add(self) -> None:
        source = b"ABCDEFGH"
        # code 20 = COPY size 4 mode 0 (address 2 -> "CDEF"), code 3 = ADD size 2 ("XY")
        p = patch(len(source), b"CDEFXY", b"XY", bytes([20, 3]), bytes([2]))
        self.assertEqual(vcdiff.decode(source, p), b"CDEFXY")

    def test_rejects_other_files(self) -> None:
        with self.assertRaises(ValueError):
            vcdiff.decode(b"", b"PK\x03\x04")


class TestPort(unittest.TestCase):
    def test_change_found_by_its_surroundings(self) -> None:
        # the same table in two builds, pointers (4 bytes before each record) differ
        before = b"\xf4\xec\x1d\x80" + bytes.fromhex("44212e1505610000") + b"\x5c\xec\x1d\x80" + bytes.fromhex("ff5732ff49ff0000")
        after = b"\xf4\xec\x1d\x80" + bytes.fromhex("ff212e1505610000") + b"\x5c\xec\x1d\x80" + bytes.fromhex("445732ff49ff0000")
        target = b"\x00" * 7 + b"\x9c\x18\x1e\x80" + bytes.fromhex("44212e1505610000") + b"\x3c\x18\x1e\x80" + bytes.fromhex("ff5732ff49ff0000")
        ported, notes = fixes.port(before, after, target)
        self.assertEqual(ported[11], 0xFF)
        self.assertEqual(ported[23], 0x44)
        self.assertEqual(len(notes), 2)

    def test_text_not_in_the_target_is_left_alone(self) -> None:
        ported, notes = fixes.port(b"Blue Blaster....", b"Super Shocker...", b"\x83\x75\x83\x8b" * 4)
        self.assertEqual(ported, b"\x83\x75\x83\x8b" * 4)
        self.assertIn("left alone", notes[0])


class TestUsLine(unittest.TestCase):
    def test_leftover_bytes_after_a_missing_nul(self) -> None:
        self.assertEqual(en_text.us_line(b"by +300.\x95\x9c"), b"by +300.")
        self.assertEqual(en_text.us_line(b"*b1 attack"), b"*b1 attack")


if __name__ == "__main__":
    unittest.main()
