"""Text catalog: game strings paired with their translation (config/<serial>/text/catalog.txt).

The catalog lists ids and offsets only; the text comes from the player's own dumps. build()
writes, into assets/<serial>/text/:

  source.tsv   id<TAB>JP template (Shift-JIS bytes, escaped): what the renderer matches
  en.tsv       id<TAB>English template (US text, or the port's own en.tsv next to the catalog)

Other languages are more <lang>.tsv files with the same ids (DCB_LANG=<lang> picks one).

Template syntax, shared by every file: printf placeholders (%d, %3d, %c, %s, %%) the game fills
in; the memory-card slot digit the game writes over "S"/"E" after スロット (US: "*S"/"*E")
becomes %c, and the card count it writes over "??" before 枚 (two characters, space-padded)
%2d. Escapes: \\n line break, \\t tab, \\\\ backslash.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

_SLOT_JP = re.compile(rb"(\x83\x67)([SE])")  # ト then the slot letter
_SLOT_US = re.compile(rb"\*([SE])")
# "??枚": OPENSEG 801EA4F4 writes a card count (1-99) over the two "?" as " n" / "nn" (the old-save
# conversion's "??枚のカードデータの修復に成功しました。"), so the string drawn is "%2d枚...".
_COUNT_JP = re.compile(rb"\?\?(?=\x96\x87)")


@dataclass
class Pair:
    id: str
    us: str | None  # US id, or None: the English comes from the port's en.tsv


def parse(text: str) -> list[tuple[str, str | None, int]]:
    """Catalog lines -> (id, us id or None, count). Comments (#) and blank lines skipped."""
    out: list[tuple[str, str | None, int]] = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        kind = line[0]
        if kind == "pair" and len(line) == 3:
            out.append((line[1], None if line[2] == "-" else line[2], 1))
        elif kind == "run" and len(line) == 4:
            out.append((line[1], line[2], int(line[3])))
        else:
            raise ValueError(f"catalog line {n}: {raw.strip()!r}")
    return out


def split_id(ident: str) -> tuple[str, int]:
    file, off = ident.split(":")
    return file, int(off, 16)


def read_string(blob: bytes, off: int) -> tuple[bytes, int]:
    """The NUL-terminated string at off, and the offset of the next one (after the padding)."""
    end = blob.index(b"\0", off)
    nxt = end + 1
    while nxt < len(blob) and blob[nxt] == 0:
        nxt += 1
    return blob[off:end], nxt


def jp_template(raw: bytes) -> bytes:
    return _COUNT_JP.sub(rb"%2d", _SLOT_JP.sub(rb"\1%c", raw))


def us_template(raw: bytes) -> bytes:
    return _SLOT_US.sub(rb"%c", raw)


def escape(b: bytes) -> bytes:
    return b.replace(b"\\", b"\\\\").replace(b"\n", b"\\n").replace(b"\t", b"\\t")


def build(catalog_text: str, jp_file: Callable[[str], bytes], us_file: Callable[[str], bytes],
          own_en: dict[str, bytes]) -> tuple[list[tuple[str, bytes]], list[tuple[str, bytes]], list[str]]:
    """(source rows, en rows, problems). jp_file/us_file map a file name (EXE, OPENSEG) to bytes."""
    source: list[tuple[str, bytes]] = []
    en: list[tuple[str, bytes]] = []
    problems: list[str] = []
    for ident, us_ident, count in parse(catalog_text):
        file, off = split_id(ident)
        jp_blob = jp_file(file)
        if us_ident is not None:
            us_name, us_off = split_id(us_ident)
            us_blob = us_file(us_name)
        for _ in range(count):
            jp_raw, next_off = read_string(jp_blob, off)
            sid = f"{file}:{off:x}"
            source.append((sid, jp_template(jp_raw)))
            us_raw = None
            if us_ident is not None:  # a run's US side advances even past an own-English entry
                us_raw, us_off = read_string(us_blob, us_off)
            if sid in own_en:
                en.append((sid, own_en[sid]))
            elif us_raw is not None:
                en.append((sid, us_template(us_raw)))
            else:
                problems.append(f"{sid}: no US pair and no entry in the port's en.tsv")
            off = next_off
    return source, en, problems


def read_own(path: Path) -> dict[str, bytes]:
    """The port's own en.tsv (id<TAB>text, # comments), text unescaped to raw bytes later."""
    own: dict[str, bytes] = {}
    if not path.exists():
        return own
    for line in path.read_bytes().splitlines():
        if not line.strip() or line.startswith(b"#"):
            continue
        ident, _, text = line.partition(b"\t")
        own[ident.decode()] = text  # already in escaped form
    return own


def write_rows(path: Path, rows: list[tuple[str, bytes]], escaped: set[str] = frozenset()) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(ident.encode() + b"\t" + (text if ident in escaped else escape(text)) + b"\n"
                              for ident, text in rows))
