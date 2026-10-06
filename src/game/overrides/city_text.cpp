// City message lines in English (SAISEG). The city scripts come from the US disc (en_text.py
// grafts the US script chunk into each C:\AREAnn.PAK), so their text lines are English; the JP
// city code cannot show them:
//
//   city_msg_build (SAISEG 801E2978, a0 = the script's text): claims one of the three 64-byte
//     line slots at 801F7E88 (city_msg_slot_alloc 801E293C), writes the JP letter spacing "w1",
//     then copies Shift-JIS characters and bare JP codes - and drops every other byte, i.e. all
//     English letters. It has no length check: 60 bytes of text per slot, +60 u16 characters
//     shown, +62 s8 in use (-1 free), +63 s8 length.
//   city_msg_reveal (SAISEG 801E2B94, a0 x, a1 y, a2 slot, a3 OT): the typewriter. Each call
//     shows one more Shift-JIS character and returns 1; once all are shown it draws the whole
//     line and returns -1, and the caller (801E2CC0) moves on to the next line.
//
// For an English line (no Shift-JIS), or a JP line the text catalog translates, the override
// keeps the text host-side and puts a marker in the slot: 0x7F and a line serial. The reveal
// then shows two characters a frame (control codes count as none) through the normal text
// renderer. The shown count stays in the slot (+60) and the text is found by serial, so a save
// state taken mid-line restores the right line at the right point. JP lines take the originals.

#include "keyword.hpp"
#include "text.hpp"
#include "typewriter.hpp"

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

extern "C" {
void o_SAISEG_801E293C(PsxContext* ctx);  // city_msg_slot_alloc(slots) -> slot or 0
}

namespace {

using namespace dcb::typewriter;

constexpr uint32_t kSlots = 0x801F7E88u;  // 3 line slots, 64 bytes each
constexpr uint32_t kSlotSize = 64;
constexpr int kSlotCount = 3;
constexpr uint32_t kBuild = 0x801E2978u;
constexpr uint32_t kReveal = 0x801E2B94u;
constexpr uint8_t kMarker = 0x7F;
constexpr size_t kCharsPerFrame = 2;  // an English letter is about half a JP character wide
constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2;

std::unordered_map<uint32_t, std::string> g_lines;  // serial -> English line
uint32_t g_next_serial = 1;

uint32_t slot_serial(PsxContext& ctx, uint32_t slot) {
    if (psx_read8(&ctx, slot) != kMarker) return 0;
    return psx_read32(&ctx, slot + 4);
}

}  // namespace

namespace {

constexpr uint32_t kGameData = 0x80070C2Cu;  // -> game_data (+0 the player's name)

/// `*h0` -> the player's name (game_data + 0, at most 12 bytes): the city builder's `h0` code
/// (its jump table sends `h` to the name copy, 801E2A64); the US city lines write it `*h0`.
std::string expand_player_name(PsxContext& ctx, const std::string& text) {
    const size_t at = text.find("*h0");
    if (at == std::string::npos) return text;
    const std::string name = read_string(ctx, psx_read32(&ctx, kGameData), 12);
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 3, "*h0") == 0) {
            out += name;
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

}  // namespace

extern "C" {

// SAISEG 801E2978: city_msg_build(text) -> slot index, or -1 when no slot is free.
void dcb_city_msg_build(PsxContext* ctx) {
    const std::string src = read_string(*ctx, ctx->r[kA0]);
    std::string text;
    const bool english = is_english(src);
    if (english) text = src;
    else if (!dcb::text_translate(*ctx, src, text)) return psx_call_original(ctx, kBuild);
    // A line the port rewords; some only right after a given line, so the previous US line is kept.
    static std::string previous;
    if (english) dcb::text_reword_line(text, previous);
    previous = src;
    dcb::keyword_expand_gift(text);  // Wizardmon's completion codes (keyword.cpp), names swapped too
    dcb::text_swap_names(text);  // before the player's own name goes in
    text = expand_player_name(*ctx, text);

    ctx->r[kA0] = kSlots;
    o_SAISEG_801E293C(ctx);  // city_msg_slot_alloc: marks the slot in use, zeroes the shown count
    const uint32_t slot = ctx->r[kV0];
    if (slot == 0) {
        ctx->r[kV0] = static_cast<uint32_t>(-1);
        return;
    }
    const uint32_t serial = g_next_serial++;
    if (g_lines.size() > 4096) g_lines.clear();  // a long session: old serials are long gone
    g_lines[serial] = text;
    psx_write8(ctx, slot, kMarker);
    psx_write8(ctx, slot + 1, 0);
    psx_write32(ctx, slot + 4, serial);
    psx_write8(ctx, slot + 63, static_cast<uint8_t>(std::min<size_t>(visible_count(text), 127)));
    ctx->r[kV0] = (slot - kSlots) / kSlotSize;
}

// SAISEG 801E2B94: city_msg_reveal(x, y, slot, ot) -> 1 while revealing, -1 once shown whole.
void dcb_city_msg_reveal(PsxContext* ctx) {
    const uint32_t slot = ctx->r[kA2];
    const uint32_t serial = slot >= kSlots && slot < kSlots + kSlotCount * kSlotSize ? slot_serial(*ctx, slot) : 0;
    const auto it = serial ? g_lines.find(serial) : g_lines.end();
    if (it == g_lines.end()) return psx_call_original(ctx, kReveal);  // a JP line
    const std::string& text = it->second;
    const size_t total = visible_count(text);
    size_t shown = psx_read16(ctx, slot + 60);
    const bool done = shown >= total;
    if (!done) {
        shown = std::min(total, shown + kCharsPerFrame);
        psx_write16(ctx, slot + 60, static_cast<uint16_t>(shown));
    }
    draw_line(*ctx, static_cast<int>(ctx->r[kA0]), static_cast<int>(ctx->r[kA1]), ctx->r[kA3], prefix(text, shown));
    ctx->r[kV0] = done ? static_cast<uint32_t>(-1) : 1u;
}

}  // extern "C"
