// Level badges in English: the card levels drawn as the US R / A / C / U badges instead of the JP
// Ⅲ / A / Ⅳ / 完 (config/SLPS-03101/overrides.json).
//
// The level badge is icon 16..19 of the text icon function (80029F70, mode 0: a 12x11 cell of
// SYSTEM.TIM at u = (idx%14)*12, v = 0x69 + (idx/14)*11). Deck select, the card list, deck edit
// and the battle card panel draw it after "Lv" through that function. The US disc has the same
// grid with the R / A / C / U badges, 22 rows lower (v base 0x7F): those rows are part of the US
// font rows text.cpp keeps in the GPU's private sheet (en_font.bin, sheet row 0 = TIM row 48,
// built from the player's US disc). So the original draws the badge as usual, and the packet it
// just wrote is pointed at the sheet: the texpage word becomes the sheet marker (as for the
// English glyphs) and v the US row. Size, position, colour and CLUT stay the game's; the US badge
// art uses the same palette slots as the JP one.
//
// Only when the English assets are loaded (the private sheet exists). English strings with a
// `*e3`-style code are drawn by text.cpp, which calls the original directly, so they keep the JP
// art: the port's own strings spell the level out ("Level R").

#include "gpu/gpu.hpp"
#include "text.hpp"

#include <psx/recomp.h>

#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kTextIcon = 0x80029F70u;  // text_icon(x, y, mode, idx, rgb*, ot)
constexpr uint32_t kTextPrim = 0x801D9714u;  // g_text_prim: next free text primitive
constexpr uint32_t kPrimSize = 0x1C;         // DR_TPAGE (2 words) + SPRT (5 words)
constexpr int kA2 = 6, kA3 = 7;

constexpr int kFirstBadge = 16, kLastBadge = 19;  // R (Ⅲ), A, C (Ⅳ), U (完)
constexpr int kIconCols = 14, kIconW = 12, kIconH = 11;
constexpr int kUsIconBase = 0x7F;  // US icon grid row base in SYSTEM.TIM (JP 0x69)
constexpr int kSheetFirstRow = 48;  // private sheet row 0 = TIM row 48 (text.cpp, en_font.bin)

/// English assets loaded (en_font.bin in the private sheet, catalog read): checked once through
/// the catalog with a string every catalog has ("勝", wins).
bool english(PsxContext& ctx) {
    static int state = -1;  // -1 unknown, 0 no, 1 yes
    if (state < 0) {
        std::string out;
        state = dcb::text_translate(ctx, "\x8F\x9F", out) ? 1 : 0;
    }
    return state == 1;
}

}  // namespace

extern "C" {

// 80029F70: text_icon(x, y, mode, idx, rgb*, ot).
void dcb_text_icon(PsxContext* ctx) {
    const int mode = static_cast<int>(ctx->r[kA2]);
    const int idx = static_cast<int>(ctx->r[kA3]);
    const uint32_t before = psx_read32(ctx, kTextPrim);
    psx_call_original(ctx, kTextIcon);
    if (mode != 0 || idx < kFirstBadge || idx > kLastBadge) return;
    if (psx_read32(ctx, kTextPrim) != before + kPrimSize || !english(*ctx)) return;  // pool full: nothing drawn
    const uint32_t p = before;
    const int v = kUsIconBase + (idx / kIconCols) * kIconH - kSheetFirstRow;
    psx_write32(ctx, p + 0x04, hle::Gpu::kSheetMarker | 0u);  // texpage word -> private sheet
    psx_write8(ctx, p + 0x14, static_cast<uint8_t>((idx % kIconCols) * kIconW));
    psx_write8(ctx, p + 0x15, static_cast<uint8_t>(v));
}

}  // extern "C"
