// Wizardmon's spell: the WORD INPUT keyword check (SAISEG 801EE704), docs/re/name-entry.md.
//
// keyword_check(buf) runs the entry screen, copies the typed keyword into `buf`, then compares it
// (strcmp) with the ten full-width keywords (pointer table 801F6D24, ＯＭＥＧＡ１ first) and leaves
// the result in the city script's r1 (*(*(801F7E80) + 8) + 4): the keyword's index 0-9, -1 for
// a wrong one (OK), -2 when cancelled. Sky City's script then gives index n's card once.
//
// The screen types half-width here (name_entry.cpp), so after the original:
//   - a keyword the game did not match is matched as text: the JP keywords as ASCII (JI2MON,
//     H-KABU ...) and the US ones (JIJIMON, H-KBUTERIMON ...), the same index; case and dashes
//     do not matter (HKBUTERIMON, omnimon1);
//   - CARDnnn and DIGIPARTnnn (completion codes, always on): the card / Digi-Part numbered nnn,
//     only when the player has none of it. The item is given here, with the game's own calls,
//     and r1 says what happened for the lines the mod adds to the script (patch/mods.cpp,
//     add_wizardmon_codes): 100 a card, 101 a Digi-Part, 102 already owned. A number out of
//     range, or a card the game never gives this way (172-190), is a wrong keyword (-1).

#include "keyword.hpp"
#include "keyword_match.hpp"
#include "text.hpp"
#include "typewriter.hpp"

#include <psx/recomp.h>

#include <cstdint>
#include <optional>
#include <string>

extern "C" {
void f_8004850C(PsxContext* ctx);  // collection_add_card(player, card, count) -> < 0 refused
void f_8004BE48(PsxContext* ctx);  // digipart_give(player, part)
void f_8004C010(PsxContext* ctx);  // digipart_has(player, part) -> nonzero when owned
}

namespace {

using namespace dcb::keyword;

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kV0 = 2;
constexpr uint32_t kKeywordCheck = 0x801EE704u;
constexpr uint32_t kScriptRegs = 0x801F7E80u;   // -> city script state; +8 -> registers (r1 at +4)
constexpr uint32_t kCollection = 0x800E0646u;   // one byte per card, low 3 bits = copies
constexpr uint32_t kCardDb = 0x801DB000u;       // -> card database, 0x134 bytes per card, name at +3
constexpr uint32_t kCardRecord = 0x134u;
constexpr uint32_t kPartEffects = 0x80071990u;  // Digi-Part effect strings, one pointer per part
constexpr int kCards = 301, kParts = 127;
constexpr int kFirstUngivable = 172, kLastUngivable = 190;  // collection_add_card refuses them

constexpr int32_t kWrong = -1, kGotCard = 100, kGotPart = 101, kOwned = 102;

std::string g_gift, g_gift_more;

/// A game string in English: as it is when ASCII, else through the text catalog (empty if none).
std::string english(PsxContext& ctx, const std::string& s) {
    bool ascii = true;
    for (const char c : s) ascii = ascii && static_cast<uint8_t>(c) < 0x80;
    if (ascii) return s;
    std::string out;
    return dcb::text_translate(ctx, s, out) ? out : std::string{};
}

int32_t give_card(PsxContext& ctx, int card) {
    if (card >= kCards || (card >= kFirstUngivable && card <= kLastUngivable)) return kWrong;
    if ((psx_read8(&ctx, kCollection + static_cast<uint32_t>(card)) & 7) != 0) return kOwned;
    ctx.r[kA0] = 0;
    ctx.r[kA1] = static_cast<uint32_t>(card);
    ctx.r[kA2] = 1;
    f_8004850C(&ctx);
    if (static_cast<int32_t>(ctx.r[kV0]) < 0) return kWrong;
    std::string name;
    if (const uint32_t db = psx_read32(&ctx, kCardDb))
        name = english(ctx, dcb::typewriter::read_string(ctx, db + kCardRecord * static_cast<uint32_t>(card) + 3, 32));
    g_gift = (name.empty() ? "Card No." + std::to_string(card) : name) + " Card";
    g_gift_more.clear();
    return kGotCard;
}

int32_t give_part(PsxContext& ctx, int part) {
    if (part >= kParts) return kWrong;
    ctx.r[kA0] = 0;
    ctx.r[kA1] = static_cast<uint32_t>(part);
    f_8004C010(&ctx);
    if (ctx.r[kV0] != 0) return kOwned;
    ctx.r[kA0] = 0;
    ctx.r[kA1] = static_cast<uint32_t>(part);
    f_8004BE48(&ctx);
    g_gift = "Digi-Part " + std::to_string(part);
    std::string effect;
    if (const uint32_t str = psx_read32(&ctx, kPartEffects + 4u * static_cast<uint32_t>(part)))
        effect = english(ctx, dcb::typewriter::read_string(ctx, str, 64));
    for (char& c : effect)
        if (c == '\n') c = ' ';
    g_gift_more = effect;
    return kGotPart;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

}  // namespace

void dcb::keyword_expand_gift(std::string& line) {
    replace_all(line, "{gift_more}", g_gift_more);
    replace_all(line, "{gift}", g_gift);
}

extern "C" {

// SAISEG 801EE704: keyword_check(buf), the entry screen and the keyword match (see above).
void dcb_keyword_check(PsxContext* ctx) {
    const uint32_t buf = ctx->r[kA0];
    psx_call_original(ctx, kKeywordCheck);
    const uint32_t result = psx_read32(ctx, psx_read32(ctx, kScriptRegs) + 8) + 4;
    if (static_cast<int32_t>(psx_read32(ctx, result)) != kWrong) return;  // matched, or cancelled
    const std::string typed = normalize(dcb::typewriter::read_string(*ctx, buf, 13));
    int32_t r = kWrong;
    if (const auto i = keyword_index(typed)) r = *i;
    else if (const auto card = numbered(typed, "CARD")) r = give_card(*ctx, *card);
    else if (const auto part = numbered(typed, "DIGIPART")) r = give_part(*ctx, *part);
    psx_write32(ctx, result, static_cast<uint32_t>(r));
}

}  // extern "C"
