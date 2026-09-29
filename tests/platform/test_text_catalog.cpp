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

}  // namespace

int main() {
    test_literal_and_placeholders();
    test_lone_percent_is_literal();
    test_escapes_and_load();
    std::printf("text_catalog: ok\n");
    return 0;
}
