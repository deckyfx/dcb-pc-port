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

#include "text.hpp"

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

extern "C" {
void f_8002ADC8(PsxContext* ctx);         // text_draw_grey(x, y, clut, prop, ot@sp16, str@sp20)
void o_SAISEG_801E293C(PsxContext* ctx);  // city_msg_slot_alloc(slots) -> slot or 0
}

namespace {

constexpr uint32_t kSlots = 0x801F7E88u;  // 3 line slots, 64 bytes each
constexpr uint32_t kSlotSize = 64;
constexpr int kSlotCount = 3;
constexpr uint32_t kBuild = 0x801E2978u;
constexpr uint32_t kReveal = 0x801E2B94u;
constexpr uint8_t kMarker = 0x7F;
constexpr size_t kCharsPerFrame = 2;  // an English letter is about half a JP character wide
constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kSp = 29;

std::unordered_map<uint32_t, std::string> g_lines;  // serial -> English line
uint32_t g_next_serial = 1;

bool sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

std::string read_string(PsxContext& ctx, uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint8_t c = psx_read8(&ctx, addr + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

/// Length of the control code at s[i] (`*` + letter + argument), or 0 when s[i] shows a glyph.
size_t code_length(const std::string& s, size_t i) {
    if (s[i] != '*' || i + 1 >= s.size()) return 0;
    const char code = s[i + 1];
    if (code < 'a' || code > 'w') return 2;
    if ((code == 'h' || code == 'w') && i + 2 < s.size() && s[i + 2] == '-') return std::min<size_t>(4, s.size() - i);
    return std::min<size_t>(3, s.size() - i);
}

/// How many characters of `s` show (control codes excluded).
size_t visible_count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t code = code_length(s, i);
        if (code) {
            i += code;
        } else {
            ++n;
            ++i;
        }
    }
    return n;
}

/// The first `visible` characters of `s`, with every control code before the next character.
std::string prefix(const std::string& s, size_t visible) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        const size_t code = code_length(s, i);
        if (code) {
            i += code;
            continue;
        }
        if (n == visible) break;
        ++n;
        ++i;
    }
    return s.substr(0, i);
}

uint32_t slot_serial(PsxContext& ctx, uint32_t slot) {
    if (psx_read8(&ctx, slot) != kMarker) return 0;
    return psx_read32(&ctx, slot + 4);
}

/// text_draw_grey(x, y, 7, 1, ot, text) with the text in a frame pushed on the guest stack.
void draw_line(PsxContext& ctx, int x, int y, uint32_t ot, const std::string& text) {
    const uint32_t regs[4] = {ctx.r[kA0], ctx.r[kA1], ctx.r[kA2], ctx.r[kA3]};
    const uint32_t sp = ctx.r[kSp];
    const uint32_t frame = sp - ((32 + static_cast<uint32_t>(text.size()) + 1 + 7) & ~7u);
    const uint32_t str = frame + 32;
    for (size_t i = 0; i < text.size(); ++i) psx_write8(&ctx, str + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
    psx_write8(&ctx, str + static_cast<uint32_t>(text.size()), 0);
    psx_write32(&ctx, frame + 16, ot);
    psx_write32(&ctx, frame + 20, str);
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y);
    ctx.r[kA2] = 7;  // the JP call's CLUT and proportional flag
    ctx.r[kA3] = 1;
    ctx.r[kSp] = frame;
    f_8002ADC8(&ctx);
    ctx.r[kSp] = sp;
    for (int i = 0; i < 4; ++i) ctx.r[kA0 + i] = regs[i];
}

}  // namespace

extern "C" {

// SAISEG 801E2978: city_msg_build(text) -> slot index, or -1 when no slot is free.
void dcb_city_msg_build(PsxContext* ctx) {
    const std::string src = read_string(*ctx, ctx->r[kA0]);
    std::string text;
    const bool english = std::none_of(src.begin(), src.end(), [](char c) { return sjis_lead(static_cast<uint8_t>(c)); });
    if (english) text = src;
    else if (!dcb::text_translate(*ctx, src, text)) return psx_call_original(ctx, kBuild);

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
