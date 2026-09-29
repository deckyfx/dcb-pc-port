"""Community fixes (.xdelta) for the game, applied to the SLPS-03101 build.

The fixes on romhacking.net target the US disc image. Here that image is only our reference
data, so a fix works in two ways:
  - it corrects the reference data we take English from (card text, overlay strings, city
    scripts): the converter reads the fixed files instead of the extracted ones;
  - a data change in an overlay (a table, not text) is ported into the SLPS overlay: the same
    bytes are found there by their unchanged surroundings and changed the same way.
The .xdelta files are the authors' work: the player downloads them and puts them in
assets/SLPS-03101/fixes/ (never in git). Each must match the disc it was made for; they are
applied to the untouched image independently and merged (overlapping changes are refused).
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import vcdiff

RAW_SECTOR, DATA_OFFSET, DATA_SIZE = 2352, 24, 2048


@dataclass
class Fixed:
    files: dict[str, bytes] = field(default_factory=dict)  # disc path -> fixed file bytes
    applied: list[str] = field(default_factory=list)       # patch names
    problems: list[str] = field(default_factory=list)


def _file_bytes(image: bytes, lba: int, size: int) -> bytes:
    sectors = (size + DATA_SIZE - 1) // DATA_SIZE
    raw = b"".join(image[(lba + k) * RAW_SECTOR + DATA_OFFSET:(lba + k) * RAW_SECTOR + DATA_OFFSET + DATA_SIZE]
                   for k in range(sectors))
    return raw[:size]


def apply(reference_root: Path, repo: Path, patches: list[Path]) -> Fixed:
    """Apply every patch to the reference image; the files they change, fixed."""
    result = Fixed()
    if not patches:
        return result
    manifest = json.loads((reference_root / "manifest.json").read_text())
    image_path = repo / manifest["image"]
    original = image_path.read_bytes()
    merged = bytearray(original)
    owner: dict[int, str] = {}
    for patch in patches:
        try:
            out = vcdiff.decode(original, patch.read_bytes())
        except (ValueError, IndexError) as e:
            result.problems.append(f"{patch.name}: {e}")
            continue
        if len(out) != len(original):
            result.problems.append(f"{patch.name}: changes the image size (not a fix for this disc?)")
            continue
        clash = False
        changes = [i for i in range(0, len(out), RAW_SECTOR) if out[i:i + RAW_SECTOR] != original[i:i + RAW_SECTOR]]
        for start in changes:
            for i in range(start + DATA_OFFSET, start + DATA_OFFSET + DATA_SIZE):
                if out[i] != original[i]:
                    if i in owner and merged[i] != out[i]:
                        result.problems.append(f"{patch.name}: overlaps {owner[i]} at image byte {i:#x}")
                        clash = True
                        break
                    merged[i] = out[i]
                    owner[i] = patch.name
            if clash:
                break
        if not clash:
            result.applied.append(patch.name)
    changed_lbas = {i // RAW_SECTOR for i in owner}
    for f in manifest["files"]:
        if f.get("storage", "form1") != "form1":
            continue
        if not any(f["lba"] <= lba < f["lba"] + f["sectors"] for lba in changed_lbas):
            continue
        before = _file_bytes(original, f["lba"], f["size"])
        after = _file_bytes(bytes(merged), f["lba"], f["size"])
        if before != after:
            result.files[f["path"]] = after
    return result


def _runs(a: bytes, b: bytes) -> list[tuple[int, int]]:
    """[start, end) ranges where a and b differ."""
    runs, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]:
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    return runs


def port(before: bytes, after: bytes, target: bytes, min_context: int = 8, max_side: int = 16) -> tuple[bytes, list[str]]:
    """Carry the changes before -> after into `target`, a different build of the same file.

    Each changed run is located in `target` by the smallest window of `before` around it
    (at least `min_context` bytes, run included) that occurs exactly once there; runs with no
    such window (text, or code that moved) are reported and left alone.
    """
    out = bytearray(target)
    notes = []
    for start, end in _runs(before, after):
        placed = None
        for total in range(max(min_context, end - start), end - start + 2 * max_side + 1):
            for left in range(0, total - (end - start) + 1):
                right = total - (end - start) - left
                if left > max_side or right > max_side or start - left < 0 or end + right > len(before):
                    continue
                window = before[start - left:end + right]
                hit = target.find(window)
                if hit >= 0 and target.find(window, hit + 1) < 0:
                    placed = hit + left
                    break
            if placed is not None:
                break
        if placed is None:
            notes.append(f"{start:#x}: {end - start} byte(s) not found in the target build, left alone")
            continue
        out[placed:placed + end - start] = after[start:end]
        notes.append(f"{start:#x} -> {placed:#x}: {before[start:end].hex()} -> {after[start:end].hex()}")
    return bytes(out), notes
