#!/usr/bin/env python3
"""Put the US (SLUS-01328) images into the JP (SLPS-03101) texture folder, where the layout matches.

    tools/assets/swap_us_images.py            # dry run: what would change, and what is skipped
    tools/assets/swap_us_images.py --apply    # copy, backing up every JP PNG it replaces
    tools/assets/swap_us_images.py --restore  # put the backed-up JP PNGs back

Both games must be ripped first (dcb_asset_ripper), so assets/converted/<serial>/ holds each
game's PNGs and manifest, and extracted/<serial>/fs/ holds the DRVs. Then re-pack
(dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak).

Pairing works on the disc data, not on file names: every texture belongs to one DRV entry (found
from its manifest drv_offset), and an entry is swapped only when its whole TIM layout is the same
in both games (the same set of images by pixel size, bit depth, VRAM position, palette position
and rows, and palette variants). Images are paired by that shape, since the US build sometimes
reorders a container; among images sharing a shape, identical ones (same content hash) pair
first and the rest pair in file order. An entry whose layout differs (the
title, the MATCH/WIN name plates, reordered city plates) is listed and left alone; those need a
per-image decision (see sprites.txt and the manifest's slot_w/slot_h in the README).

SYSTEM.TIM is never swapped: its US version replaces the kana font rows the JP text engine draws
from and moves the icons (docs/re/text-engine.md). The US PNGs are converted against the JP
palette at run time (the manifest's "pal"), so colours follow the JP palette. The ripper stores
identical images once, so the JP-only D-1 Grand Prix DRVs (W-Z), which reuse many of these
images, pick up the same US PNGs.

Only local, gitignored files are touched: assets/converted/SLPS-03101/ and the backup folder
assets/SLPS-03101/backup/us_images/.
"""

from __future__ import annotations

import argparse
import bisect
import filecmp
import json
import re
import shutil
import struct
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
import drv_unpack  # noqa: E402

JP, US = "SLPS-03101", "SLUS-01328"
# The same-geometry groups of docs/HYBRID_EN_ASSETS.md section 5 (DRV, entry path regex).
DEFAULT_ENTRIES = [
    ("A.DRV", r"BATTLE\.PAK"),
    ("B.DRV", r"(M_CARD|P_CARD|PARTNER|OPENING|FRIEND|TRADE|BCARD|CBTL_SYS)\.ARC"),
    ("B.DRV", r"CARD/LC\d+\.TIM"),
    ("E.DRV", r"\d+\.PAK"),
    ("F.DRV", r"\d+\.PAK"),
]
NEVER = re.compile(r"(^|/)SYSTEM\.TIM$")
# Entries where a few images changed shape: swap the images whose shape still matches, keep the
# rest JP. CBTL_SYS: one palette is uploaded as 32x1 in the US build instead of 16x2.
PARTIAL = {"B.DRV:CBTL_SYS.ARC"}
PAL_RE = re.compile(r"_pal(\d+)\.png$")


@dataclass(frozen=True)
class Tim:
    """One TIM as the game uploads it: the shape that must match between the two games."""
    offset: int                  # absolute offset in the DRV
    bpp: int
    image: tuple[int, int, int, int]  # VRAM x, y, w, h (16-bit units)
    clut: tuple[int, int, int, int] | None


def read_tim(drv: bytes, off: int) -> Tim | None:
    if drv[off:off + 4] != b"\x10\x00\x00\x00":
        return None
    flags = struct.unpack_from("<I", drv, off + 4)[0]
    p = off + 8
    clut = None
    if flags & 8:
        ln, x, y, w, h = struct.unpack_from("<IHHHH", drv, p)
        clut = (x, y, w, h)
        p += ln
    _, x, y, w, h = struct.unpack_from("<IHHHH", drv, p)
    return Tim(off, {0: 4, 1: 8, 2: 16, 3: 24}[flags & 3], (x, y, w, h), clut)


def load_side(serial: str) -> tuple[dict[str, bytes], dict[tuple[str, int], list[dict]]]:
    """DRV bytes by name, and manifest entries grouped by (drv, drv_offset)."""
    manifest = json.loads((ROOT / "assets" / "converted" / serial / "assets_manifest.json").read_text())
    by_tim: dict[tuple[str, int], list[dict]] = defaultdict(list)
    for e in manifest["entries"]:
        if e.get("drv") and "drv_offset" in e:
            by_tim[(e["drv"], e["drv_offset"])].append(e)
    drvs = {d: (ROOT / "extracted" / serial / "fs" / d).read_bytes() for d in {k[0] for k in by_tim}}
    return drvs, by_tim


def variant_index(e: dict) -> int:
    m = PAL_RE.search(e["path"])
    return int(m.group(1)) if m else -1


def copy_with_backup(dst: Path, src: Path, keep: Path) -> None:
    """Replace dst with src; the first backup of dst is the JP original and is never overwritten."""
    if not keep.exists():
        keep.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dst, keep)
    shutil.copy2(src, dst)


def variants(entries: list[dict]) -> list[dict]:
    """Palette variants of one TIM, sorted by palette index (_palN), unsuffixed first."""
    return sorted(entries, key=variant_index)


def entry_tims(drv: bytes, toc: list, by_tim: dict, drv_name: str) -> dict[str, list[tuple[Tim, list[dict]]]]:
    """For each DRV entry: its ripped TIMs in file order, with their manifest variants."""
    spans = sorted((f.offset, f.offset + f.size, f.path) for f in toc)
    out: dict[str, list[tuple[Tim, list[dict]]]] = defaultdict(list)
    starts = [s[0] for s in spans]
    for (d, off), entries in by_tim.items():
        if d != drv_name:
            continue
        i = bisect.bisect_right(starts, off) - 1
        if i < 0 or not (spans[i][0] <= off < spans[i][1]):
            continue
        tim = read_tim(drv, off)
        if tim is not None:
            out[spans[i][2]].append((tim, variants(entries)))
    for path in out:
        out[path].sort(key=lambda t: t[0].offset)
    return out


def shape_of(tim: Tim, var: list[dict]) -> tuple:
    return (tim.bpp, tim.image, tim.clut, len(var), tuple((e["w"], e["h"]) for e in var))


def pair(a: list[tuple[Tim, list[dict]]], b: list[tuple[Tim, list[dict]]], partial: bool = False):
    """Pair the TIMs of one entry by shape. The US build sometimes reorders a container's TIMs
    (the attack-name TIM moved to the end of every E PAK), so order alone is not enough; TIMs
    that share a shape pair by identical content first, then in file order. Returns (pairs,
    shared) or None when the two sets of shapes differ; `shared` counts the pairs made by file
    order inside a group of equal shapes (worth a spot check)."""
    groups_a: dict[tuple, list] = defaultdict(list)
    groups_b: dict[tuple, list] = defaultdict(list)
    for t, v in a:
        groups_a[shape_of(t, v)].append(v)
    for t, v in b:
        groups_b[shape_of(t, v)].append(v)
    if partial:  # keep only the shapes both sides have the same number of
        common = {k for k in groups_a if len(groups_a[k]) == len(groups_b.get(k, []))}
        groups_a = {k: v for k, v in groups_a.items() if k in common}
    elif {k: len(v) for k, v in groups_a.items()} != {k: len(v) for k, v in groups_b.items()}:
        return None
    pairs, shared = [], 0
    for k, va in groups_a.items():
        vb = list(groups_b[k])
        if len(va) == 1:
            pairs.append((va[0], vb[0]))
            continue
        # Images unchanged between the builds (same content hash) pair with each other first:
        # the US build also swaps equal-shape images inside a PAK (E 541, 592, 829).
        rest_a = []
        for v in va:
            same = next((w for w in vb if w[0]["img"] == v[0]["img"]), None)
            if same is not None:
                vb.remove(same)
                pairs.append((v, same))
            else:
                rest_a.append(v)
        shared += len(rest_a) if len(rest_a) > 1 else 0
        pairs.extend(zip(rest_a, vb))
    return pairs, shared


def differing_pixels(a: Path, b: Path) -> float:
    """Fraction of pixels that differ between two PNGs of the same size (ImageMagick)."""
    r = subprocess.run(["magick", "compare", "-metric", "AE", str(a), str(b), "null:"],
                       capture_output=True, text=True)
    count = float((r.stderr.split() or ["inf"])[0])
    w, h = (int(x) for x in subprocess.run(["magick", "identify", "-format", "%w %h", str(a)],
                                            capture_output=True, text=True).stdout.split())
    return count / max(1, w * h)


def resolve(original: Path, jp_path: str, cands: list[tuple[Path, str]]) -> tuple[Path, str, str]:
    """Pick one US image for a JP PNG that several entries map to. The US build left some copies
    untranslated (the same picture as the JP original): drop those, then prefer the entry the JP
    file is named after (E_800_... -> 800.PAK)."""
    translated = [(src, label) for src, label in cands if differing_pixels(original, src) > 0.02]
    pool = translated or cands
    own = re.match(r"[A-Z]_(.+?)_off", Path(jp_path).name)
    pick = next((c for c in pool if own and Path(c[1].split(":", 1)[1]).stem == own.group(1)), pool[0])
    others = [label for _, label in cands if label != pick[1]]
    kind = "untranslated copy dropped" if len(translated) == 1 else f"{len(translated)} translations"
    return pick[0], pick[1], f"{pick[1]} ({kind}; also {', '.join(others)})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="copy the US PNGs (default: dry run)")
    ap.add_argument("--restore", action="store_true", help="put the backed-up JP PNGs back")
    ap.add_argument("--only", help="regex on 'DRV:entry path' to limit the run (e.g. 'B.DRV:M_CARD')")
    args = ap.parse_args()

    jp_dir = ROOT / "assets" / "converted" / JP
    backup = ROOT / "assets" / JP / "backup" / "us_images"

    if args.restore:
        restored = 0
        for f in sorted(backup.rglob("*.png")):
            shutil.copy2(f, jp_dir / f.relative_to(backup))
            restored += 1
        print(f"restored {restored} JP images from {backup.relative_to(ROOT)}")
        return 0

    jp_drvs, jp_by = load_side(JP)
    us_drvs, us_by = load_side(US)
    only = re.compile(args.only) if args.only else None
    skipped, shared_entries = [], []
    # Plan first: the ripper stores identical images once, so one JP PNG can be the target of
    # several pairs (the same attack name in two PAKs) whose US images differ.
    plan: dict[str, list[tuple[Path, str]]] = defaultdict(list)  # JP PNG -> [(US PNG, entry label)]

    for drv_name, pattern in DEFAULT_ENTRIES:
        rx = re.compile(pattern + "$")
        if drv_name not in jp_drvs or drv_name not in us_drvs:
            continue
        jp_toc, _ = drv_unpack.read_toc(jp_drvs[drv_name])
        us_toc, _ = drv_unpack.read_toc(us_drvs[drv_name])
        jp_ents = entry_tims(jp_drvs[drv_name], jp_toc, jp_by, drv_name)
        us_ents = entry_tims(us_drvs[drv_name], us_toc, us_by, drv_name)
        for path in sorted(jp_ents):
            label = f"{drv_name}:{path}"
            if not rx.search(path) or NEVER.search(path) or (only and not only.search(label)):
                continue
            if path not in us_ents:
                skipped.append((label, "no US entry"))
                continue
            a, b = jp_ents[path], us_ents[path]
            paired = pair(a, b, partial=label in PARTIAL)
            if paired is None:
                skipped.append((label, f"layout differs ({len(a)} vs {len(b)} images)"))
                continue
            pairs, shared = paired
            if shared:
                shared_entries.append(label)
            for jv, uv in pairs:
                for je, ue in zip(jv, uv):
                    src = ROOT / "assets" / "converted" / US / ue["path"]
                    if src.is_file() and (jp_dir / je["path"]).is_file() and all(src != c for c, _ in plan[je["path"]]):
                        plan[je["path"]].append((src, label))

    swapped = same = 0
    per_entry: dict[str, int] = defaultdict(int)
    conflicts = []
    for jp_path, cands in sorted(plan.items()):
        dst = jp_dir / jp_path
        original = backup / jp_path if (backup / jp_path).is_file() else dst
        if len(cands) > 1:
            src, label, note = resolve(original, jp_path, cands)
            conflicts.append((jp_path, note))
        else:
            src, label = cands[0]
        if filecmp.cmp(src, dst, shallow=False):
            same += 1
            continue
        swapped += 1
        per_entry[label] += 1
        if args.apply:
            copy_with_backup(dst, src, backup / jp_path)
    entries_done = sorted(per_entry.items())

    verb = "swapped" if args.apply else "would swap"
    print(f"{verb} {swapped} images in {len(entries_done)} entries ({same} already identical)")
    groups: dict[str, list[int]] = defaultdict(list)
    for label, n in entries_done:
        groups[re.sub(r"\d+", "#", label)].append(n)
    for g, ns in sorted(groups.items()):
        print(f"  {g}: {len(ns)} entries, {sum(ns)} images")
    if conflicts:
        print(f"{len(conflicts)} JP images are shared by entries whose US images differ (one PNG, one pick):")
        for jp_path, note in conflicts:
            print(f"  {Path(jp_path).name}: {note}")
    if shared_entries:
        print(f"{len(shared_entries)} entries have several images of the same shape, paired in file order "
              f"(spot-check these), e.g. {', '.join(shared_entries[:4])}")
    if skipped:
        print(f"left alone ({len(skipped)} entries):")
        reasons: dict[str, list[str]] = defaultdict(list)
        for label, why in skipped:
            reasons[why.split(" (")[0]].append(label)
        for why, labels in sorted(reasons.items()):
            shown = ", ".join(labels[:6]) + (f", ... (+{len(labels) - 6})" if len(labels) > 6 else "")
            print(f"  {why}: {shown}")
    if args.apply and swapped:
        print(f"JP originals backed up in {backup.relative_to(ROOT)}; re-pack the assets to use them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
