// VS-screen big names in English (docs/re/text-engine.md 7.11): port of tools/text/bigfont.py.
//
// en_bigfont.bin: the US big-name font (B:\FONT.ARC, 16x32 4-bpp glyph TIMs) repacked for the
// runtime override of the JP loader (src/game/overrides/bigname.cpp):
//   "BGF1"  u8 first (0x20)  u8 count (92)  u8 width in halfwords (4)  u8 height (32)
//   32 B    CLUT (the same in every glyph TIM)
//   count B 1 when the character has its own glyph, else 0
//   count x 256 B  glyph pixels, 4 bpp (zeros for characters without a glyph)
// files/B/MATCH/NNN.ARC: the JP match archive with the opponent's name picture (its last TIM)
// taken from the US one.

#include "text_internal.hpp"

#include <algorithm>

namespace patch::text {

namespace {

constexpr int kFirst = 0x20, kCount = 92, kGlyphHw = 4, kGlyphH = 32;
constexpr size_t kGlyphBytes = kGlyphHw * 2 * kGlyphH;
constexpr size_t kTimSize = 320;  // 8 header + 12 + 32 CLUT block + 12 + 256 pixel block

/// (CLUT 32 B, pixels 256 B) of one FONT.ARC glyph TIM.
std::pair<View, View> tim_parts(View tim) {
    if (rd32(tim, 0) != 0x10 || rd32(tim, 4) != 8) throw std::runtime_error("FONT.ARC: glyph is not a 4-bpp TIM with a CLUT");
    const uint32_t cl_len = rd32(tim, 8);
    if (rd16(tim, 16) != 16 || rd16(tim, 18) != 1) throw std::runtime_error("FONT.ARC: unexpected CLUT");
    const size_t p = 8 + size_t{cl_len};
    if (rd16(tim, p + 8) != kGlyphHw || rd16(tim, p + 10) != kGlyphH) throw std::runtime_error("FONT.ARC: unexpected glyph");
    if (p + 12 + kGlyphBytes > tim.size()) throw std::runtime_error("FONT.ARC: short glyph");
    return {tim.subspan(20, 32), tim.subspan(p + 12, kGlyphBytes)};
}

/// bigfont._tim_rects: (CLUT x, y) if any, and the image (x, y, w, h).
struct Rects {
    bool has_clut = false;
    uint16_t cx = 0, cy = 0, x = 0, y = 0, w = 0, h = 0;
};
Rects tim_rects(View tim) {
    Rects r;
    size_t p = 8;
    if (rd32(tim, 4) & 8) {
        r.has_clut = true;
        r.cx = rd16(tim, p + 4);
        r.cy = rd16(tim, p + 6);
        rd16(tim, p + 10);  // the whole header must be there (struct.unpack_from)
        p += rd32(tim, p);
    }
    rd32(tim, p);
    r.x = rd16(tim, p + 4);
    r.y = rd16(tim, p + 6);
    r.w = rd16(tim, p + 8);
    r.h = rd16(tim, p + 10);
    return r;
}

}  // namespace

Bytes build_bigfont(View arc) {
    std::vector<uint32_t> offs(kCount);
    for (int i = 0; i < kCount; ++i) offs[static_cast<size_t>(i)] = rd32(arc, 4 * static_cast<size_t>(i));
    if (offs[0] != kCount * 4) throw std::runtime_error("FONT.ARC: bad offset table");
    // Several codes share one TIM; the glyph belongs to the last code of each run.
    std::vector<std::pair<uint32_t, int>> owner;  // TIM offset -> last code, in first-seen order
    for (int i = 0; i < kCount; ++i) {
        const uint32_t off = offs[static_cast<size_t>(i)];
        if (off + kTimSize > arc.size()) continue;
        const auto it = std::find_if(owner.begin(), owner.end(), [&](const auto& o) { return o.first == off; });
        if (it == owner.end()) owner.emplace_back(off, kFirst + i);
        else it->second = kFirst + i;
    }
    View clut;
    std::vector<View> glyphs(kCount);
    std::vector<bool> present(kCount, false);
    for (const auto& [off, code] : owner) {
        const auto [c, px] = tim_parts(arc.subspan(off, kTimSize));
        if (!clut.empty() && !equal(c, clut)) throw std::runtime_error("FONT.ARC: glyphs with different CLUTs");
        clut = c;
        glyphs[static_cast<size_t>(code - kFirst)] = px;
        present[static_cast<size_t>(code - kFirst)] = true;
    }
    Bytes out{'B', 'G', 'F', '1', kFirst, kCount, kGlyphHw, kGlyphH};
    out.insert(out.end(), clut.begin(), clut.end());
    for (int i = 0; i < kCount; ++i) out.push_back(present[static_cast<size_t>(i)] ? 1 : 0);
    for (int i = 0; i < kCount; ++i) {
        if (present[static_cast<size_t>(i)]) out.insert(out.end(), glyphs[static_cast<size_t>(i)].begin(), glyphs[static_cast<size_t>(i)].end());
        else out.insert(out.end(), kGlyphBytes, 0);
    }
    return out;
}

std::optional<Bytes> graft_match_name(View jp, View us) {
    const auto je = arc_entries(jp), ue = arc_entries(us);
    if (!je || !ue || je->size() != ue->size() || je->empty()) return std::nullopt;
    // The name picture: 4 bpp at VRAM (704, 480), CLUT (752, 472), 32 rows, at most 64 halfwords.
    for (View tim : {je->back(), ue->back()}) {
        const Rects r = tim_rects(tim);
        if (!r.has_clut || r.cx != 752 || r.cy != 472 || r.x != 704 || r.y != 480 || r.w > 64 || r.h != 32)
            return std::nullopt;
    }
    std::vector<View> entries(je->begin(), je->end() - 1);
    entries.push_back(ue->back());
    return write_arc(entries);
}

}  // namespace patch::text
