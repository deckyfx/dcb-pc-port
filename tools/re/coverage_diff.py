#!/usr/bin/env python3
"""Diff two DCB_COVERAGE JSON reports.

Intended use: two replays of the same recording that differ by one action
(e.g. one extra button press). Reports functions only in run B, only in run
A, and functions whose call counts changed significantly.

Usage: coverage_diff.py <a.json> <b.json> [--min-calls N] [--ratio R]

Functions match by (overlay, addr). --min-calls drops functions with fewer
than N calls in both runs (default 10: filters one-shot init noise).
--ratio sets the significant-change threshold (default 2.0: one side has
more than twice the calls of the other).
"""
from __future__ import annotations

import argparse
import json
import sys


def load(path):
    with open(path) as f:
        data = json.load(f)
    out = {}
    for e in data.get("entries", []):
        key = (e.get("overlay", ""), e.get("addr", e.get("id")))
        out[key] = e
    return out


def fmt(key, entry):
    ov, addr = key
    name = entry.get("symbol", "")
    where = f"{ov}::{addr}" if ov else str(addr)
    calls = entry.get("calls", 0)
    first = entry.get("first_frame", 0)
    return f"{where:<28} {name:<24} calls={calls:<10} first_frame={first}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("a", help="baseline coverage JSON")
    ap.add_argument("b", help="comparison coverage JSON")
    ap.add_argument("--min-calls", type=int, default=10)
    ap.add_argument("--ratio", type=float, default=2.0)
    args = ap.parse_args(argv)

    try:
        run_a, run_b = load(args.a), load(args.b)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    only_b = sorted(set(run_b) - set(run_a), key=str)
    only_a = sorted(set(run_a) - set(run_b), key=str)
    changed = []
    for key in set(run_a) & set(run_b):
        ca, cb = run_a[key].get("calls", 0), run_b[key].get("calls", 0)
        if max(ca, cb) < args.min_calls:
            continue
        lo, hi = (ca, cb) if ca <= cb else (cb, ca)
        if lo == 0 or hi / lo >= args.ratio:
            changed.append((key, ca, cb))
    changed.sort(key=lambda t: -max(t[1], t[2]))

    print(f"== only in B ({len(only_b)}) ==")
    for key in only_b:
        print(f"  {fmt(key, run_b[key])}")
    print(f"== only in A ({len(only_a)}) ==")
    for key in only_a:
        print(f"  {fmt(key, run_a[key])}")
    print(f"== changed counts ({len(changed)}) ==")
    for key, ca, cb in changed:
        arrow = "up" if cb > ca else "down"
        print(f"  {fmt(key, run_b[key])}  (A={ca} -> B={cb} {arrow})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
