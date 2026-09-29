#pragma once
// Port-only text codes for the English renderer (src/game/overrides/text.cpp), next to the US
// `*` codes (docs/re/text-engine.md §2.2). Header-only, no game dependency: unit-tested on its own
// (tests/platform/test_text_codes.cpp).

#include <cstddef>

namespace text {

/// `*yN` / `*y-N` (N one digit): draws the rest of the string N px lower (higher with '-'); the
/// pen's line position, the line breaks and the measured height are unchanged. The US has no such
/// code (its table stops at `w`); the port uses it for the short unit labels that replace one JP
/// kanji next to a number in the 6x11 digit font (W, L, Cds, Tot. ...): the digit font draws at
/// y + 1 with 10-row digits (rows y+1..y+10), the US capitals are 10 rows from the top of the cell
/// (y..y+9), so `*y1` puts both on one line (docs/re/text-engine.md §7.9).
///
/// `p` points at the byte after the `*`. Returns the bytes the code takes (2 or 3) and sets `dy`,
/// or 0 when `p` holds no complete `y` code (the caller then treats the byte as it did before).
inline size_t parse_y_code(const char* p, int& dy) {
    if (p[0] != 'y') return 0;
    if (p[1] >= '0' && p[1] <= '9') {
        dy = p[1] - '0';
        return 2;
    }
    if (p[1] == '-' && p[2] >= '0' && p[2] <= '9') {
        dy = -(p[2] - '0');
        return 3;
    }
    return 0;
}

}  // namespace text
