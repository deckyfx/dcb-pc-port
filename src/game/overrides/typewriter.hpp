#pragma once
// Helpers shared by the overrides that reveal English message lines a few characters a frame
// (city_text.cpp: SAISEG city lines, event_text.cpp: EVOSEG Fusion Shop lines): the line stays
// host-side, the game's slot holds a marker and the count shown.

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <string>

extern "C" void f_8002ADC8(PsxContext* ctx);  // text_draw_grey(x, y, clut, prop, ot@sp16, str@sp20)

namespace dcb::typewriter {

inline bool sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

/// True when `s` has no Shift-JIS character (an English line).
inline bool is_english(const std::string& s) {
    return std::none_of(s.begin(), s.end(), [](char c) { return sjis_lead(static_cast<uint8_t>(c)); });
}

/// The NUL-terminated guest string at `addr` (at most `limit` bytes).
inline std::string read_string(PsxContext& ctx, uint32_t addr, uint32_t limit = 4096) {
    std::string s;
    for (uint32_t i = 0; i < limit; ++i) {
        const uint8_t c = psx_read8(&ctx, addr + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

/// Length of the control code at s[i] (`*` + letter + argument), or 0 when s[i] shows a glyph.
inline size_t code_length(const std::string& s, size_t i) {
    if (s[i] != '*' || i + 1 >= s.size()) return 0;
    const char code = s[i + 1];
    if (code < 'a' || code > 'w') return 2;
    if ((code == 'h' || code == 'w') && i + 2 < s.size() && s[i + 2] == '-') return std::min<size_t>(4, s.size() - i);
    return std::min<size_t>(3, s.size() - i);
}

/// Bytes of the character at s[i]: 2 for a Shift-JIS character (a JP player name inside an
/// English line), else 1 - so a reveal never shows half a character.
inline size_t char_length(const std::string& s, size_t i) {
    return sjis_lead(static_cast<uint8_t>(s[i])) && i + 1 < s.size() ? 2 : 1;
}

/// How many characters of `s` show (control codes excluded).
inline size_t visible_count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t code = code_length(s, i);
        if (code) {
            i += code;
        } else {
            ++n;
            i += char_length(s, i);
        }
    }
    return n;
}

/// The first `visible` characters of `s`, with every control code before the next character.
inline std::string prefix(const std::string& s, size_t visible) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        const size_t code = code_length(s, i);
        if (code) {
            i += code;
            continue;
        }
        if (n == visible) break;
        ++n;
        i += char_length(s, i);
    }
    return s.substr(0, i);
}

/// text_draw_grey(x, y, 7, 1, ot, text) with the text in a frame pushed on the guest stack
/// (CLUT 7 and proportional: what the JP message code passes). Registers a0-a3 are kept.
inline void draw_line(PsxContext& ctx, int x, int y, uint32_t ot, const std::string& text) {
    constexpr int kA0 = 4, kSp = 29;
    const uint32_t regs[4] = {ctx.r[kA0], ctx.r[kA0 + 1], ctx.r[kA0 + 2], ctx.r[kA0 + 3]};
    const uint32_t sp = ctx.r[kSp];
    const uint32_t frame = sp - ((32 + static_cast<uint32_t>(text.size()) + 1 + 7) & ~7u);
    const uint32_t str = frame + 32;
    for (size_t i = 0; i < text.size(); ++i) psx_write8(&ctx, str + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
    psx_write8(&ctx, str + static_cast<uint32_t>(text.size()), 0);
    psx_write32(&ctx, frame + 16, ot);
    psx_write32(&ctx, frame + 20, str);
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA0 + 1] = static_cast<uint32_t>(y);
    ctx.r[kA0 + 2] = 7;
    ctx.r[kA0 + 3] = 1;
    ctx.r[kSp] = frame;
    f_8002ADC8(&ctx);
    ctx.r[kSp] = sp;
    for (int i = 0; i < 4; ++i) ctx.r[kA0 + i] = regs[i];
}

}  // namespace dcb::typewriter
