#!/usr/bin/env python3
"""Decode the SYS chunk task list, extract exact fiber-stack byte ranges, and
classify every 8-byte value against /proc/<pid>/maps. This is the Part 2
evidence: which host address classes actually sit on game stacks.

Usage: scan_stacks.py <state-file> <maps-file>
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter


class R:
    def __init__(self, data):
        self.d = data
        self.o = 0

    def u64(self):
        v = struct.unpack_from("<Q", self.d, self.o)[0]
        self.o += 8
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.o)[0]
        self.o += 4
        return v

    def boolean(self):
        v = self.d[self.o] != 0
        self.o += 1
        return v

    def vec(self):
        n = self.u64()
        v = self.d[self.o:self.o + n]
        self.o += n
        return v

    def pod(self, n):
        v = self.d[self.o:self.o + n]
        self.o += n
        return v


def load_regions(maps_path):
    regions = []
    with open(maps_path) as f:
        for line in f:
            parts = line.split()
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            regions.append((lo, hi, parts[1], parts[5] if len(parts) > 5 else ""))
    return regions


def classify(regions, v):
    for lo, hi, perms, name in regions:
        if lo <= v < hi:
            if "x" in perms:
                return f"CODE {name.split('/')[-1] if name else 'anon-exec'}"
            if "[stack]" in name:
                return "STACK (main thread)"
            if "[heap]" in name:
                return "HEAP"
            base = name.split("/")[-1] if name else ""
            if base.endswith(".so") or ".so." in base or "libc" in base or "libSDL" in base:
                return f"LIB {base}"
            if not name:
                return "ANON-RW (mmap)" if "rw" in perms else f"ANON-{perms}"
            if "dcb" in name:
                return "BIN-DATA"
            return f"OTHER {name}"
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("state")
    ap.add_argument("maps")
    args = ap.parse_args(argv)
    regions = load_regions(args.maps)
    data = open(args.state, "rb").read()

    # Top-level chunks; find SYS.
    off = 0
    sys_payload = None
    while off + 16 <= len(data):
        tag, ver, size = struct.unpack_from("<4sIQ", data, off)
        off += 16
        if tag == b"SYS ":
            sys_payload = data[off:off + size]
        off += size
    if sys_payload is None:
        print("no SYS chunk", file=sys.stderr)
        return 1

    r = R(sys_payload)
    r.u64()  # vblanks_
    r.u64()  # vblank_event_sent_
    r.boolean()  # in_irq_
    irq_env = r.u64()
    r.u32()  # dispatcher_hook_
    r.u32()  # dispatcher_
    r.u32()  # next_cookie_
    r.u32()  # main_entry_
    r.u32()  # current cookie
    n = r.u64()  # task count (w.size)
    print(f"tasks: {n}, irq_env in saved stacks: (checked below)")
    detail = Counter()
    stacks = 0
    stack_bytes = 0
    for _ in range(n):
        r.u32()  # cookie
        r.u32()  # start_pc
        r.pod(32 * 4)  # regs
        r.u32()  # hi
        r.u32()  # lo
        r.u32()  # sr
        r.u32()  # resume_tcb
        r.boolean()  # resume_full
        r.boolean()  # dead
        stack_base = r.u64()
        stack_bytes_img = r.u64()
        data_base = r.u64()
        img_data = r.vec()
        r.vec()  # context
        r.u64()  # entry
        r.u64()  # arg
        stacks += 1
        stack_bytes += len(img_data)
        for o in range(0, len(img_data) - 8):
            (v,) = struct.unpack_from("<Q", img_data, o)
            if v < 0x10000:
                continue
            c = classify(regions, v)
            if c:
                detail[c] += 1
    print(f"stack images: {stacks}, bytes: {stack_bytes}, irq_env=0x{irq_env:x}")
    for cls, cnt in detail.most_common(15):
        print(f"{cnt:8d}  {cls}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
