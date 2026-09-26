#!/usr/bin/env python3
"""Compare the DRV archives of two extracted discs entry by entry (e.g. JP SLPS-03101 vs US SLUS-01328).

For every archive present on both sides, entries are matched by path and classified as
identical / same-size-but-different / resized / only-in-A / only-in-B, with per-extension totals.
Nothing is written; the output is a report on stdout (add --entries for the per-entry lines).

    drv_diff.py extracted/SLPS-03101/fs extracted/SLUS-01328/fs [--entries] [--drv B]
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from drv_unpack import load  # noqa: E402

ARCHIVES = "ABCEFGPWXYZ"


def ext(path: str) -> str:
    return path.rsplit(".", 1)[-1]


def diff(a_dir: Path, b_dir: Path, name: str, show: bool) -> None:
    a_path, b_path = a_dir / f"{name}.DRV", b_dir / f"{name}.DRV"
    if not a_path.is_file() or not b_path.is_file():
        side = "A" if a_path.is_file() else "B" if b_path.is_file() else None
        if side:
            _, files, _ = load(a_path if side == "A" else b_path, hashes=False)
            kinds = Counter(ext(e.path) for e in files)
            print(f"== {name}.DRV only in {side}: {len(files)} files {dict(kinds)}")
        return
    _, fa, _ = load(a_path)
    _, fb, _ = load(b_path)
    ma = {e.path: e for e in fa}
    mb = {e.path: e for e in fb}
    order_same = [e.path for e in fa if e.path in mb] == [e.path for e in fb if e.path in ma]
    stats: dict[str, Counter[str]] = defaultdict(Counter)
    lines = []
    for p, e in ma.items():
        o = mb.get(p)
        if o is None:
            cls = "only-A"
        elif o.sha1 == e.sha1:
            cls = "identical"
        elif o.size == e.size:
            cls = "same-size"
        else:
            cls = "resized"
        stats[ext(p)][cls] += 1
        if show and cls != "identical":
            lines.append(f"  {cls:<9} {p:<28} {e.size:>9} -> {o.size if o else '-':>9}")
    for p, o in mb.items():
        if p not in ma:
            stats[ext(p)]["only-B"] += 1
            if show:
                lines.append(f"  {'only-B':<9} {p:<28} {'-':>9} -> {o.size:>9}")
    total = Counter()
    for c in stats.values():
        total.update(c)
    print(f"== {name}.DRV  A {len(fa)} / B {len(fb)} entries, shared order {'same' if order_same else 'DIFFERENT'}: "
          + ", ".join(f"{k} {v}" for k, v in sorted(total.items())))
    for k in sorted(stats):
        print(f"   .{k:<4} " + ", ".join(f"{c} {n}" for c, n in sorted(stats[k].items())))
    for line in lines:
        print(line)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("a", type=Path, help="fs/ directory of disc A")
    ap.add_argument("b", type=Path, help="fs/ directory of disc B")
    ap.add_argument("--drv", default=ARCHIVES, help="archive letters to compare (default: all)")
    ap.add_argument("--entries", action="store_true", help="print every non-identical entry")
    args = ap.parse_args()
    for name in args.drv:
        diff(args.a, args.b, name, args.entries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
