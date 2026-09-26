#!/usr/bin/env python3
"""Extract the ISO9660 filesystem and boot PS-EXE from a PlayStation disc image.

Reads a raw ``MODE2/2352`` BIN (via its CUE or directly) or a plain 2048-byte ISO
and produces::

    <out>/
      manifest.json      disc identity, SYSTEM.CNF, every file (LBA, size, XA flags, SHA-1)
      system_area.bin    sectors 0-15 (license text / region data)
      fs/...             the filesystem tree
      exe/boot.exe       the boot PS-EXE named by SYSTEM.CNF, byte-for-byte
      exe/boot.text      its load image (file offset 0x800 onward), for a raw Ghidra import
      exe/boot.json      parsed PS-EXE header
      layout.txt         sector map for running the game from these files (see write_layout)
      iso_meta.bin       raw directory/system-area sectors referenced by layout.txt

Files whose sectors are Mode 2 Form 2 (XA-ADPCM audio, STR video) do not fit in 2048-byte
user data. They are written as raw 2352-byte sectors with a ``.raw2352`` suffix, so that
subheaders (channel, submode) survive for the asset tools. Tools that read only 2048 bytes
per sector (bchunk, 7z, most ISO mounters) silently corrupt these files.

Uses only the Python standard library.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterator

RAW_SECTOR = 2352
ISO_SECTOR = 2048
FORM2_DATA = 2324
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

# CD-XA directory-record attribute bits (big-endian word in the system use area).
XA_FORM1 = 0x0800
XA_FORM2 = 0x1000
XA_INTERLEAVED = 0x2000
XA_CDDA = 0x4000
XA_DIRECTORY = 0x8000

SUBMODE_FORM2 = 0x20

PSEXE_MAGIC = b"PS-X EXE"
PSEXE_HEADER_SIZE = 0x800


class DiscError(Exception):
    """Raised when the image is not a readable PlayStation disc."""


class SectorReader:
    """Random access to sectors of a raw (2352) or cooked (2048) image."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._f: BinaryIO = path.open("rb")
        size = path.stat().st_size
        head = self._f.read(16)
        if head[:12] == SYNC:
            self.sector_size = RAW_SECTOR
            if head[15] != 2:
                raise DiscError(f"data track is mode {head[15]}, expected mode 2 (CD-XA)")
        else:
            self.sector_size = ISO_SECTOR
        if size % self.sector_size:
            print(f"warning: image size {size} is not a multiple of {self.sector_size}", file=sys.stderr)
        self.sector_count = size // self.sector_size

    @property
    def is_raw(self) -> bool:
        return self.sector_size == RAW_SECTOR

    def close(self) -> None:
        self._f.close()

    def raw(self, lba: int) -> bytes:
        """Return the full 2352-byte sector (raw images only)."""
        if not self.is_raw:
            raise DiscError("raw sector access needs a 2352-byte image")
        self._check(lba)
        self._f.seek(lba * RAW_SECTOR)
        return self._f.read(RAW_SECTOR)

    def user(self, lba: int) -> bytes:
        """Return the 2048 bytes of Form 1 user data of a sector."""
        self._check(lba)
        if not self.is_raw:
            self._f.seek(lba * ISO_SECTOR)
            return self._f.read(ISO_SECTOR)
        # Mode 2: 12 sync + 4 header + 8 subheader, then data.
        return self.raw(lba)[24 : 24 + ISO_SECTOR]

    def is_form2(self, lba: int) -> bool:
        return self.is_raw and bool(self.raw(lba)[18] & SUBMODE_FORM2)

    def read_user(self, lba: int, count: int) -> bytes:
        return b"".join(self.user(lba + i) for i in range(count))

    def _check(self, lba: int) -> None:
        if not 0 <= lba < self.sector_count:
            raise DiscError(f"LBA {lba} outside image ({self.sector_count} sectors)")


@dataclass
class FileEntry:
    path: str
    lba: int
    size: int
    sectors: int
    xa_attributes: int | None
    storage: str = "form1"  # form1 | raw2352 | cdda
    sha1: str | None = None


def sectors_for(size: int) -> int:
    return (size + ISO_SECTOR - 1) // ISO_SECTOR


def clean_name(raw: bytes) -> str:
    """Turn an ISO9660 identifier into a safe path component."""
    name = raw.decode("ascii", errors="replace").split(";", 1)[0].rstrip(".")
    if not name or name in (".", "..") or any(c in name for c in "/\\\x00"):
        raise DiscError(f"refusing unsafe filename {raw!r}")
    return name


def iter_records(data: bytes) -> Iterator[bytes]:
    """Yield directory records; records never straddle a 2048-byte sector."""
    i = 0
    while i < len(data):
        length = data[i]
        if length == 0:
            i = (i // ISO_SECTOR + 1) * ISO_SECTOR
            continue
        yield data[i : i + length]
        i += length


def xa_attributes(record: bytes) -> int | None:
    name_len = record[32]
    su = 33 + name_len + (1 - name_len % 2)  # pad byte keeps the system use area even-aligned
    area = record[su:]
    if len(area) >= 14 and area[6:8] == b"XA":
        return struct.unpack_from(">H", area, 4)[0]
    return None


def walk_filesystem(reader: SectorReader, dirs: list[tuple[int, int]] | None = None) -> list[FileEntry]:
    """Every file on the disc, sorted by LBA. Directory extents (lba, sectors) go to `dirs`."""
    pvd = reader.user(16)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise DiscError("no ISO9660 primary volume descriptor at sector 16")
    if struct.unpack_from("<H", pvd, 128)[0] != ISO_SECTOR:
        raise DiscError("unsupported logical block size")

    root = pvd[156 : 156 + 34]
    files: list[FileEntry] = []
    seen: set[int] = set()
    stack = [(PurePosixPath(), struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])]

    while stack:
        parent, lba, size = stack.pop()
        if lba in seen:
            continue
        seen.add(lba)
        if dirs is not None:
            dirs.append((lba, sectors_for(size)))
        for rec in iter_records(reader.read_user(lba, sectors_for(size))):
            name_len = rec[32]
            ident = rec[33 : 33 + name_len]
            if ident in (b"\x00", b"\x01"):
                continue
            ext_lba = struct.unpack_from("<I", rec, 2)[0]
            ext_size = struct.unpack_from("<I", rec, 10)[0]
            path = parent / clean_name(ident)
            if rec[25] & 0x02:
                stack.append((path, ext_lba, ext_size))
            else:
                files.append(FileEntry(str(path), ext_lba, ext_size, sectors_for(ext_size), xa_attributes(rec)))
    files.sort(key=lambda f: f.lba)
    return files


def classify(reader: SectorReader, entry: FileEntry) -> None:
    attr = entry.xa_attributes or 0
    if attr & XA_CDDA:
        entry.storage = "cdda"
    elif attr & (XA_FORM2 | XA_INTERLEAVED) or any(
        reader.is_form2(entry.lba + i) for i in range(entry.sectors)
    ):
        entry.storage = "raw2352"
        if not reader.is_raw:
            print(f"warning: {entry.path} is XA/Form 2 but the image is cooked; data is incomplete", file=sys.stderr)
            entry.storage = "form1"


def extract_file(reader: SectorReader, entry: FileEntry, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    sha = hashlib.sha1()
    if entry.storage == "raw2352":
        dest = dest.with_name(dest.name + ".raw2352")
        with dest.open("wb") as out:
            for i in range(entry.sectors):
                chunk = reader.raw(entry.lba + i)
                sha.update(chunk)
                out.write(chunk)
    else:
        remaining = entry.size
        with dest.open("wb") as out:
            for i in range(entry.sectors):
                chunk = reader.user(entry.lba + i)[: min(ISO_SECTOR, remaining)]
                remaining -= len(chunk)
                sha.update(chunk)
                out.write(chunk)
    entry.sha1 = sha.hexdigest()
    return dest


def parse_system_cnf(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            values[key.strip().upper()] = value.strip()
    return values


def boot_path(cnf: dict[str, str]) -> str:
    """``cdrom:\\SLPS_031.01;1 arg`` -> ``SLPS_031.01``."""
    boot = cnf.get("BOOT", "cdrom:PSX.EXE;1").split()[0]
    boot = boot.split(":", 1)[-1].split(";", 1)[0]
    return boot.replace("\\", "/").lstrip("/")


def parse_psexe(data: bytes) -> dict[str, object]:
    if data[:8] != PSEXE_MAGIC:
        raise DiscError("boot file is not a PS-X EXE")
    fields = ("pc0", "gp0", "t_addr", "t_size", "d_addr", "d_size", "b_addr", "b_size", "s_addr", "s_size")
    hdr = dict(zip(fields, struct.unpack_from("<10I", data, 0x10)))
    marker = data[0x4C:PSEXE_HEADER_SIZE].split(b"\x00", 1)[0].decode("ascii", errors="replace")
    t_addr, t_size, pc0 = hdr["t_addr"], hdr["t_size"], hdr["pc0"]
    problems = []
    if len(data) < PSEXE_HEADER_SIZE + t_size:
        problems.append(f"file holds {len(data) - PSEXE_HEADER_SIZE:#x} bytes of text, header claims {t_size:#x}")
    if not 0x80000000 <= t_addr < 0x80200000:
        problems.append(f"load address {t_addr:#010x} is outside KSEG0 RAM")
    if not t_addr <= pc0 < t_addr + t_size:
        problems.append(f"entry {pc0:#010x} is outside the loaded text")
    out: dict[str, object] = {k: f"0x{v:08X}" for k, v in hdr.items()}
    out["region_marker"] = marker
    out["problems"] = problems
    return out


def resolve_image(path: Path) -> tuple[Path, list[str]]:
    """Return the data-track BIN for a CUE (or the image itself) and a list of track descriptions."""
    if path.suffix.lower() != ".cue":
        return path, []
    current_file: Path | None = None
    data_file: Path | None = None
    tracks: list[str] = []
    for line in path.read_text(errors="replace").splitlines():
        parts = line.strip().split()
        if not parts:
            continue
        if parts[0].upper() == "FILE":
            name = line.split('"')[1] if '"' in line else parts[1]
            current_file = path.parent / name
        elif parts[0].upper() == "TRACK" and len(parts) >= 3:
            tracks.append(f"{parts[1]} {parts[2]} {current_file.name if current_file else '?'}")
            if data_file is None and parts[2].upper().startswith("MODE"):
                data_file = current_file
    if data_file is None:
        raise DiscError(f"{path} has no data track")
    return data_file, tracks


def sha1_file(path: Path) -> str:
    sha = hashlib.sha1()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            sha.update(chunk)
    return sha.hexdigest()


def write_layout(reader: SectorReader, files: list[FileEntry], dirs: list[tuple[int, int]], out: Path) -> None:
    """Write what the runtime needs to rebuild any disc sector from the extracted files:

    * iso_meta.bin: raw sectors of everything no file covers (system area, volume descriptors,
      path tables, directories, gaps, post-gap);
    * layout.txt: one line per sector range, e.g.
        meta <lba> <count> <index into iso_meta.bin>
        file <lba> <sectors> <bytes> <form1|raw2352> <first subheader> <last subheader> <path>
    Form 1 file sectors are rebuilt from 2048-byte chunks with these subheaders; raw2352 files
    (XA/STR) are stored as whole sectors already. Needs a raw (2352-byte) image.
    """
    if not reader.is_raw:
        print("warning: cooked image: no layout.txt (the runtime needs raw sectors)", file=sys.stderr)
        return
    # Everything no file covers: system area, descriptors, path tables, directories, gaps and the
    # post-gap. Small (tens of sectors), and it makes rebuilt sectors match the disc exactly.
    covered = bytearray(reader.sector_count)
    for f in files:
        if f.storage != "cdda":
            covered[f.lba : f.lba + f.sectors] = b"\x01" * f.sectors
    for lba, n in dirs:
        covered[lba : lba + n] = b"\x00" * n
    meta_lbas = [lba for lba in range(reader.sector_count) if not covered[lba]]
    ranges: list[list[int]] = []
    for lba in meta_lbas:
        if ranges and ranges[-1][0] + ranges[-1][1] == lba:
            ranges[-1][1] += 1
        else:
            ranges.append([lba, 1])
    lines = ["# dcb extracted disc layout v1", f"sectors {reader.sector_count}"]
    with (out / "iso_meta.bin").open("wb") as meta:
        index = 0
        for lba, count in ranges:
            for i in range(count):
                meta.write(reader.raw(lba + i))
            lines.append(f"meta {lba} {count} {index}")
            index += count
    for f in files:
        if f.storage == "cdda" or f.sectors == 0:
            continue
        stored = f"fs/{f.path}" + (".raw2352" if f.storage == "raw2352" else "")
        first_sh = reader.raw(f.lba)[16:20].hex()
        last_sh = reader.raw(f.lba + f.sectors - 1)[16:20].hex()
        lines.append(f"file {f.lba} {f.sectors} {f.size} {f.storage} {first_sh} {last_sh} {stored}")
    (out / "layout.txt").write_text("\n".join(lines) + "\n")


def extract(image: Path, out: Path, force: bool) -> dict[str, object]:
    data_track, tracks = resolve_image(image)
    if out.exists():
        if not force:
            raise DiscError(f"{out} exists; pass --force to replace it")
        shutil.rmtree(out)
    out.mkdir(parents=True)

    reader = SectorReader(data_track)
    try:
        pvd = reader.user(16)
        (out / "system_area.bin").write_bytes(reader.read_user(0, 16))

        dirs: list[tuple[int, int]] = []
        files = walk_filesystem(reader, dirs)
        by_upper = {f.path.upper(): f for f in files}
        for entry in files:
            classify(reader, entry)
            if entry.storage != "cdda":
                extract_file(reader, entry, out / "fs" / entry.path)
        write_layout(reader, files, dirs, out)

        cnf_entry = by_upper.get("SYSTEM.CNF")
        cnf = parse_system_cnf((out / "fs" / cnf_entry.path).read_text(errors="replace")) if cnf_entry else {}
        boot = by_upper.get(boot_path(cnf).upper())
        if boot is None:
            raise DiscError(f"boot executable {boot_path(cnf)!r} not found on disc")

        exe_bytes = (out / "fs" / boot.path).read_bytes()
        exe_info = parse_psexe(exe_bytes)
        exe_info["file"] = boot.path
        exe_dir = out / "exe"
        exe_dir.mkdir()
        (exe_dir / "boot.exe").write_bytes(exe_bytes)
        t_size = int(str(exe_info["t_size"]), 16)
        (exe_dir / "boot.text").write_bytes(exe_bytes[PSEXE_HEADER_SIZE : PSEXE_HEADER_SIZE + t_size])
        (exe_dir / "boot.json").write_text(json.dumps(exe_info, indent=2) + "\n")

        # Additional executables are the first place to look for code overlays.
        others = []
        for entry in files:
            if entry is boot or entry.storage != "form1" or entry.size < PSEXE_HEADER_SIZE:
                continue
            with (out / "fs" / entry.path).open("rb") as f:
                if f.read(8) == PSEXE_MAGIC:
                    others.append(entry.path)

        manifest = {
            "image": str(data_track),
            "image_sha1": sha1_file(data_track),
            "sector_size": reader.sector_size,
            "sector_count": reader.sector_count,
            "tracks": tracks,
            "volume_id": pvd[40:72].decode("ascii", errors="replace").strip(),
            "system_id": pvd[8:40].decode("ascii", errors="replace").strip(),
            "system_cnf": cnf,
            "boot_exe": exe_info,
            "other_executables": others,
            "files": [asdict(f) for f in files],
        }
    finally:
        reader.close()
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("image", type=Path, help=".cue, raw .bin (2352) or .iso (2048)")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    ap.add_argument("--force", action="store_true", help="replace an existing output directory")
    args = ap.parse_args(argv)

    try:
        m = extract(args.image, args.out, args.force)
    except DiscError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    exe = m["boot_exe"]
    files = m["files"]
    raw = [f["path"] for f in files if f["storage"] == "raw2352"]
    print(f"volume     {m['volume_id']}  ({m['sector_count']} sectors, sha1 {m['image_sha1']})")
    print(f"boot       {exe['file']}  [{exe['region_marker']}]")
    print(f"entry pc0  {exe['pc0']}   gp0 {exe['gp0']}")
    print(f"text       {exe['t_addr']} + {exe['t_size']}   stack {exe['s_addr']}")
    print(f"files      {len(files)} ({len(raw)} kept as raw 2352 XA/STR)")
    for path in raw:
        print(f"  raw      {path}")
    for path in m["other_executables"]:
        print(f"  exe      {path}")
    for problem in exe["problems"]:
        print(f"warning: {problem}", file=sys.stderr)
    gp = "set by crt0 (header gp0 is 0)" if int(str(exe["gp0"]), 16) == 0 else exe["gp0"]
    print(
        f"\nGhidra: import {args.out / 'exe' / 'boot.exe'} (ghidra_psx_ldr), or raw {args.out / 'exe' / 'boot.text'}\n"
        f"  language PSX:LE:32:default (ghidra_psx_ldr), base {exe['t_addr']}, entry {exe['pc0']}, gp {gp}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
