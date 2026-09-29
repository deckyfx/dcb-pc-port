"""Short card names for the mini font (config/<serial>/text/short-names.tsv).

A few US card names are wider than the battle card panel's name slot even in tight spacing; the
runtime (src/game/overrides/mini_text.cpp) draws our own short name there instead, and only
there. The config file is keyed by card number (with the full US name as a check column); the
runtime matches the name it is about to draw, so this module resolves each number to its US
name in CARD2.CDD and writes assets/<serial>/en_short_names.txt: "<full>\\t<short>" per line.

Config format: "<card number>\\t<short name>[\\t<full US name>]" per line; '#' comments and
blank lines are ignored. A wrong number, a full-name column that does not match the disc, a
short name longer than the 20-byte name slot, or a number listed twice is an error.
"""
from __future__ import annotations

import struct
from pathlib import Path

NAME_MAX = 20  # CARD2.CDD name field: 21 bytes with the NUL

# US CARD2.CDD record layout (en_text.graft_cdd): 8-byte header, then Digimon / items /
# options, name[21] at +3 of each record.
_US_STRIDES = (0x13C, 0xE2, 0x70)


def us_card_names(us_cdd: bytes) -> list[bytes]:
    """Every card name of the US CARD2.CDD, by card number (Digimon, items, options)."""
    counts = struct.unpack_from("<HBB", us_cdd, 4)
    names: list[bytes] = []
    base = 8
    for count, stride in zip(counts, _US_STRIDES):
        for i in range(count):
            off = base + i * stride + 3
            names.append(us_cdd[off:off + NAME_MAX + 1].split(b"\0", 1)[0])
        base += count * stride
    return names


def parse(text: str) -> tuple[list[tuple[int, str, str | None]], list[str]]:
    """Rows (number, short, full or None) of a short-names.tsv, and the format problems."""
    rows: list[tuple[int, str, str | None]] = []
    problems: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) not in (2, 3) or not cols[0].strip().isdigit() or not cols[1]:
            problems.append(f"line {lineno}: expected <number>\\t<short name>[\\t<full name>]")
            continue
        rows.append((int(cols[0]), cols[1], cols[2] if len(cols) == 3 else None))
    return rows, problems


def build(text: str, names: list[bytes]) -> tuple[list[tuple[bytes, bytes]], list[str]]:
    """(full, short) pairs for the runtime file, and the problems found."""
    rows, problems = parse(text)
    pairs: list[tuple[bytes, bytes]] = []
    seen: set[int] = set()
    for number, short, full in rows:
        if number in seen:
            problems.append(f"card {number}: listed twice")
            continue
        seen.add(number)
        if number >= len(names):
            problems.append(f"card {number}: no such card ({len(names)} cards)")
            continue
        name = names[number]
        if full is not None and full.encode("ascii", "replace") != name:
            problems.append(f"card {number}: full name {full!r} does not match the disc")
            continue
        try:
            short_b = short.encode("ascii")
        except UnicodeEncodeError:
            problems.append(f"card {number}: short name {short!r} is not ASCII")
            continue
        if len(short_b) > NAME_MAX:
            problems.append(f"card {number}: short name {short!r} longer than {NAME_MAX} bytes")
            continue
        pairs.append((name, short_b))
    return pairs, problems


def write(config: Path, us_cdd: bytes, out: Path) -> tuple[int, list[str]]:
    """Writes out/en_short_names.txt from `config` (missing: an empty file). Returns the count
    and the problems."""
    text = config.read_text(encoding="utf-8") if config.exists() else ""
    pairs, problems = build(text, us_card_names(us_cdd))
    (out / "en_short_names.txt").write_bytes(b"".join(full + b"\t" + short + b"\n" for full, short in pairs))
    return len(pairs), problems
