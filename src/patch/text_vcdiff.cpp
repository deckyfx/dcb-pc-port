// VCDIFF (RFC 3284) decoder for xdelta3 patches: port of tools/text/vcdiff.py.
//
// Supports what xdelta3 writes by default: the default code table, source and target copies, the
// application header, window Adler-32 checksums (skipped), and LZMA secondary compression
// (xdelta3's id 2: each compressed section is a varint of its decoded size + an .xz stream).

#include "text_internal.hpp"

#include <array>

namespace patch::text {

namespace {

constexpr uint8_t kSource = 0x01, kTarget = 0x02, kAdler32 = 0x04;
constexpr uint8_t kLzmaId = 2;

enum Kind : uint8_t { Noop, Add, Run, Copy };
struct Inst {
    Kind kind = Noop;
    uint8_t size = 0, mode = 0;
};
using CodeTable = std::array<std::array<Inst, 2>, 256>;

/// RFC 3284 5.6: the default code table.
CodeTable default_code_table() {
    CodeTable t{};
    size_t n = 0;
    const auto put = [&](Inst a, Inst b = {}) { t[n++] = {a, b}; };
    const auto u8 = [](int v) { return static_cast<uint8_t>(v); };
    put({Run, 0, 0});
    for (int s = 0; s < 18; ++s) put({Add, u8(s), 0});
    for (int m = 0; m < 9; ++m) {
        put({Copy, 0, u8(m)});
        for (int s = 4; s < 19; ++s) put({Copy, u8(s), u8(m)});
    }
    for (int m = 0; m < 6; ++m)
        for (int a = 1; a < 5; ++a)
            for (int c = 4; c < 7; ++c) put({Add, u8(a), 0}, {Copy, u8(c), u8(m)});
    for (int m = 6; m < 9; ++m)
        for (int a = 1; a < 5; ++a) put({Add, u8(a), 0}, {Copy, 4, u8(m)});
    for (int m = 0; m < 9; ++m) put({Copy, 4, u8(m)}, {Add, 1, 0});
    if (n != 256) throw std::logic_error("VCDIFF code table");
    return t;
}

/// A byte cursor over a section; throws when read past its end (Python's IndexError).
struct Reader {
    View b;
    size_t i = 0;

    uint8_t byte() {
        if (i >= b.size()) throw std::runtime_error("VCDIFF: truncated data");
        return b[i++];
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (;;) {
            const uint8_t c = byte();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
    }
    View take(uint64_t n) {
        const size_t start = std::min(i, b.size());
        const size_t len = static_cast<size_t>(std::min<uint64_t>(n, b.size() - start));
        i += static_cast<size_t>(n);
        return b.subspan(start, len);
    }
};

/// A secondary-compressed section: varint decoded size, then an .xz stream.
Bytes secondary(View section) {
    Reader r{section};
    const uint64_t size = r.varint();
    Bytes out = xz_decode(section.subspan(r.i), static_cast<size_t>(size));
    if (out.size() != size) throw std::runtime_error("VCDIFF: short LZMA section");
    return out;
}

}  // namespace

Bytes vcdiff_decode(View source, View patch) {
    static const CodeTable kTable = default_code_table();
    if (patch.size() < 5 || patch[0] != 0xD6 || patch[1] != 0xC3 || patch[2] != 0xC4)
        throw std::runtime_error("not a VCDIFF file");
    Reader p{patch, 4};
    const uint8_t hdr = p.byte();
    bool compressed = false;
    if (hdr & 0x01) {
        if (p.byte() != kLzmaId) throw std::runtime_error("VCDIFF: unsupported secondary compressor");
        compressed = true;
    }
    if (hdr & 0x02) throw std::runtime_error("VCDIFF: custom code tables are not supported");
    if (hdr & 0x04) p.take(p.varint());  // application header (xdelta3 stores the file names)

    Bytes out;
    while (p.i < patch.size()) {
        const uint8_t win = p.byte();
        View segment;
        size_t seg_from_out = 0, seg_len = 0;  // a target segment lives in `out` (may reallocate)
        bool seg_is_out = false;
        if (win & (kSource | kTarget)) {
            const uint64_t length = p.varint(), pos = p.varint();
            const View base = (win & kSource) ? source : View(out);
            const size_t a = static_cast<size_t>(std::min<uint64_t>(pos, base.size()));
            const size_t n = static_cast<size_t>(std::min<uint64_t>(length, base.size() - a));
            if (win & kSource) {
                segment = source.subspan(a, n);
            } else {
                seg_is_out = true;
                seg_from_out = a;
                seg_len = n;
            }
        }
        Bytes seg_copy;
        if (seg_is_out) {  // Python copies the slice: later appends must not move it
            seg_copy.assign(out.begin() + static_cast<std::ptrdiff_t>(seg_from_out),
                            out.begin() + static_cast<std::ptrdiff_t>(seg_from_out + seg_len));
            segment = seg_copy;
        }
        p.varint();  // delta encoding length
        const uint64_t target_len = p.varint();
        const uint8_t indicator = p.byte();
        const uint64_t dlen = p.varint(), ilen = p.varint(), alen = p.varint();
        if (win & kAdler32) p.take(4);
        View data = p.take(dlen), inst = p.take(ilen), addr = p.take(alen);
        Bytes data_buf, inst_buf, addr_buf;
        if (indicator) {
            if (!compressed) throw std::runtime_error("VCDIFF: compressed section without a secondary compressor");
            if (indicator & 1) data = data_buf = secondary(data);
            if (indicator & 2) inst = inst_buf = secondary(inst);
            if (indicator & 4) addr = addr_buf = secondary(addr);
        }

        Bytes target;
        target.reserve(static_cast<size_t>(std::min<uint64_t>(target_len, 1u << 26)));
        Reader d{data}, in{inst}, ad{addr};
        std::array<uint64_t, 4> near{};
        size_t near_next = 0;
        std::array<uint64_t, 3 * 256> same{};
        while (in.i < inst.size()) {
            for (const Inst& ins : kTable[in.byte()]) {
                if (ins.kind == Noop) continue;
                uint64_t size = ins.size;
                if (size == 0) size = in.varint();
                if (ins.kind == Add) {
                    const View add = d.take(size);
                    if (add.size() != size) throw std::runtime_error("VCDIFF: ADD past the data section");
                    target.insert(target.end(), add.begin(), add.end());
                } else if (ins.kind == Run) {
                    const uint8_t v = d.byte();
                    target.insert(target.end(), static_cast<size_t>(size), v);
                } else {
                    const uint64_t here = segment.size() + target.size();
                    uint64_t a = 0;
                    if (ins.mode == 0) {
                        a = ad.varint();
                    } else if (ins.mode == 1) {
                        a = here - ad.varint();
                    } else if (ins.mode < 6) {
                        a = near[ins.mode - 2] + ad.varint();
                    } else {
                        a = same[(ins.mode - 6) * 256u + ad.byte()];
                    }
                    near[near_next] = a;
                    near_next = (near_next + 1) % 4;
                    same[a % (3 * 256)] = a;
                    if (a + size <= segment.size()) {  // the usual case: all from the source
                        const auto from = segment.begin() + static_cast<std::ptrdiff_t>(a);
                        target.insert(target.end(), from, from + static_cast<std::ptrdiff_t>(size));
                        continue;
                    }
                    for (uint64_t k = 0; k < size; ++k) {  // may overlap the bytes being written
                        const uint64_t q = a + k;
                        if (q < segment.size()) {
                            target.push_back(segment[static_cast<size_t>(q)]);
                        } else {
                            const uint64_t t = q - segment.size();
                            if (t >= target.size()) throw std::runtime_error("VCDIFF: COPY past the target");
                            target.push_back(target[static_cast<size_t>(t)]);
                        }
                    }
                }
            }
        }
        if (target.size() != target_len) throw std::runtime_error("VCDIFF: window decoded to the wrong size");
        out.insert(out.end(), target.begin(), target.end());
    }
    return out;
}

}  // namespace patch::text
