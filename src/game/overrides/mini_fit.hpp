#pragma once
// Layout of English in the JP mini font (8x7 cells), shared by mini_text.cpp and its unit test:
// proportional glyph placement, the tight spacing used when a card name would overrun its slot,
// the choice between the full name, the tight full name and a short name, card names in
// capitals and their place in a slot, and the short-name table's file format. Pure logic on an ink table: no guest memory, no VRAM.
// docs/re/text-engine.md §7.10.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

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

/// The full name when it fits, else the full name in tight spacing, else the short name (if
/// any; normal, then tight spacing), else the full name tight (still too wide: best effort).
/// budget <= 0: no limit.
inline Fit fit_name(const InkTable& ink, const std::string& full, const std::string& short_name, int budget) {
    const auto try_fit = [&](const std::string& s, Spacing sp, Fit& out) {
        const int w = measure(ink, s, sp);
        out = {s, sp, w};
        return budget <= 0 || w <= budget;
    };
    Fit f;
    if (try_fit(full, Spacing::Normal, f) || try_fit(full, Spacing::Tight, f)) return f;
    if (!short_name.empty()) {
        Fit s;
        if (try_fit(short_name, Spacing::Normal, s) || try_fit(short_name, Spacing::Tight, s)) return s;
    }
    return f;
}

/// `s` with a..z folded to A..Z, code arguments (01 N icon, 0C N colour) left alone. Card names
/// are drawn in capitals, as the US build did (its 4x5 micro font folds them): the mini font's
/// capitals read well, its lowercase does not (a stray dot on 'a', 'g' like 's').
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

/// Parses the converter's short-name file (assets/<serial>/en_short_names.txt, written by
/// tools/text/short_names.py): "<full name>\t<short name>" per line, '#' comments, blank lines
/// ignored. The full name is the card name as stored in CARD2.CDD (US text, e.g. "Mega Def.
/// Disk *b0"). Returns full -> short.
inline std::unordered_map<std::string, std::string> parse_short_names(const std::string& text) {
    std::unordered_map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        out[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return out;
}

}  // namespace dcb::mini
