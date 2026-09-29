#pragma once
// Layout of English in the JP mini font (8x7 cells) and micro font (4x5 capitals), shared by
// mini_text.cpp and its unit test: proportional mini glyph placement, the tight spacing used when
// a card name would overrun its slot, card names in capitals and their place in a slot, and the
// US micro layout (fixed 5 px cells, folded capitals, 5x5 icons). Pure logic: no guest memory,
// no VRAM.
// docs/re/text-engine.md §7.10.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace dcb::mini {

constexpr int kSpace = 3;       // proportional space
constexpr int kTightSpace = 2;  // space in tight spacing
constexpr int kIconStep = 8;    // mini icon (01 N) advance
constexpr int kLineStep = 9;    // mini newline

/// Ink of one mini ASCII glyph (0x20..0x7F): first inked column in its cell, inked width, and
/// the rows inked in its first / last inked column (bit v = row v), for tight spacing.
struct Ink {
    int8_t left = 0, width = 0;  // width 0: blank cell
    uint8_t left_rows = 0, right_rows = 0;
};
using InkTable = std::array<Ink, 96>;

/// Normal: 1 px between glyphs, space 3. Tight: 1 px only where the facing columns of two
/// neighbours are inked on the same row (they would merge), else 0; space 2.
enum class Spacing : uint8_t { Normal, Tight };

/// One step of a layout: a glyph (ASCII code) or icon (01 N, `icon` = N) at pen x, or a colour
/// change (0C N, `clut` = N; x unused).
struct Item {
    enum Kind : uint8_t { Glyph, Icon, Colour } kind;
    uint8_t code;
    int x, y;
};

/// Lays `s` out from (x, y) and calls `emit(const Item&)` for each glyph, icon and colour code;
/// returns the width of the widest line (pen advance, the last glyph's 1 px gap included, as the
/// DCB_TRACE_TEXT "prop w=N" figure). Codes as the original mini draw: 01 N icon, 0C N colour,
/// 0A newline. A space right after an icon is dropped (the icon cell has its own gap).
template <class Emit>
int layout(const InkTable& ink, const std::string& s, Spacing spacing, int x, int y, Emit&& emit) {
    const bool tight = spacing == Spacing::Tight;
    const int space = tight ? kTightSpace : kSpace;
    int pen = x, width = 0;
    bool after_icon = false;
    int prev = -1;  // previous glyph (index into ink) on this line, -1 after a space/icon/start
    for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        const bool icon_before = after_icon;
        after_icon = false;
        if (c == 0x01 && i + 1 < s.size()) {
            if (prev >= 0 && tight) pen += 1;  // the 1 px gap of the glyph before, kept
            emit(Item{Item::Icon, static_cast<uint8_t>(s[++i]), pen, y});
            pen += kIconStep;
            after_icon = true;
            prev = -1;
        } else if (c == 0x0C && i + 1 < s.size()) {
            emit(Item{Item::Colour, static_cast<uint8_t>(s[++i]), pen, y});
        } else if (c == 0x0A) {
            if (prev >= 0 && tight) pen += 1;
            width = std::max(width, pen - x);
            pen = x;
            y += kLineStep;
            prev = -1;
        } else if (c == 0x20) {  // tight: 2 px between the words' ink (normal: gap + 3 = 4)
            if (!icon_before) pen += space;
            prev = -1;
        } else if (c > 0x20 && c < 0x80) {
            const Ink& g = ink[c - 0x20];
            if (g.width == 0) {  // a blank cell: keep a space's width
                pen += space;
                prev = -1;
                continue;
            }
            // Tight: the gap before this glyph only where the two would touch on a row.
            if (prev >= 0 && tight && (ink[static_cast<size_t>(prev)].right_rows & g.left_rows)) pen += 1;
            emit(Item{Item::Glyph, c, pen, y});
            pen += g.width + (tight ? 0 : 1);
            prev = c - 0x20;
        }
    }
    if (prev >= 0 && tight) pen += 1;  // trailing gap, as in Normal, so widths compare alike
    return std::max(width, pen - x);
}

/// Width of `s` (see layout).
inline int measure(const InkTable& ink, const std::string& s, Spacing spacing) {
    return layout(ink, s, spacing, 0, 0, [](const Item&) {});
}

/// What to draw for a card name in a slot of `budget` px (pen advance, as measure()).
struct Fit {
    std::string text;
    Spacing spacing = Spacing::Normal;
    int width = 0;
};

/// The name in normal spacing when it fits, else in tight spacing (also when that is still too
/// wide: best effort). budget <= 0: no limit.
inline Fit fit_name(const InkTable& ink, const std::string& name, int budget) {
    const int w = measure(ink, name, Spacing::Normal);
    if (budget <= 0 || w <= budget) return {name, Spacing::Normal, w};
    return {name, Spacing::Tight, measure(ink, name, Spacing::Tight)};
}

/// `s` with a..z folded to A..Z, code arguments (01 N icon, 0C N colour) left alone. Card names
/// are drawn in capitals, as the US build did: the mini font's capitals read well, its lowercase
/// does not (a stray dot on 'a', 'g' like 's').
inline std::string fold_upper(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if ((c == 0x01 || c == 0x0C) && i + 1 < out.size()) {
            ++i;  // the code's argument byte
            continue;
        }
        if (c >= 'a' && c <= 'z') out[i] = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

/// Where a name of pen advance `width` (as measure(): its last inked column is start + width - 2)
/// starts in a slot whose ink may run from `min_x` to `ink_end`: at the caller's own `x` when it
/// ends by `ink_end` there, else moved left just enough to end at `ink_end`, but never before
/// `min_x` (a name wider than the slot starts at min_x and overruns: the fit chose it as best
/// effort).
inline int place(int x, int width, int min_x, int ink_end) {
    if (width <= 0 || x + width - 2 <= ink_end) return x;
    return std::max(min_x, std::min(x, ink_end - width + 2));
}

// ---- Micro font (4x5 capitals: JP 80027EF4, US 80027DE8) ---------------------------------
// The US build draws the battle card panel's card names and support labels in this font. Fixed
// cells: each glyph and each space advances 5 px (4 px glyph + 1 px gap), a newline 6 rows; the
// US renderer folds a..z to A..Z and draws code 01 N as a 5x5 icon (mode 3 of its text_icon,
// US-only art) that advances 6. The JP micro renderer has the same glyph cells (SYSTEM.TIM rows
// 234..253, byte-identical in both discs) but no folding and no icon code, so the port lays the
// string out here and draws glyph by glyph (docs/re/text-engine.md §7.10).

constexpr int kMicroAdvance = 5;      // glyph and space
constexpr int kMicroIconAdvance = 6;  // 01 N icon (5x5)
constexpr int kMicroLineStep = 6;     // newline
constexpr int kMicroIconSize = 5;

/// Lays `s` out in the micro font from (x, y) as the US renderer does: a..z folded to capitals,
/// 01 N icon, 0C N colour, 0A newline; a byte with no micro cell (outside 0x21..0x5F after the
/// fold) advances like a space. Calls `emit(const Item&)` per glyph (folded code), icon and colour
/// code; returns the widest line's pen advance (the last glyph's 1 px gap included, as the mini
/// layout: the last inked column is x + width - 2).
template <class Emit>
int micro_layout(const std::string& s, int x, int y, Emit&& emit) {
    int pen = x, width = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c == 0x01 && i + 1 < s.size()) {
            emit(Item{Item::Icon, static_cast<uint8_t>(s[++i]), pen, y});
            pen += kMicroIconAdvance;
        } else if (c == 0x0C && i + 1 < s.size()) {
            emit(Item{Item::Colour, static_cast<uint8_t>(s[++i]), pen, y});
        } else if (c == 0x0A) {
            width = std::max(width, pen - x);
            pen = x;
            y += kMicroLineStep;
        } else {
            if (c >= 'a' && c <= 'z') c = static_cast<uint8_t>(c - 'a' + 'A');
            if (c > 0x20 && c < 0x60) emit(Item{Item::Glyph, c, pen, y});
            pen += kMicroAdvance;
        }
    }
    return std::max(width, pen - x);
}

/// Width of `s` in the micro font (see micro_layout).
inline int micro_measure(const std::string& s) {
    return micro_layout(s, 0, 0, [](const Item&) {});
}

/// Cell of micro icon `idx` (code 01 N: idx = N - 1) in the US SYSTEM.TIM, as US text_icon mode 3
/// (80029A0C): u = 48 + (idx % 14) * 6, v = 160 + (idx / 14) * 6 (TIM rows; the port's private
/// sheet holds the US rows 48..223).
struct IconCell {
    int u, v;
};
constexpr IconCell micro_icon_cell(int idx) {
    return {48 + (idx % 14) * 6, 160 + (idx / 14) * 6};
}

}  // namespace dcb::mini
