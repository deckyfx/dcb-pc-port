// New cards (src/patch/cards.cpp): the definition format and the extended card database, on a
// synthetic CARD2.CDD with the game's layout.

#include "patch/card_art.hpp"
#include "patch/cards.hpp"

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

namespace {

using patch::Bytes;
namespace cards = patch::cards;

const char* kDef = R"(# a card
[card]
number = 301
base = 2
name = TestMon
specialty = darkness
level = ultimate
hp = 1680
dp = 50
plus_p = 10
fusion = 15
rarity = 30
cross_effect = jamming
support_from = 3
model = 235
model_copy_of = 124
art = custom/cards/test.jpg
[attack circle]
name = Big Hit
power = 780
[attack triangle]
name = Mid Hit
power = 540
[attack cross]
name = Small Hit
power = 300
)";

/// CARD2.CDD's layout: 191 Digimon records (byte 0x30 + card at +0x74.., text, model), 102
/// Options, 8 Digivolve, each record's +3 a recognisable byte.
Bytes database() {
    Bytes b = {'A', 'D', 'C', 'D', 191, 0, 102, 8};
    for (int n = 0; n < 191; ++n) {
        Bytes r(0x134, 0);
        r[3] = static_cast<uint8_t>('a' + n % 26);
        for (size_t i = 0x74; i < 0xE4; ++i) r[i] = static_cast<uint8_t>(n);
        for (size_t i = 0xE7; i < 0xE7 + 76; ++i) r[i] = static_cast<uint8_t>(n + 1);
        r[0xE5] = static_cast<uint8_t>(n);
        b.insert(b.end(), r.begin(), r.end());
    }
    for (int n = 0; n < 102; ++n) b.insert(b.end(), 0xDA, static_cast<uint8_t>(0xA0));
    for (int n = 0; n < 8; ++n) b.insert(b.end(), 0x68, static_cast<uint8_t>(0xB0));
    return b;
}

void test_parse() {
    std::string why;
    const auto c = cards::parse(kDef, &why);
    CHECK(c);
    CHECK(c->number == 301 && c->base == 2 && c->name == "TestMon" && c->specialty == 3 && c->level == 3);
    CHECK(c->hp == 1680 && c->dp == 50 && c->plus_p == 10 && c->fusion == 15 && c->rarity == 30);
    CHECK(c->cross_effect == 10 && c->support_from == 3 && c->model == 235 && c->model_copy_of == 124);
    CHECK(c->attacks[0].name == "Big Hit" && c->attacks[0].power == 780 && c->attacks[2].power == 300);
    // Errors: an unknown key, a bad value, a missing attack.
    CHECK(!cards::parse(std::string(kDef) + "colour = red\n", &why));
    std::string bad = kDef;
    bad.replace(bad.find("darkness"), 8, "shadow");
    CHECK(!cards::parse(bad, &why));
    std::string missing = kDef;
    missing.resize(missing.find("[attack cross]"));
    CHECK(!cards::parse(missing, &why));
}

void test_extend() {
    std::string why;
    const auto c = cards::parse(kDef, &why);
    CHECK(c);
    const Bytes in = database();
    const auto out = cards::extend_database(in, {*c}, &why);
    CHECK(out && out->size() == in.size() + 0x134);
    if (!out) return;
    CHECK(patch::rd16(*out, 4) == 192);  // the Digimon count, so the Option base moves along
    const size_t r = 8 + 191 * 0x134;    // right after the 191 Digimon: index 191
    CHECK(patch::rd16(*out, r) == 301 && (*out)[r + 2] == 0 && std::string(reinterpret_cast<const char*>(&(*out)[r + 3])) == "TestMon");
    CHECK((*out)[r + 0x1A] == 0x33 && (*out)[r + 0x1B] == 50 && (*out)[r + 0x1C] == 10 && patch::rd16(*out, r + 0x1E) == 1680);
    CHECK(patch::rd16(*out, r + 0x20) == 780 && std::string(reinterpret_cast<const char*>(&(*out)[r + 0x26])) == "Big Hit");
    CHECK((*out)[r + 0x74] == 3 && (*out)[r + 0xE3] == 3);  // the support of card 3 ...
    CHECK((*out)[r + 0xE7] == 4);                           // ... and its text
    CHECK((*out)[r + 0xE4] == 10 && (*out)[r + 0xE5] == 235);
    CHECK((*out)[r + 0x134] == 0xA0 && out->back() == 0xB0);  // then the Options and Digivolve, unchanged
    // Not a CARD2.CDD, or a changed layout: refused.
    CHECK(!cards::extend_database(Bytes(16, 0), {*c}, &why));
    Bytes shorter = in;
    shorter.pop_back();
    CHECK(!cards::extend_database(shorter, {*c}, &why));
}

/// M_CARD.ARC's layout: 302 offsets (301 entries and the end), entry n of n % 7 + 1 bytes of n.
Bytes mini_cards() {
    Bytes b(302 * 4);
    for (uint32_t n = 0; n < 301; ++n) {
        for (int i = 0; i < 4; ++i) b[n * 4 + i] = static_cast<uint8_t>(b.size() >> (8 * i));
        b.insert(b.end(), n % 7 + 1, static_cast<uint8_t>(n));
    }
    for (int i = 0; i < 4; ++i) b[301 * 4 + i] = static_cast<uint8_t>(b.size() >> (8 * i));
    return b;
}

void test_mini_cards() {
    std::string why;
    const auto c = cards::parse(kDef, &why);
    CHECK(c);
    const Bytes in = mini_cards();
    // Without art: a copy of the base's entry (card 2: 3 bytes of 2), after a table one word longer.
    const auto out = cards::extend_mini_cards(in, {*c}, {}, &why);
    CHECK(out && out->size() == in.size() + 4 + 3);
    if (!out) return;
    CHECK(patch::rd32(*out, 0) == 303 * 4 && patch::rd32(*out, 302 * 4) == out->size());
    const uint32_t at = patch::rd32(*out, 301 * 4);
    CHECK(out->size() - at == 3 && (*out)[at] == 2);
    CHECK((*out)[patch::rd32(*out, 300 * 4)] == static_cast<uint8_t>(300));  // the old entries moved along
    // With art: that entry.
    const auto art = cards::extend_mini_cards(in, {*c}, {Bytes{9, 9, 9, 9, 9}}, &why);
    CHECK(art && art->size() == in.size() + 4 + 5 && art->back() == 9);
    CHECK(!cards::extend_mini_cards(Bytes(64, 0), {*c}, {}, &why));
}

void test_copy_model() {
    // {type, id, size, data}: the model and texture by model id (124), animations 0-1, an effect.
    Bytes pak;
    const auto entry = [&](int type, int id, Bytes data) {
        for (const int v : {type, id}) pak.insert(pak.end(), {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
        const auto n = static_cast<uint32_t>(data.size());
        pak.insert(pak.end(), {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), 0, 0});
        pak.insert(pak.end(), data.begin(), data.end());
    };
    entry(0, 124, {1, 2, 3, 4});
    entry(1, 0, {5, 6});
    entry(1, 124 % 8, {7});
    entry(2, 503, {8, 9});
    entry(5, 124, {10});
    pak.insert(pak.end(), {0xFF, 0xFF, 0, 0});  // the end: a negative type in 4 bytes, as on the disc
    std::string why;
    const auto out = cards::copy_model(pak, 124, 235, &why);
    CHECK(out && out->size() == pak.size());
    if (!out) return;
    CHECK(patch::rd16(*out, 2) == 235);                              // the model
    CHECK(patch::rd16(*out, 12 + 2) == 0 && patch::rd16(*out, 22 + 2) == 4);  // animations kept
    CHECK(patch::rd16(*out, 31 + 2) == 503);                         // the effect kept
    CHECK(patch::rd16(*out, 41 + 2) == 235);                         // the texture
    Bytes cut = pak;
    cut.resize(20);
    CHECK(!cards::copy_model(cut, 124, 235, &why));
}

void test_art() {
    namespace art = patch::card_art;
    // A red square on white: the white is cut out, the square fills the picture's height.
    art::Image img{20, 20, std::vector<uint8_t>(20 * 20 * 4, 255)};
    for (int y = 5; y < 15; ++y)
        for (int x = 8; x < 12; ++x) {
            uint8_t* p = &img.rgba[static_cast<size_t>(y * 20 + x) * 4];
            p[0] = 220, p[1] = 10, p[2] = 10;
        }
    const auto pic = art::picture(img, 16);
    CHECK(pic && pic->w == 16 && pic->h == 16);
    if (!pic) return;
    const auto at = [&](int x, int y) { return &pic->rgba[static_cast<size_t>(y * 16 + x) * 4]; };
    CHECK(at(7, 10)[0] > 150 && at(7, 10)[1] < 60);  // the square, in the middle
    CHECK(at(0, 0)[0] < 80 && at(0, 0)[1] < 80);      // the backdrop is dark, not white
    CHECK(!art::picture(art::Image{20, 20, std::vector<uint8_t>(20 * 20 * 4, 255)}, 16));  // all background
    // Its own palette: every pixel's colour is close, and no entry is the transparent 0x0000.
    art::Tim8 like;
    like.clut_x = 704, like.clut_y = 248, like.x = 320, like.y = 0;
    const art::Tim8 own = art::own_palette(like, *pic);
    CHECK(own.w == 16 && own.h == 16 && own.pixels.size() == 256);
    for (const uint16_t c : own.palette) CHECK(c != 0);
    const uint16_t red = own.palette[own.pixels[10 * 16 + 7]];
    CHECK((red & 31) > 18 && (red >> 5 & 31) < 8);
    // The TIM round-trips.
    const auto back = art::read_tim8(art::write_tim8(own));
    CHECK(back && back->clut_x == 704 && back->clut_y == 248 && back->x == 320 && back->w == 16 && back->pixels == own.pixels &&
          back->palette == own.palette);
    // What most of three TIMs share: pixel 0 in all, pixel 1 in two of three.
    art::Tim8 a = own, b2 = own, c = own;
    b2.pixels[1] = static_cast<uint8_t>(own.pixels[1] + 1);
    c.pixels[2] = static_cast<uint8_t>(own.pixels[2] + 1), b2.pixels[2] = static_cast<uint8_t>(own.pixels[2] + 2);
    const auto all = art::common_pixels({a, b2, c});
    CHECK(all.keep.size() == 256 && all.keep[0] && !all.keep[1] && !all.keep[2]);
    const auto most = art::common_pixels({a, b2, c}, 60);
    CHECK(most.keep[1] && most.like.pixels[1] == own.pixels[1] && !most.keep[2]);
    CHECK(art::common_pixels({a}).keep.empty());
}

void test_builtin() {
    std::string why;
    const auto list = cards::load_builtin(&why);
    CHECK(why.empty());
    CHECK(!list.empty() && list[0].number == 301);
    CHECK(cards::index_of(301, list.size()) == 191 && cards::index_of(300, list.size()) == -1);
}

}  // namespace

int main() {
    test_parse();
    test_extend();
    test_mini_cards();
    test_copy_model();
    test_art();
    test_builtin();
    std::puts("cards: ok");
    return 0;
}
