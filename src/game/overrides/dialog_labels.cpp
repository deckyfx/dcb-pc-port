// Generic dialog (dialog_setup 80019FF0): the new-game "see the explanation?" choices read
// Yes / No in English, not On / Off.
//
// dialog_setup(win, text, style) sizes a message / choice box; with style 2 the caller has put
// its own choice labels at win+0x98 / win+0x9C, otherwise it writes the defaults はい / いいえ
// (EXE 80010028 / 80010030, "Yes" / "No" in the catalog). OPENSEG uses one pair of labels,
// みる / みない ("watch" / "don't watch", OPENSEG 801E1888 / 801E1890), for two questions:
// the Polygon Battle setting (a box titled ポリゴンバトル, 801EA718) and the choice after
// "User Registration is complete. Would you like to know more about this world?" and "Do you
// want to learn about the game?" (a box with no text, the question being in the message window,
// 801EA220). The US build split them (US OPENSEG a7c "Yes"/"No", ab4 "On"/"Off"); the catalog
// has one translation per JP string, the lowest id's: KAWSEG:1a48 / 1a50 "On" / "Off" (the
// battle option), so both questions showed On / Off. This override gives a box with no text and
// the みる / みない labels the default labels instead, which the catalog draws as Yes / No. The
// Polygon Battle box (with a title) keeps On / Off. Only with the English assets loaded: the JP
// game keeps its labels.

#include "text.hpp"

#include <psx/recomp.h>

#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kDialogSetup = 0x80019FF0u;
constexpr uint32_t kLabelYes = 0x80010028u;  // はい (EXE default label)
constexpr uint32_t kLabelNo = 0x80010030u;   // いいえ
constexpr uint32_t kLabel1 = 0x98, kLabel2 = 0x9C;  // win+: choice label pointers
constexpr uint32_t kStyleCustomLabels = 2;

std::string guest_string(PsxContext& ctx, uint32_t addr, size_t max = 16) {
    std::string s;
    if (addr < 0x80000000u || addr >= 0x80200000u) return s;
    for (size_t i = 0; i < max; ++i) {
        const auto c = static_cast<char>(psx_read8(&ctx, addr + static_cast<uint32_t>(i)));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

}  // namespace

extern "C" {

// 80019FF0: dialog_setup(win, text, style).
void dcb_dialog_setup(PsxContext* ctx) {
    static const std::string kWatch = "\x82\xdd\x82\xe9";               // みる
    static const std::string kDontWatch = "\x82\xdd\x82\xc8\x82\xa2";  // みない
    const uint32_t win = ctx->r[4], text = ctx->r[5], style = ctx->r[6];
    if (text == 0 && style == kStyleCustomLabels && win >= 0x80000000u && win < 0x80200000u) {
        const uint32_t l1 = psx_read32(ctx, win + kLabel1), l2 = psx_read32(ctx, win + kLabel2);
        std::string unused;
        if (guest_string(*ctx, l1) == kWatch && guest_string(*ctx, l2) == kDontWatch &&
            dcb::text_translate(*ctx, guest_string(*ctx, kLabelYes), unused)) {
            psx_write32(ctx, win + kLabel1, kLabelYes);
            psx_write32(ctx, win + kLabel2, kLabelNo);
        }
    }
    psx_call_original(ctx, kDialogSetup);
}

}  // extern "C"
