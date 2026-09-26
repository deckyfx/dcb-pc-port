"""Round-trip tests for extract_disc.py against a synthetic Mode 2 disc (no game data needed)."""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import extract_disc  # noqa: E402

MKISOFS = shutil.which("genisoimage") or shutil.which("mkisofs")


def make_psexe(text: bytes, t_addr: int = 0x80010000, pc0: int = 0x80010000) -> bytes:
    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<10I", header, 0x10, pc0, 0, t_addr, len(text), 0, 0, 0, 0, 0x801FFFF0, 0)
    header[0x4C : 0x4C + 21] = b"Sony Computer Test Ex"
    return bytes(header) + text


def bcd(v: int) -> int:
    return (v // 10) << 4 | v % 10


def to_mode2_raw(iso: bytes, form2_lbas: set[int]) -> bytes:
    """Wrap 2048-byte ISO sectors into raw 2352-byte Mode 2 sectors (EDC/ECC left zero)."""
    out = bytearray()
    for lba in range(len(iso) // 2048):
        frames = lba + 150
        header = bytes([bcd(frames // 4500), bcd(frames // 75 % 60), bcd(frames % 75), 2])
        submode = 0x24 if lba in form2_lbas else 0x08  # audio+form2 vs data
        sub = bytes([1, 0, submode, 0]) * 2
        data = iso[lba * 2048 : (lba + 1) * 2048]
        payload = data.ljust(2324 if lba in form2_lbas else 2048, b"\0")
        sector = extract_disc.SYNC + header + sub + payload
        out += sector.ljust(2352, b"\0")
    return bytes(out)


@unittest.skipUnless(MKISOFS, "genisoimage/mkisofs not installed")
class ExtractDiscTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        root = self.tmp / "root"
        (root / "DATA").mkdir(parents=True)
        self.text = bytes(range(256)) * 16
        (root / "SYSTEM.CNF").write_text("BOOT = cdrom:\\SLPS_999.99;1\r\nTCB = 4\r\nSTACK = 801fff00\r\n")
        (root / "SLPS_999.99").write_bytes(make_psexe(self.text))
        (root / "DATA" / "CARDS.BIN").write_bytes(b"card" * 1000)
        (root / "MOVIE.STR").write_bytes(b"\x55" * 4096)
        iso = self.tmp / "t.iso"
        subprocess.run([MKISOFS, "-quiet", "-o", str(iso), str(root)], check=True)
        iso_bytes = iso.read_bytes()

        # Find MOVIE.STR's extent so its sectors can be marked Form 2 like a real XA/STR stream.
        from io import BytesIO

        class _Iso(extract_disc.SectorReader):
            def __init__(self) -> None:  # noqa: D401 - minimal in-memory reader
                self._f = BytesIO(iso_bytes)
                self.sector_size = 2048
                self.sector_count = len(iso_bytes) // 2048

        movie = next(f for f in extract_disc.walk_filesystem(_Iso()) if f.path == "MOVIE.STR")
        self.movie_lbas = set(range(movie.lba, movie.lba + movie.sectors))
        bin_path = self.tmp / "t.bin"
        bin_path.write_bytes(to_mode2_raw(iso_bytes, self.movie_lbas))
        self.cue = self.tmp / "t.cue"
        self.cue.write_text('FILE "t.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp)

    def test_extracts_filesystem_boot_exe_and_xa(self) -> None:
        out = self.tmp / "out"
        self.assertEqual(extract_disc.main([str(self.cue), "-o", str(out)]), 0)

        m = json.loads((out / "manifest.json").read_text())
        self.assertEqual(m["system_cnf"]["BOOT"], "cdrom:\\SLPS_999.99;1")
        self.assertEqual(m["boot_exe"]["file"], "SLPS_999.99")
        self.assertEqual(m["boot_exe"]["t_addr"], "0x80010000")
        self.assertEqual(m["boot_exe"]["problems"], [])

        self.assertEqual((out / "fs" / "DATA" / "CARDS.BIN").read_bytes(), b"card" * 1000)
        self.assertEqual((out / "exe" / "boot.text").read_bytes(), self.text)

        movie = next(f for f in m["files"] if f["path"] == "MOVIE.STR")
        self.assertEqual(movie["storage"], "raw2352")
        raw = (out / "fs" / "MOVIE.STR.raw2352").read_bytes()
        self.assertEqual(len(raw), 2352 * len(self.movie_lbas))
        self.assertEqual(raw[18] & extract_disc.SUBMODE_FORM2, extract_disc.SUBMODE_FORM2)

    def test_refuses_to_overwrite_without_force(self) -> None:
        out = self.tmp / "out"
        out.mkdir()
        self.assertEqual(extract_disc.main([str(self.cue), "-o", str(out)]), 1)

    def test_boot_path_normalisation(self) -> None:
        self.assertEqual(extract_disc.boot_path({"BOOT": "cdrom:\\SLUS_013.28;1"}), "SLUS_013.28")
        self.assertEqual(extract_disc.boot_path({"BOOT": "cdrom:SLPS_031.01;1 arg"}), "SLPS_031.01")
        self.assertEqual(extract_disc.boot_path({}), "PSX.EXE")


if __name__ == "__main__":
    unittest.main()
