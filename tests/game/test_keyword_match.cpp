// Wizardmon's spell as typed (src/game/overrides/keyword_match.hpp): JP and US keywords, case
// and dashes, full-width input, the completion codes.

#include "keyword_match.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

using namespace dcb::keyword;

int main() {
    const auto index = [](const std::string& typed) { return keyword_index(normalize(typed)); };
    CHECK(index("OMNIMON-1") == 0 && index("OMEGA1") == 0);  // index 0: Omnimon's first card
    CHECK(index("JIJIMON") == 9 && index("JI2MON") == 9);
    CHECK(index("H-KBUTERIMON") == 5 && index("HKBUTERIMON") == 5 && index("h-kabu") == 5);
    CHECK(index("MTLGARURUMON") == 3 && index("MtlGrr") == 3);
    CHECK(index("\x82\x69\x82\x68\x82\x51\x82\x6c\x82\x6e\x82\x6d") == 9);  // ＪＩ２ＭＯＮ, full-width
    CHECK(index("AERO\x81\x5bV") == 4);                                       // a kana long-vowel mark as the dash
    CHECK(!index("JIJIMO") && !index("") && !index("OMNIMON-3"));
    CHECK(numbered(normalize("CARD000"), "CARD") == 0);
    CHECK(numbered(normalize("card-134"), "CARD") == 134);
    CHECK(numbered(normalize("DIGIPART001"), "DIGIPART") == 1);
    CHECK(numbered(normalize("CARD7"), "CARD") == 7);
    CHECK(!numbered(normalize("CARD"), "CARD") && !numbered(normalize("CARD1234"), "CARD") && !numbered(normalize("CARD1A"), "CARD"));
    CHECK(!numbered(normalize("CARD001"), "DIGIPART"));
    std::puts("keyword match: ok");
    return 0;
}
