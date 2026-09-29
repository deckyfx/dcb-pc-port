"""VCDIFF (RFC 3284) decoder for xdelta3 patches, e.g. the romhacking.net .xdelta fixes.

Supports what xdelta3 writes by default: the default code table, source and target copies,
the application header, window Adler-32 checksums (skipped), and LZMA secondary compression
(xdelta3's id 2: each compressed section is a varint of its decoded size + an xz stream).
"""
from __future__ import annotations

import lzma

VCD_SOURCE, VCD_TARGET, VCD_ADLER32 = 0x01, 0x02, 0x04
NOOP, ADD, RUN, COPY = 0, 1, 2, 3
LZMA_ID = 2


def _varint(b: bytes, i: int) -> tuple[int, int]:
    v = 0
    while True:
        c = b[i]
        i += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, i


def _default_code_table() -> list[tuple[tuple[int, int, int], tuple[int, int, int]]]:
    """RFC 3284 §5.6: 256 entries of ((type, size, mode), (type, size, mode))."""
    none = (NOOP, 0, 0)
    t = [((RUN, 0, 0), none)]
    t += [((ADD, s, 0), none) for s in range(18)]
    for m in range(9):
        t.append(((COPY, 0, m), none))
        t += [((COPY, s, m), none) for s in range(4, 19)]
    for m in range(6):
        t += [((ADD, a, 0), (COPY, c, m)) for a in range(1, 5) for c in range(4, 7)]
    for m in range(6, 9):
        t += [((ADD, a, 0), (COPY, 4, m)) for a in range(1, 5)]
    t += [((COPY, 4, m), (ADD, 1, 0)) for m in range(9)]
    assert len(t) == 256
    return t


_TABLE = _default_code_table()


def _secondary(section: bytes) -> bytes:
    size, j = _varint(section, 0)
    out = lzma.LZMADecompressor(format=lzma.FORMAT_XZ).decompress(section[j:], max_length=size)
    if len(out) != size:
        raise ValueError("short LZMA section")
    return out


def decode(source: bytes, patch: bytes) -> bytes:
    """The target file: `patch` applied to `source`."""
    b = patch
    if b[:3] != b"\xd6\xc3\xc4":
        raise ValueError("not a VCDIFF file")
    i = 4
    hdr = b[i]
    i += 1
    secondary = None
    if hdr & 0x01:
        secondary = b[i]
        i += 1
        if secondary != LZMA_ID:
            raise ValueError(f"unsupported secondary compressor {secondary}")
    if hdr & 0x02:
        raise ValueError("custom code tables are not supported")
    if hdr & 0x04:  # application header (xdelta3 stores the file names)
        n, i = _varint(b, i)
        i += n
    out = bytearray()
    while i < len(b):
        win = b[i]
        i += 1
        segment = b""
        if win & (VCD_SOURCE | VCD_TARGET):
            length, i = _varint(b, i)
            pos, i = _varint(b, i)
            segment = bytes((source if win & VCD_SOURCE else out)[pos:pos + length])
        _, i = _varint(b, i)  # delta encoding length
        target_len, i = _varint(b, i)
        indicator = b[i]
        i += 1
        dlen, i = _varint(b, i)
        ilen, i = _varint(b, i)
        alen, i = _varint(b, i)
        if win & VCD_ADLER32:
            i += 4
        data, i = b[i:i + dlen], i + dlen
        inst, i = b[i:i + ilen], i + ilen
        addr, i = b[i:i + alen], i + alen
        if indicator:
            if secondary is None:
                raise ValueError("compressed section without a secondary compressor")
            data = _secondary(data) if indicator & 1 else data
            inst = _secondary(inst) if indicator & 2 else inst
            addr = _secondary(addr) if indicator & 4 else addr
        target = bytearray()
        di = ii = ai = 0
        near = [0] * 4
        near_next = 0
        same = [0] * (3 * 256)
        while ii < len(inst):
            code = inst[ii]
            ii += 1
            for kind, size, mode in _TABLE[code]:
                if kind == NOOP:
                    continue
                if size == 0:
                    size, ii = _varint(inst, ii)
                if kind == ADD:
                    target += data[di:di + size]
                    di += size
                elif kind == RUN:
                    target += bytes([data[di]]) * size
                    di += 1
                else:
                    here = len(segment) + len(target)
                    if mode == 0:
                        a, ai = _varint(addr, ai)
                    elif mode == 1:
                        v, ai = _varint(addr, ai)
                        a = here - v
                    elif mode < 6:
                        v, ai = _varint(addr, ai)
                        a = near[mode - 2] + v
                    else:
                        a = same[(mode - 6) * 256 + addr[ai]]
                        ai += 1
                    near[near_next] = a
                    near_next = (near_next + 1) % 4
                    same[a % (3 * 256)] = a
                    for k in range(size):  # may overlap the bytes being written
                        p = a + k
                        target.append(segment[p] if p < len(segment) else target[p - len(segment)])
        if len(target) != target_len:
            raise ValueError(f"window decoded to {len(target)} bytes, expected {target_len}")
        out += target
    return bytes(out)
