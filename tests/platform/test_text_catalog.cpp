// Text catalog: template matching (literals, %d with padding, %c slot digit, %s), translation
// with the captured values, the file escapes, and loading source.tsv + <lang>.tsv.

#include "text_catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;

// Shift-JIS stand-ins: the matcher works on bytes, so any bytes do.
const std::string kSlot = "\x83\x58\x83\x8D\x83\x62\x83\x67";  // スロット

void test_literal_and_placeholders() {
    text::Catalog c;
    CHECK(c.add("yes", "\x82\xCD\x82\xA2", "Yes"));
    CHECK(c.add("slot", kSlot + "%c\x82\xCC", "*s0Finished loading data from\nMEMORY CARD slot %c."));
    CHECK(c.add("time", "%3d\x8E\x9E\x8A\xD4 %2d\x95\xAA", "%3dh %2dm"));
    CHECK(c.add("player", "P%d:S%d", "Player %d : Slot %d"));
    CHECK(c.add("pct", "%3d.%1dw3c6%%", "%3d.%1d*w3*c6%%"));
    CHECK(c.add("name", "%s\x83\x66\x83\x62\x83\x4E", "%s Deck"));
    std::string out;
    CHECK(c.translate("\x82\xCD\x82\xA2", out) && out == "Yes");
    CHECK(!c.translate("\x82\xCD\x82\xA2!", out));  // whole string only
    CHECK(c.translate(kSlot + "1\x82\xCC", out) && out == "*s0Finished loading data from\nMEMORY CARD slot 1.");
    CHECK(c.translate("  0\x8E\x9E\x8A\xD4 49\x95\xAA", out) && out == "  0h 49m");
    CHECK(c.translate("P1:S2", out) && out == "Player 1 : Slot 2");
    CHECK(c.translate("  7.2w3c6%", out) && out == "  7.2*w3*c6%");
    CHECK(c.translate("New Power\x83\x66\x83\x62\x83\x4E", out) && out == "New Power Deck");
    CHECK(!c.translate("P:S2", out));  // %d needs a digit
    CHECK(c.size() == 6);
}

void test_lone_percent_is_literal() {
    text::Catalog c;
    CHECK(c.add("a", "EXP 30%%.", "EXP boost 30%."));  // the US leaves % unescaped
    CHECK(c.add("b", "100%", "%q"));
    std::string out;
    CHECK(c.translate("EXP 30%.", out) && out == "EXP boost 30%.");
    CHECK(c.translate("100%", out) && out == "%q");
}

void test_escapes_and_load() {
    CHECK(text::Catalog::unescape("a\\nb\\tc\\\\d\\x") == "a\nb\tc\\d\\x");
    const fs::path dir = fs::temp_directory_path() / "dcb_test_text_catalog";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "source.tsv", std::ios::binary) << "# comment\nA:1\tline1\\nline2\nA:2\tonly source\nbad line\n";
    std::ofstream(dir / "en.tsv", std::ios::binary) << "A:1\tFirst\\nSecond\nA:3\tno source\n";
    std::ofstream(dir / "xx.tsv", std::ios::binary) << "A:1\tPremier\n";
    text::Catalog en;
    CHECK(en.load(dir, "en") == 1);
    std::string out;
    CHECK(en.translate("line1\nline2", out) && out == "First\nSecond");
    text::Catalog xx;
    CHECK(xx.load(dir, "xx") == 1 && xx.translate("line1\nline2", out) && out == "Premier");
    text::Catalog none;
    CHECK(none.load(dir, "zz") == 0);
    fs::remove_all(dir);
}

void test_prefix_while_typing() {
    text::Catalog c;
    // "c2" + 4 SJIS characters, then "!" ; English "*c2Welcome*c7!" (8 glyphs)
    const std::string jp = "c2\x83\x66\x83\x57\x83\x5E\x83\x8B!";
    CHECK(c.add("w", jp, "*c2Welcome*c7!"));
    CHECK(c.add("x", "\x82\xA0\x82\xA2", "Yes"));
    CHECK(c.add("y", "\x82\xA0\x82\xA4", "No"));
    std::string out;
    CHECK(!c.translate_prefix(jp, out) || out.size() > 0);            // whole string: translate() handles it
    CHECK(c.translate_prefix("c2\x83\x66", out) && out == "*c2We");   // 1 of 4 -> 2 of 8 glyphs
    CHECK(c.translate_prefix("c2\x83\x66\x83\x57\x83\x5E\x83\x8B", out) && out == "*c2Welcome*c7!");  // all 4 shown: all 8
    CHECK(c.translate_prefix("\x82\xA0", out) && out.empty());        // could be either: nothing yet
    CHECK(!c.translate_prefix("c2", out));                            // no SJIS character shown
    CHECK(!c.translate_prefix("\x83\x41", out));                      // starts nothing we know
    CHECK(c.add("deck", "%s\x83\x66\x83\x62\x83\x4E", "%s Deck"));
    // "c2デ" could also be a deck name + デック, but a template that starts with the drawn bytes
    // themselves wins over "%s" + a literal: those count only when no such template fits
    CHECK(c.translate_prefix("c2\x83\x66", out) && out == "*c2We");
    CHECK(c.translate_prefix("c2\x83\x66\x83\x57", out) && out == "*c2Welc");  // 2 of 4 -> 4 of 8
    CHECK(!c.translate_prefix("New Power\x83\x66\x83\x62\x83\x4E", out));  // a whole %s template
}

// A typed-out line passes through a whole "%sデック" for one frame (the partner select's deck
// descriptions reach "ブイモンがパートナーのc5デック" before "c7です。..."). lookup() keeps it the
// start of the known message instead of "<Japanese> Deck".
void test_lookup_prefers_the_known_message() {
    text::Catalog c;
    const std::string deck = "\x83\x66\x83\x62\x83\x4E";            // デック
    const std::string veemon = "\x83\x75\x83\x43\x83\x82\x83\x93";  // ブイモン
    CHECK(c.add("deck", "%s" + deck, "%s Deck"));
    CHECK(c.add("vee", veemon + "c5" + deck + "c7!!", "A *c5Veemon Deck*c7!"));
    std::string out;
    const std::string typed = veemon + "c5" + deck;  // every SJIS character shown, "c7!!" not yet
    CHECK(c.translate(typed, out) && out == veemon + "c5 Deck");  // whole: the %s template
    CHECK(c.lookup(typed, out) && out == "A *c5Veemon Deck*c7!");  // drawn: the message
    CHECK(c.lookup(veemon.substr(0, 4), out) && out == "A *c5Ve");  // 2 of 7 -> 4 of 14 glyphs
    CHECK(c.lookup(typed + "c7!!", out) && out == "A *c5Veemon Deck*c7!");  // whole message
    CHECK(c.lookup("New Power" + deck, out) && out == "New Power Deck");  // a real deck name
    CHECK(!c.lookup("\x82\xA0", out));
}

// A %s capture ends between characters: モ is 83 82, and 82 is also the lead byte of の (82 CC),
// so byte-wise "モ" + CC could be read as a split モ and "%sの".
void test_str_capture_keeps_characters_whole() {
    text::Catalog c;
    const std::string no = "\x82\xCC";                                // の
    const std::string veemon = "\x83\x75\x83\x43\x83\x82\x83\x93";  // ブイモン
    CHECK(c.add("of", "%s" + no + "!", "%s's!"));
    std::string out;
    CHECK(!c.translate(std::string("\x83\x82") + "\xCC!", out));  // モ + CC + !: no の in it
    CHECK(!c.translate_prefix(veemon.substr(0, 6), out));       // "ブイモ" starts no "%sの!"
    CHECK(c.translate(veemon + no + "!", out) && out == veemon + "'s!");
}

}  // namespace

int main() {
    test_literal_and_placeholders();
    test_lone_percent_is_literal();
    test_escapes_and_load();
    test_prefix_while_typing();
    test_lookup_prefers_the_known_message();
    test_str_capture_keeps_characters_whole();
    std::printf("text_catalog: ok\n");
    return 0;
}
