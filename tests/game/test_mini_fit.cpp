// Mini / micro font layout (src/game/overrides/mini_fit.hpp): proportional widths, tight
// spacing, the fit of a card name in a slot, its place, and the US micro layout.

#include "mini_fit.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using namespace dcb::mini;

/// A synthetic ink table: capitals 7 px, lowercase 5 px, 'i' 1 px, '.' 1 px, others 5 px.
/// Row masks: 'l' is a vertical bar (all rows on both sides), 'o' round (rows 2..5, middle),
/// 'r' touches only on the left; everything else full height on both sides.
InkTable table() {
    InkTable t{};
    for (int c = 0x21; c < 0x80; ++c) {
        int w = 5;
        if (c >= 'A' && c <= 'Z') w = 7;
        if (c == 'i' || c == '.') w = 1;
        t[static_cast<size_t>(c - 0x20)] = {0, static_cast<int8_t>(w), 0x7F, 0x7F};
    }
    t['o' - 0x20].left_rows = t['o' - 0x20].right_rows = 0x1C;  // rows 2..4
    t['r' - 0x20].right_rows = 0x02;                             // arm at row 1
    t['v' - 0x20].right_rows = 0x40;                             // row 6 only
    return t;
}

void test_normal_widths() {
    const InkTable t = table();
    CHECK(measure(t, "", Spacing::Normal) == 0);
    CHECK(measure(t, "A", Spacing::Normal) == 8);          // 7 + gap
    CHECK(measure(t, "Ai", Spacing::Normal) == 10);        // 8 + 1 + 1
    CHECK(measure(t, "a b", Spacing::Normal) == 6 + 3 + 6);
    CHECK(measure(t, "\x01\x08 x", Spacing::Normal) == 8 + 6);  // no space after an icon
    CHECK(measure(t, "ab\ncdef", Spacing::Normal) == 24);   // widest line
    CHECK(measure(t, "a\x0C\x05" "b", Spacing::Normal) == 12);  // colour code has no width
}

void test_tight_widths() {
    const InkTable t = table();
    // Full-height neighbours keep their 1 px gap: same width as normal.
    CHECK(measure(t, "ab", Spacing::Tight) == measure(t, "ab", Spacing::Normal));
    // 'o' then 'o': rows 2..4 face rows 2..4 -> gap kept.
    CHECK(measure(t, "oo", Spacing::Tight) == 12);
    // 'v' (right side row 6 only) before 'o' (left rows 2..4): no shared row -> no gap.
    CHECK(measure(t, "vo", Spacing::Tight) == 5 + 5 + 1);
    // 'r' (right side row 1) before 'o': no gap either.
    CHECK(measure(t, "ro", Spacing::Tight) == 11);
    // Space 2 (normal: gap + 3).
    CHECK(measure(t, "a b", Spacing::Normal) == 15);
    CHECK(measure(t, "a b", Spacing::Tight) == 5 + 2 + 5 + 1);
    // Never wider than normal.
    for (const char* s : {"MasterTyrannomon", "Mega Def. Disk \x01\x08", "rvo rvo", "a\nb"})
        CHECK(measure(t, s, Spacing::Tight) <= measure(t, s, Spacing::Normal));
}

void test_layout_positions() {
    const InkTable t = table();
    std::vector<Item> items;
    const int w = layout(t, "vo \x01\x08x", Spacing::Tight, 100, 50, [&](const Item& it) { items.push_back(it); });
    CHECK(items.size() == 4);
    CHECK(items[0].kind == Item::Glyph && items[0].code == 'v' && items[0].x == 100 && items[0].y == 50);
    CHECK(items[1].kind == Item::Glyph && items[1].code == 'o' && items[1].x == 105);  // no gap
    CHECK(items[2].kind == Item::Icon && items[2].code == 8 && items[2].x == 112);     // 110 + space 2
    CHECK(items[3].kind == Item::Glyph && items[3].code == 'x' && items[3].x == 120);
    CHECK(w == 26);
}

void test_fit_name() {
    const InkTable t = table();
    // Fits: unchanged, normal.
    Fit f = fit_name(t, "Agumon", 88);
    CHECK(f.text == "Agumon" && f.spacing == Spacing::Normal && f.width == measure(t, "Agumon", Spacing::Normal));
    // "roro": 24 normal, 22 tight (the r-o pairs lose their gap).
    CHECK(measure(t, "roro", Spacing::Normal) == 24 && measure(t, "roro", Spacing::Tight) == 22);
    f = fit_name(t, "roro", 23);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight && f.width == 22);
    // Too wide even tight: tight anyway (best effort).
    f = fit_name(t, "roro", 20);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight && f.width == 22);
    // No budget: normal.
    f = fit_name(t, "HerculesKabuterimon", 0);
    CHECK(f.text == "HerculesKabuterimon" && f.spacing == Spacing::Normal);
}

void test_fold_upper() {
    CHECK(fold_upper("MasterTyrannomon") == "MASTERTYRANNOMON");
    CHECK(fold_upper("Mega Def. Disk \x01\x08") == "MEGA DEF. DISK \x01\x08");
    CHECK(fold_upper("") == "");
    // Code arguments are left alone, even when they look like lowercase letters.
    CHECK(fold_upper(std::string("a\x01" "b\x0C" "cd")) == std::string("A\x01" "b\x0C" "cD"));
    // A code byte at the very end has no argument: nothing to skip.
    CHECK(fold_upper(std::string("z\x0C")) == std::string("Z\x0C"));
    // Capitals of the synthetic table: 8 px each with the gap.
    CHECK(measure(table(), fold_upper("abc"), Spacing::Normal) == 24);
    // An icon code (a US "*e1" in a card name, 01 0F) keeps its argument byte.
    CHECK(fold_upper(std::string("Omnimon \x01\x0F")) == std::string("OMNIMON \x01\x0F"));
}

void test_place() {
    // Fits from the caller's x (last ink column x + width - 2 <= ink_end): unchanged.
    CHECK(place(72, 40, 56, 150) == 72);
    CHECK(place(72, 80, 56, 150) == 72);  // ink 72 .. 150
    // Wider: moved left just enough to end at ink_end.
    CHECK(place(72, 81, 56, 150) == 71);
    CHECK(place(72, 96, 56, 150) == 56);  // the whole area
    // Wider than the area: from min_x (overruns; the fit chose it as best effort).
    CHECK(place(72, 120, 56, 150) == 56);
    // An empty name stays put.
    CHECK(place(72, 0, 56, 150) == 72);
    // The battle panel's two sides at rest, from sources independent of mini_text.cpp:
    // - name x: the US build's card panel (US 800398A0, disassembly of SLUS_013.28) draws the
    //   name at panel x + 17 - 14 * side; with the panel at x 40 (P1, side 0) and x 164 (P2,
    //   side 1) that is 57 and 167;
    // - usable columns: measured in a battle snapshot (frame 9900 of the headless run, docs
    //   text-engine.md 7.10): P1 fill 56..150 (DP separator at 55, border at 151), P2 fill
    //   167..263 (bar edge at 166, DP box border at 264).
    constexpr int kP1Panel = 40, kP2Panel = 164;
    constexpr int kP1X = kP1Panel + 17 - 14 * 0, kP2X = kP2Panel + 17 - 14 * 1;
    CHECK(kP1X == 57 && kP2X == 167);
    // The longest name (19 capitals, 95) fits both in place.
    CHECK(place(kP1X, 95, 56, 150) == kP1X);
    CHECK(place(kP2X, 95, 167, 263) == kP2X);
    // One letter more: P1 moves left by the one column it has; P2 has none to give.
    CHECK(place(kP1X, 100, 56, 150) == 56);
    CHECK(place(kP2X, 100, 167, 263) == kP2X);
}

void test_micro_layout() {
    // Fixed 5 px cells, the last gap included: the last inked column is x + width - 2.
    CHECK(micro_measure("") == 0);
    CHECK(micro_measure("A") == 5);
    CHECK(micro_measure("HerculesKabuterimon") == 95);  // 19 capitals: ink 94 columns
    CHECK(micro_measure("A B") == 15);                   // a space is a cell too
    CHECK(micro_measure("\x01\x08 to 0") == 6 + 5 * 5);  // icon 6, then " TO 0"
    CHECK(micro_measure("\x0C\x03" "AB") == 10);         // a colour code has no width
    CHECK(micro_measure("AB\nCDE") == 15);                // widest line

    // Positions, the fold to capitals, codes.
    std::vector<Item> items;
    const int w = micro_layout(std::string("a\x01\x0F" "b\x0C\x05 c\nd"), 100, 50,
                               [&](const Item& it) { items.push_back(it); });
    CHECK(items.size() == 6);
    CHECK(items[0].kind == Item::Glyph && items[0].code == 'A' && items[0].x == 100 && items[0].y == 50);
    CHECK(items[1].kind == Item::Icon && items[1].code == 0x0F && items[1].x == 105);
    CHECK(items[2].kind == Item::Glyph && items[2].code == 'B' && items[2].x == 111);
    CHECK(items[3].kind == Item::Colour && items[3].code == 5);
    CHECK(items[4].kind == Item::Glyph && items[4].code == 'C' && items[4].x == 121);  // after a space
    CHECK(items[5].kind == Item::Glyph && items[5].code == 'D' && items[5].x == 100 && items[5].y == 56);
    CHECK(w == 26);

    // No micro cell (0x60 and up after the fold, e.g. '{'): a blank cell.
    items.clear();
    CHECK(micro_layout("{A", 0, 0, [&](const Item& it) { items.push_back(it); }) == 10);
    CHECK(items.size() == 1 && items[0].code == 'A' && items[0].x == 5);
}

void test_micro_icon_cell() {
    // US text_icon mode 3: 14 icons a row, 6 px apart, from (48, 160).
    CHECK(micro_icon_cell(0).u == 48 && micro_icon_cell(0).v == 160);
    CHECK(micro_icon_cell(7).u == 90 && micro_icon_cell(7).v == 160);   // 01 08: the b0 icon
    CHECK(micro_icon_cell(14).u == 48 && micro_icon_cell(14).v == 166);  // 01 0F: Omnimon *e1
}

}  // namespace

int main() {
    test_normal_widths();
    test_tight_widths();
    test_layout_positions();
    test_fit_name();
    test_fold_upper();
    test_place();
    test_micro_layout();
    test_micro_icon_cell();
    std::puts("mini_fit: ok");
    return 0;
}
