// MSD scripts ("MSCD" VM bytecode, docs/re/text-engine.md 5.1): port of tools/text/msd.py.
//
// Records are 4-byte aligned, u16 op first:
//   8          text: u16 8, u16 reg, u16 len, char[len] (NUL included), padded to 4
//   6          raw block: u16 6, u16 len, len bytes, padded to 4
//   5          jump (8 B), 7 arithmetic (12 B), 9 conditional skip (12 B)
//   0x0A-0x0E  host command: u16 op, u16 cmd, {u16 is_reg, u16 value} x (op - 0x0A)

#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>

namespace patch::text {

namespace {

/// Size of the fixed-size records by op; 0 = not a fixed-size op.
size_t fixed_size(uint16_t op) {
    switch (op) {
        case 5: return 8;
        case 7: return 12;
        case 9: return 12;
        case 0x0A: return 4;
        case 0x0B: return 8;
        case 0x0C: return 12;
        case 0x0D: return 16;
        case 0x0E: return 20;
        default: return 0;
    }
}

View slice(View b, size_t a, size_t e) {
    a = std::min(a, b.size());
    e = std::clamp(e, a, b.size());
    return b.subspan(a, e - a);
}

/// msd._shape equality: jumps by kind (their offsets move with the text), the rest byte for byte.
bool same_shape(const MsdRecord& a, const MsdRecord& b) {
    const bool ja = a.op == kMsdJump, jb = b.op == kMsdJump;
    if (ja != jb) return false;
    if (ja) return equal(slice(a.raw, 0, 4), slice(b.raw, 0, 4));
    return equal(a.raw, b.raw);
}

std::string at(const MsdRecord& r) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "record op %#x at %#zx differs", r.op, r.offset);
    return buf;
}

}  // namespace

const ShowText& city_show_text() {
    static const ShowText k{{0x0A, 4}, {0x0A, 5}};
    return k;
}

std::vector<MsdRecord> msd_walk(View script) {
    if (script.size() < 4 || str(script.first(4)) != "MSCD") throw std::runtime_error("not an MSD script");
    std::vector<MsdRecord> out;
    size_t off = 16;
    while (off < script.size()) {
        MsdRecord r;
        r.offset = off;
        r.op = rd16(script, off);
        size_t size = 0;
        if (r.op == kMsdText) {
            const size_t length = rd16(script, off + 4);
            size = (6 + length + 3) & ~size_t{3};
            r.raw = slice(script, off, off + 6);
            r.has_text = true;
            r.text = slice(script, off + 6, off + 6 + length);
        } else if (r.op == 6) {
            const size_t length = rd16(script, off + 2);
            size = (4 + length + 3) & ~size_t{3};
            r.raw = slice(script, off, off + 4 + length);
        } else if ((size = fixed_size(r.op)) != 0) {
            r.raw = slice(script, off, off + size);
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, "unknown op %#x at %#zx", r.op, off);
            throw std::runtime_error(buf);
        }
        out.push_back(r);
        off += size;
    }
    return out;
}

bool msd_is_text_side(const MsdRecord& r, const ShowText& show_text) {
    if (r.op == kMsdText) return true;
    return r.op == 0x0A && show_text.count({r.op, rd16(r.raw, 2)}) != 0;
}

bool msd_is_button_test(const MsdRecord& r, const ButtonRegs& regs) {
    return r.op == 9 && regs.count(rd16(r.raw, 2)) != 0;
}

std::vector<MsdRecord> msd_skeleton(View script, const ShowText& show_text) {
    std::vector<MsdRecord> out;
    for (const MsdRecord& r : msd_walk(script))
        if (!msd_is_text_side(r, show_text)) out.push_back(r);
    return out;
}

bool msd_same_program(View jp, View us, const ShowText& show_text, const ButtonRegs& regs, std::string* why) {
    const std::vector<MsdRecord> a = msd_skeleton(jp, show_text), b = msd_skeleton(us, show_text);
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (same_shape(a[i], b[i])) continue;
        // A test of one pad register may test another at the same place (the US moved
        // confirm / cancel): same jump, other register.
        if (msd_is_button_test(a[i], regs) && msd_is_button_test(b[i], regs) &&
            equal(slice(a[i].raw, 4, a[i].raw.size()), slice(b[i].raw, 4, b[i].raw.size())))
            continue;
        if (why) *why = at(b[i]);
        return false;
    }
    if (a.size() != b.size()) {
        if (why) *why = at((a.size() > b.size() ? a : b)[n]);
        return false;
    }
    // Text records must load the same registers on both sides.
    std::set<uint16_t> regs_jp, regs_us;
    for (const MsdRecord& r : msd_walk(jp))
        if (r.op == kMsdText) regs_jp.insert(rd16(r.raw, 2));
    for (const MsdRecord& r : msd_walk(us))
        if (r.op == kMsdText) regs_us.insert(rd16(r.raw, 2));
    if (regs_jp != regs_us) {
        if (why) *why = "text registers differ";
        return false;
    }
    return true;
}

}  // namespace patch::text
