// The battle record on the VS and result screens in English (config/SLPS-03101/overrides.json,
// docs/re/text-engine.md §7.11).
//
// vs_record_draw (KAWSEG 801F03C8, a0 x, a1 y, a2 wins, a3 losses) draws the record strip of
// one player: sprintf "%4d" (wins + losses) and "%3d" twice, drawn with the digit font
// (text_draw_digits_grey 80028C84, CLUT 7, OT 1) at (x + 8 / 56 / 97, y + 3), then the strip
// picture from VRAM (464, 184), 136x18, CLUT (400, 249). The words for battles, wins and losses
// (戦 勝 敗) are kanji inside that picture. Its callers: vs_screen_draw (801F35D8, both players)
// and the result screen after a battle (two calls near 801F6278 / 801F6524).
//
// The US build drew a blank strip 192 px wide and the words as text (US KAWSEG 801ED968, US
// overlays load at 801DDF38: "*s0%4d        %3d      %3d" in the main font at (x + 8, y + 3),
// then "BATTLES" / "WINS" / "LOSSES" in its 4x5 capital font, 80027DB8, at x + 36 / 102 / 156,
// y + 9). tools/assets/swap_us_images.py puts the US strip, cut to
// the JP width, in place of the JP one (the same picture in all 142 MATCH and 142 WIN
// archives). This override runs the original, then, when the strip in VRAM has no kanji (the
// US strip is loaded), draws the three words where the kanji were (x + 37 / 79 / 119, y + 3, in
// the kanji's green), through the game's text call
// (text_draw_grey 8002ADC8): the catalog's English for 戦 / 勝 / 敗 when it has one, else
// "Btl." / "W" / "L" (the deck select line's words) with the English assets, else the kanji in
// the JP font. With the JP strip in VRAM it draws nothing more (the original, unchanged).

#include "gpu/gpu.hpp"
#include "hw/mmio.hpp"
#include "text.hpp"

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <cstdint>
#include <string>

extern "C" {
void f_8002ADC8(PsxContext* ctx);  // text_draw_grey(x, y, clut, prop, ot@sp16, str@sp20)
}

namespace {

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kSp = 29;
constexpr uint32_t kRecordDraw = 0x801F03C8u;  // KAWSEG vs_record_draw

// The strip picture in VRAM (4 bpp, x in halfwords) and the kanji cells in it (pixel columns,
// rows 3-14; tools/assets/swap_us_images.py NARROW).
constexpr int kStripX = 464, kStripY = 184;
constexpr int kInkTop = 3, kInkBottom = 15;
constexpr int kBgColumn = 20;  // a column of plain background in every row
struct Word {
    int left, right;  // kanji cell columns [left, right)
    int x;            // where the word is drawn, from the strip's x
    const char* sjis;
    const char* fallback;
};
constexpr Word kWords[] = {
    {39, 51, 37, "\x90\xED", "Btl."},  // 戦 battles, after "%4d" at +8 (ends at +32)
    {80, 93, 79, "\x8F\x9F", "W"},     // 勝 wins, after "%3d" at +56 (ends at +74)
    {120, 132, 119, "\x94\x73", "L"},  // 敗 losses, after "%3d" at +97 (ends at +115)
};
constexpr int kTextDy = 3, kProp = 1, kOt = 1;  // the numbers' y offset and OT
constexpr int kClut = 4;  // text CLUT 4: green, the colour of the kanji in the JP strip

int texel(const uint16_t* vram, int u, int v) {
    const uint16_t h = vram[static_cast<size_t>(((kStripY + v) & 511) * 1024 + ((kStripX + u / 4) & 1023))];
    return (h >> ((u & 3) * 4)) & 15;
}

/// True when the kanji cells of the strip in VRAM hold only the background (the US strip).
bool strip_blank(PsxContext& ctx) {
    auto* mmio = static_cast<hle::Mmio*>(psx::Machine::from(&ctx).mmio());
    if (!mmio) return false;
    const uint16_t* vram = mmio->gpu().vram();
    for (int v = kInkTop; v < kInkBottom; ++v) {
        const int bg = texel(vram, kBgColumn, v);
        for (const Word& w : kWords)
            for (int u = w.left; u < w.right; ++u)
                if (texel(vram, u, v) != bg) return false;
    }
    return true;
}

bool english(PsxContext& ctx) {
    static int state = -1;  // checked once: the catalog knows 勝 when the English assets are loaded
    if (state < 0) {
        std::string out;
        state = dcb::text_translate(ctx, "\x8F\x9F", out) ? 1 : 0;
    }
    return state == 1;
}

/// text_draw_grey(x, y, clut, prop, ot, str) with `s` copied onto the guest stack.
void draw_grey(PsxContext& ctx, int x, int y, const std::string& s) {
    const uint32_t sp = ctx.r[kSp];
    const uint32_t len = static_cast<uint32_t>(s.size());
    const uint32_t frame = sp - ((32 + len + 1 + 7) & ~7u);
    const uint32_t str = frame + 32;
    for (uint32_t i = 0; i < len; ++i) psx_write8(&ctx, str + i, static_cast<uint8_t>(s[i]));
    psx_write8(&ctx, str + len, 0);
    psx_write32(&ctx, frame + 16, static_cast<uint32_t>(kOt));
    psx_write32(&ctx, frame + 20, str);
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y);
    ctx.r[kA2] = static_cast<uint32_t>(kClut);
    ctx.r[kA3] = static_cast<uint32_t>(kProp);
    ctx.r[kSp] = frame;
    f_8002ADC8(&ctx);  // through dcb_text_draw: ASCII in the US font, Shift-JIS in the JP one
    ctx.r[kSp] = sp;
}

}  // namespace

extern "C" {

// KAWSEG 801F03C8: vs_record_draw(x, y, wins, losses).
void dcb_vs_record_draw(PsxContext* ctx) {
    const int x = static_cast<int>(ctx->r[kA0]);
    const int y = static_cast<int>(ctx->r[kA1]);
    psx_call_original(ctx, kRecordDraw);
    if (!strip_blank(*ctx)) return;
    const bool en = english(*ctx);
    for (const Word& w : kWords) {
        std::string s;
        if (!dcb::text_translate(*ctx, w.sjis, s)) s = en ? w.fallback : w.sjis;
        draw_grey(*ctx, x + w.x, y + kTextDy, s);
    }
}

}  // extern "C"
