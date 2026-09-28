#!/usr/bin/env python3
"""Show the US (SLUS-01328) images in the JP (SLPS-03101) game, where the layout matches.

    tools/assets/swap_us_images.py            # dry run: what would change, and what is skipped
    tools/assets/swap_us_images.py --apply    # write the US data and point the JP manifest at it
    tools/assets/swap_us_images.py --restore  # undo: the JP manifest as it was, no US data

Both games must be ripped first (dcb_asset_ripper), so assets/converted/<serial>/ holds each
game's manifest, and extracted/<serial>/fs/ holds the DRVs. Then re-pack
(dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak).

The US data goes in exactly as the US disc has it: each changed image becomes a ".raw" file with
the US pixel indices (assets/converted/SLPS-03101/us/<JP image hash>.raw), and the JP manifest
entry for that image points at it, so the texture replacer uploads those bytes as they are. Where
the US build also changed the image's palette, the JP palette upload is replaced by the US one
the same way. Nothing goes through a PNG, so colours are the US colours and palette animation
(the attack glow) keeps working. The JP PNGs are never touched.

Pairing works on the disc data, not on file names: every texture belongs to one DRV entry (found
from its manifest drv_offset), and an entry is swapped only when both games have the same set of
images in it (pixel size, bit depth, VRAM position, palette position and rows, palette variants).
Images are paired by that shape, since the US build reorders containers (the attack name moved
to the end of every E PAK); among images of one shape, identical ones pair first, the rest in
file order. Entries whose layout differs (the title, the MATCH/WIN name plates) are listed and
left alone. Images identical on both discs are skipped.

One JP image can stand for several US ones (the same attack name in two PAKs, translated in one
and left in Japanese in the other): the US copies identical to the JP data are dropped, then the
entry the JP file is named after wins. A JP palette shared with regular-game images that are not
swapped is left alone, so nothing there changes colour; JP-only images (the D-1 Grand Prix,
attacks the US build dropped) sharing a swapped palette take the US colours. All are listed.

SYSTEM.TIM is never swapped: its US version replaces the kana font rows the JP text engine draws
from and moves the icons (docs/re/text-engine.md). Only local, gitignored files are touched:
assets/converted/SLPS-03101/ (manifest, us/) and the backup at assets/SLPS-03101/backup/us_images/.
Edit the manifest by hand only after --restore, or the next --restore undoes the edit.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import shutil
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
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
JP_ONLY_DRVS = {"W.DRV", "X.DRV", "Y.DRV", "Z.DRV"}  # D-1 Grand Prix: not in the US build
RAW_DIR = "us"


def fnv1a64(data: bytes) -> str:
    """The replacer's upload hash (vfs::fnv1a64), as the manifest's 16 hex digits."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


@dataclass(frozen=True)
class Tim:
    """One TIM as the game uploads it: the shape that must match between the two games."""
    offset: int                  # absolute offset in the DRV
    bpp: int
    image: tuple[int, int, int, int]  # VRAM x, y, w, h (16-bit units)
    clut: tuple[int, int, int, int] | None
    pixels: bytes = field(default=b"", compare=False, repr=False)   # image block payload
    palette: bytes = field(default=b"", compare=False, repr=False)  # CLUT block payload


def read_tim(drv: bytes, off: int) -> Tim | None:
    if drv[off:off + 4] != b"\x10\x00\x00\x00":
        return None
    flags = struct.unpack_from("<I", drv, off + 4)[0]
    p = off + 8
    clut, palette = None, b""
    if flags & 8:
        ln, x, y, w, h = struct.unpack_from("<IHHHH", drv, p)
        clut, palette = (x, y, w, h), drv[p + 12:p + ln]
        p += ln
    ln, x, y, w, h = struct.unpack_from("<IHHHH", drv, p)
    return Tim(off, {0: 4, 1: 8, 2: 16, 3: 24}[flags & 3], (x, y, w, h), clut, drv[p + 12:p + ln], palette)


def load_side(serial: str, manifest: dict | None = None) -> tuple[dict[str, bytes], dict[tuple[str, int], list[dict]]]:
    """DRV bytes by name, and manifest entries grouped by (drv, drv_offset)."""
    if manifest is None:
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


def entry_tims(drv: bytes, toc: list, by_tim: dict, drv_name: str) -> dict[str, list[tuple[Tim, list[dict]]]]:
    """For each DRV entry: its ripped TIMs in file order, with their manifest variants."""
    spans = sorted((f.offset, f.offset + f.size, f.path) for f in toc)
    starts = [s[0] for s in spans]
    out: dict[str, list[tuple[Tim, list[dict]]]] = defaultdict(list)
    for (d, off), entries in by_tim.items():
        if d != drv_name:
            continue
        i = bisect.bisect_right(starts, off) - 1
        if i < 0 or not (spans[i][0] <= off < spans[i][1]):
            continue
        tim = read_tim(drv, off)
        if tim is not None:
            out[spans[i][2]].append((tim, sorted(entries, key=variant_index)))
    for path in out:
        out[path].sort(key=lambda t: t[0].offset)
    return out


def shape_of(tim: Tim, var: list[dict]) -> tuple:
    return (tim.bpp, tim.image, tim.clut, len(var), tuple((e["w"], e["h"]) for e in var))


def pair(a: list[tuple[Tim, list[dict]]], b: list[tuple[Tim, list[dict]]], partial: bool = False):
    """Pair the TIMs of one entry by shape. The US build sometimes reorders a container's TIMs
    (the attack-name TIM moved to the end of every E PAK), so order alone is not enough; TIMs
    that share a shape pair by identical pixels first, then in file order. Returns (pairs,
    shared) or None when the two sets of shapes differ; `shared` counts the pairs made by file
    order inside a group of equal shapes (worth a spot check)."""
    groups_a: dict[tuple, list] = defaultdict(list)
    groups_b: dict[tuple, list] = defaultdict(list)
    for t, v in a:
        groups_a[shape_of(t, v)].append((t, v))
    for t, v in b:
        groups_b[shape_of(t, v)].append((t, v))
    if partial:  # keep only the shapes both sides have the same number of
        groups_a = {k: v for k, v in groups_a.items() if len(v) == len(groups_b.get(k, []))}
    elif {k: len(v) for k, v in groups_a.items()} != {k: len(v) for k, v in groups_b.items()}:
        return None
    pairs, shared = [], 0
    for k, va in groups_a.items():
        vb = list(groups_b[k])
        if len(va) == 1:
            pairs.append((va[0], vb[0]))
            continue
        # The US build also swaps equal-shape TIMs inside a PAK (E 541): unchanged ones first.
        rest_a = []
        for ta in va:
            same = next((tb for tb in vb if tb[0].pixels == ta[0].pixels), None)
            if same is not None:
                vb.remove(same)
                pairs.append((ta, same))
            else:
                rest_a.append(ta)
        shared += len(rest_a) if len(rest_a) > 1 else 0
        pairs.extend(zip(rest_a, vb))
    return pairs, shared


@dataclass
class Candidate:
    us: bytes      # the US upload payload
    jp: bytes      # the JP payload it replaces
    label: str     # "DRV:entry" it came from


def resolve(key_name: str, cands: list[Candidate]) -> tuple[Candidate | None, str]:
    """Pick one US payload for a JP upload that several pairs map to (the ripper and the game
    both see identical JP data as one upload). US copies identical to the JP data are
    untranslated leftovers and dropped; then the entry the JP file is named after wins."""
    distinct: dict[bytes, Candidate] = {}
    for c in cands:
        distinct.setdefault(c.us, c)
    changed = [c for c in distinct.values() if c.us != c.jp]
    if not changed:
        return None, ""
    if len(distinct) == 1:
        return changed[0], ""
    own = re.match(r"[A-Z]_(.+?)_off", key_name)
    pick = next((c for c in changed if own and Path(c.label.split(":", 1)[1]).stem == own.group(1)), changed[0])
    others = sorted({c.label for c in cands} - {pick.label})
    kind = "untranslated copy dropped" if len(changed) == 1 else f"{len(changed)} different translations"
    return pick, f"{pick.label} ({kind}; also {', '.join(others)})"


def restore(jp_dir: Path, backup: Path) -> tuple[bool, int]:
    """Undo a previous run: the JP manifest from the backup, no us/ folder, and the JP PNGs an
    older version of this script copied over (it swapped PNGs). Returns (manifest restored,
    PNGs restored)."""
    manifest_backup = backup / "assets_manifest.json"
    had = manifest_backup.is_file()
    if had:
        shutil.copy2(manifest_backup, jp_dir / "assets_manifest.json")
        manifest_backup.unlink()
    shutil.rmtree(jp_dir / RAW_DIR, ignore_errors=True)
    pngs = 0
    for f in sorted(backup.rglob("*.png")):
        shutil.copy2(f, jp_dir / f.relative_to(backup))
        f.unlink()
        pngs += 1
    for d in sorted((p for p in backup.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    return had, pngs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="write the US data and the manifest (default: dry run)")
    ap.add_argument("--restore", action="store_true", help="undo: the JP manifest as it was, no US data")
    ap.add_argument("--only", help="regex on 'DRV:entry path' to limit the run (e.g. 'B.DRV:M_CARD')")
    args = ap.parse_args()

    jp_dir = ROOT / "assets" / "converted" / JP
    backup = ROOT / "assets" / JP / "backup" / "us_images"

    if args.restore or args.apply:
        had, pngs = restore(jp_dir, backup)
        if args.restore:
            print(f"restored: manifest {'yes' if had else '(no backup, nothing to undo)'}, "
                  f"{pngs} PNGs from an older run; re-pack the assets")
            return 0

    jp_manifest = json.loads((jp_dir / "assets_manifest.json").read_text())
    jp_drvs, jp_by = load_side(JP, jp_manifest)
    us_drvs, us_by = load_side(US)
    only = re.compile(args.only) if args.only else None
    skipped, shared_entries = [], []
    images: dict[str, list[Candidate]] = defaultdict(list)   # JP image hash -> candidates
    image_meta: dict[str, dict] = {}                          # JP image hash -> a JP manifest entry
    palettes: dict[str, list[Candidate]] = defaultdict(list)  # JP palette hash -> candidates
    palette_rect: dict[str, tuple[int, int]] = {}
    paired_palettes: set[str] = set()  # JP palettes of TIMs whose pair is planned
    hash_mismatch = 0

    jp_only: set[tuple[str, int]] = set()  # JP TIMs in entries the US build does not have
    for drv_name, pattern in DEFAULT_ENTRIES:
        rx = re.compile(pattern + "$")
        if drv_name not in jp_drvs or drv_name not in us_drvs:
            continue
        jp_toc, _ = drv_unpack.read_toc(jp_drvs[drv_name])
        us_toc, _ = drv_unpack.read_toc(us_drvs[drv_name])
        jp_ents = entry_tims(jp_drvs[drv_name], jp_toc, jp_by, drv_name)
        us_ents = entry_tims(us_drvs[drv_name], us_toc, us_by, drv_name)
        for path, tims in jp_ents.items():
            if path not in us_ents:
                jp_only.update((drv_name, t.offset) for t, _ in tims)
        for path in sorted(jp_ents):
            label = f"{drv_name}:{path}"
            if not rx.search(path) or NEVER.search(path) or (only and not only.search(label)):
                continue
            if path not in us_ents:
                skipped.append((label, "no US entry"))
                continue
            paired = pair(jp_ents[path], us_ents[path], partial=label in PARTIAL)
            if paired is None:
                skipped.append((label, "layout differs"))
                continue
            pairs, shared = paired
            if shared:
                shared_entries.append(label)
            for (jt, jv), (ut, _) in pairs:
                key = fnv1a64(jt.pixels)
                if key != jv[0]["img"]:  # the ripper hashed another upload shape: leave it
                    hash_mismatch += 1
                    continue
                images[key].append(Candidate(ut.pixels, jt.pixels, label))
                image_meta[key] = jv[0]
                if jt.palette:
                    pkey = fnv1a64(jt.palette)
                    paired_palettes.add(pkey)
                    if ut.palette != jt.palette:
                        palettes[pkey].append(Candidate(ut.palette, jt.palette, label))
                        palette_rect[pkey] = (jt.clut[2], jt.clut[3])

    # A JP palette also used by TIMs outside the swap would recolour them. The regular game
    # comes first: a palette is kept JP only when a regular-game image outside the swap uses
    # it. JP-only images (the D-1 Grand Prix DRVs W-Z, attacks the US build dropped) may take
    # the US colours; without them the swapped English image would show in the wrong colours.
    outside: set[str] = set()
    recoloured = 0
    for (drv_name, off), entries in jp_by.items():
        t = read_tim(jp_drvs[drv_name], off)
        if t is not None and t.palette:
            pkey = fnv1a64(t.palette)
            if pkey in palettes and fnv1a64(t.pixels) not in images:
                if drv_name in JP_ONLY_DRVS or (drv_name, off) in jp_only:
                    recoloured += 1
                else:
                    outside.add(pkey)

    notes, image_plan, palette_plan = [], {}, {}
    for key, cands in sorted(images.items()):
        pick, note = resolve(Path(image_meta[key]["path"]).name, cands)
        if pick:
            image_plan[key] = pick
        if note:
            notes.append((Path(image_meta[key]["path"]).name, note))
    shared_palettes = []
    for pkey, cands in sorted(palettes.items()):
        if pkey in outside:
            shared_palettes.append(pkey)
            continue
        pick, _ = resolve("", cands)  # several US palettes for one JP palette: first changed one
        if pick:
            palette_plan[pkey] = pick

    existing = {e["img"] for e in jp_manifest["entries"]}
    clash = [k for k in palette_plan if k in existing]  # a palette that is also a ripped image
    for k in clash:
        del palette_plan[k]

    if args.apply:
        raw = jp_dir / RAW_DIR
        raw.mkdir(parents=True, exist_ok=True)
        backup.mkdir(parents=True, exist_ok=True)
        shutil.copy2(jp_dir / "assets_manifest.json", backup / "assets_manifest.json")
        entries = [e for e in jp_manifest["entries"] if e["img"] not in image_plan]
        for key, c in image_plan.items():
            (raw / f"{key}.raw").write_bytes(c.us)
            m = image_meta[key]
            entries.append({"img": key, "w": m["w"], "h": m["h"], "bpp": m["bpp"],
                            "path": f"{RAW_DIR}/{key}.raw", "us": c.label})
        for key, c in palette_plan.items():
            (raw / f"{key}.raw").write_bytes(c.us)
            w, h = palette_rect[key]
            entries.append({"img": key, "w": w, "h": h, "bpp": 16, "path": f"{RAW_DIR}/{key}.raw",
                            "us": c.label + " (palette)"})
        jp_manifest["entries"] = entries
        (jp_dir / "assets_manifest.json").write_text(json.dumps(jp_manifest, separators=(",", ":")))

    verb = "replaced" if args.apply else "would replace"
    groups: dict[str, int] = defaultdict(int)
    for c in image_plan.values():
        groups[re.sub(r"\d+", "#", c.label)] += 1
    print(f"{verb} {len(image_plan)} images and {len(palette_plan)} palettes with the US data "
          f"({sum(1 for k in images if k not in image_plan)} images identical on both discs)")
    for g, n in sorted(groups.items()):
        print(f"  {g}: {n} images")
    if notes:
        print(f"{len(notes)} JP images stand for several US ones (one pick each):")
        for name, note in notes:
            print(f"  {name}: {note}")
    if recoloured:
        print(f"{recoloured} JP-only images (D-1 Grand Prix, attacks without a US version) share a "
              f"swapped palette and take the US colours")
    if shared_palettes or clash:
        print(f"{len(shared_palettes) + len(clash)} JP palettes kept (also used by images not swapped, "
              f"so the US image shows in JP colours there)")
    if hash_mismatch:
        print(f"{hash_mismatch} images skipped: the manifest hash is not over the TIM's pixels")
    if shared_entries:
        print(f"{len(shared_entries)} entries pair several same-shape images in file order "
              f"(spot-check): {', '.join(shared_entries[:4])}")
    if skipped:
        print(f"left alone ({len(skipped)} entries):")
        reasons: dict[str, list[str]] = defaultdict(list)
        for label, why in skipped:
            reasons[why].append(label)
        for why, labels in sorted(reasons.items()):
            shown = ", ".join(labels[:6]) + (f", ... (+{len(labels) - 6})" if len(labels) > 6 else "")
            print(f"  {why}: {shown}")
    if args.apply:
        print("done; re-pack the assets to use it (--restore undoes it)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
