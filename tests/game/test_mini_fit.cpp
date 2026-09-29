// Mini-font layout (src/game/overrides/mini_fit.hpp): proportional widths, tight spacing, the
// choice of full / tight / short card name for a slot, and the short-name file format.

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
    Fit f = fit_name(t, "Agumon", "Agu", 88);
    CHECK(f.text == "Agumon" && f.spacing == Spacing::Normal);
    // "roro": 24 normal, 22 tight (the r-o pairs lose their gap).
    CHECK(measure(t, "roro", Spacing::Normal) == 24 && measure(t, "roro", Spacing::Tight) == 22);
    f = fit_name(t, "roro", "ab", 23);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight && f.width == 22);
    // Too wide even tight: the short name, normal spacing when it fits so.
    f = fit_name(t, "roro", "ab", 20);
    CHECK(f.text == "ab" && f.spacing == Spacing::Normal && f.width == 12);
    // A short name that fits only tight.
    f = fit_name(t, "rorororo", "roro", 23);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight);
    // Nothing fits: the full name, tight (best effort).
    f = fit_name(t, "roro", "abcd", 20);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight && f.width == 22);
    f = fit_name(t, "roro", "", 20);
    CHECK(f.text == "roro" && f.spacing == Spacing::Tight);
    // No budget: always the full name.
    f = fit_name(t, "HerculesKabuterimon", "HrcKabuterimon", 0);
    CHECK(f.text == "HerculesKabuterimon" && f.spacing == Spacing::Normal);
}

void test_parse_short_names() {
    const auto m = parse_short_names(
        "# comment\n"
        "HerculesKabuterimon\tHrcKabuterimon\r\n"
        "\n"
        "Mega Def. Disk *b0\tMega D.Disk *b0\n"
        "no tab line\n"
        "\tempty full\n"
        "empty short\t\n"
        "Last\tNo newline");
    CHECK(m.size() == 3);
    CHECK(m.at("HerculesKabuterimon") == "HrcKabuterimon");
    CHECK(m.at("Mega Def. Disk *b0") == "Mega D.Disk *b0");
    CHECK(m.at("Last") == "No newline");
    CHECK(parse_short_names("").empty());
}

}  // namespace

int main() {
    test_normal_widths();
    test_tight_widths();
    test_layout_positions();
    test_fit_name();
    test_parse_short_names();
    std::puts("mini_fit: ok");
    return 0;
}
