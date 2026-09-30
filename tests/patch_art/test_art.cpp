// Unit tests for the US art swap helpers (src/patch/art_swap.hpp), mirroring
// tools/assets/tests/test_swap_us_images.py. Run as `test_patch_art <case>`.
// The whole swap is checked against the Python pipeline on real dumps by
// tools/patch/compare_art.sh.

#include "patch/art_swap.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
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

using Bytes = std::vector<uint8_t>;
using Rows = std::vector<std::vector<int>>;
using patch::art::Tim;

Bytes pack4(const Rows& rows) {
    Bytes out;
    for (const auto& r : rows)
        for (size_t x = 0; x + 1 < r.size(); x += 2) out.push_back(static_cast<uint8_t>(r[x] | (r[x + 1] << 4)));
    return out;
}

Rows unpack4(const Bytes& px, int width) {
    Rows out;
    for (size_t at = 0; at < px.size(); at += static_cast<size_t>(width / 2)) {
        std::vector<int> r;
        for (int x = 0; x < width; ++x) r.push_back((px[at + static_cast<size_t>(x / 2)] >> (4 * (x & 1))) & 15);
        out.push_back(r);
    }
    return out;
}

Bytes le16(const std::vector<uint16_t>& v) {
    Bytes out;
    for (uint16_t e : v) {
        out.push_back(static_cast<uint8_t>(e & 0xFF));
        out.push_back(static_cast<uint8_t>(e >> 8));
    }
    return out;
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_prewarp() {
    // Pixel i of a 68-on-64 draw samples texel floor(i * 68 / 64): it shows source column i.
    std::vector<int> src;
    for (int i = 0; i < 64; ++i) src.push_back(i % 15 + 1);
    const Rows out = unpack4(patch::art::prewarp_columns(pack4({src, src, src}), 4, 64, 68, 64), 64);
    CHECK(out.size() == 3);
    for (int i = 0; i < 64 && i * 68 / 64 < 64; ++i) CHECK(out[0][static_cast<size_t>(i * 68 / 64)] == src[static_cast<size_t>(i)]);
    // Texels no pixel samples repeat their left neighbour.
    for (int t : {16, 33, 50}) CHECK(out[0][static_cast<size_t>(t)] == out[0][static_cast<size_t>(t - 1)]);
    // 1:1 is unchanged; other depths are refused.
    Bytes data;
    for (int i = 0; i < 128; ++i) data.push_back(static_cast<uint8_t>(i % 32));
    CHECK(patch::art::prewarp_columns(data, 4, 64, 64, 64) == data);
    CHECK(throws([] { patch::art::prewarp_columns(Bytes(64), 8, 64, 68, 64); }));
}

void test_narrow() {
    std::vector<int> row;
    for (int i = 0; i < 192; ++i) row.push_back(i % 16);
    const Rows out = unpack4(patch::art::narrow_columns(pack4({row, row}), 4, 192, 136, 10), 136);
    std::vector<int> want(row.begin(), row.begin() + 126);
    want.insert(want.end(), row.end() - 10, row.end());
    CHECK(out == Rows({want, want}));
    CHECK(throws([] { patch::art::narrow_columns(Bytes(68), 4, 136, 192, 10); }));
}

Tim banner(patch::art::Rect clut, const std::vector<uint16_t>& entries) {
    Tim t;
    t.bpp = 4;
    t.image = {948, 256, 11, 72};
    t.has_clut = true;
    t.clut = clut;
    t.pixels = {1, 2};
    t.palette = le16(entries);
    return t;
}

void test_reshape() {
    std::vector<uint16_t> both;
    for (int i = 0; i < 32; ++i) both.push_back(static_cast<uint16_t>(0x8000 | (i * 997 % 0x7FFF)));
    const Tim jp = banner({816, 497, 16, 2}, both);
    std::vector<uint16_t> cleared = both;
    cleared[3] &= 0x7FFF;  // the semi-transparency bit may differ
    CHECK(patch::art::is_palette_reshape(jp, banner({816, 497, 32, 1}, cleared)));
    std::vector<uint16_t> recoloured = both;
    recoloured[7] = 0x7FFF;
    CHECK(!patch::art::is_palette_reshape(jp, banner({816, 497, 32, 1}, recoloured)));
    CHECK(!patch::art::is_palette_reshape(jp, banner({832, 497, 32, 1}, both)));  // moved
    CHECK(!patch::art::is_palette_reshape(jp, banner({816, 497, 16, 2}, both)));  // same shape
}

void test_read_tim() {
    // A 4-bit TIM with a 16-entry CLUT and a 2x2 (units) image, after 3 bytes of padding.
    Bytes d = {0, 0, 0, 0x10, 0, 0, 0, 8, 0, 0, 0};
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    const auto u16 = [&](uint16_t v) {
        d.push_back(static_cast<uint8_t>(v & 0xFF));
        d.push_back(static_cast<uint8_t>(v >> 8));
    };
    u32(12 + 32);
    u16(512); u16(239); u16(16); u16(1);
    for (int i = 0; i < 16; ++i) u16(static_cast<uint16_t>(i));
    u32(12 + 8);
    u16(792); u16(0); u16(2); u16(2);
    for (uint8_t c : std::string("ABCDEFGH")) d.push_back(c);
    const auto t = patch::art::read_tim(d, 3);
    CHECK(t && t->bpp == 4 && t->has_clut && t->clut.w == 16 && t->image.x == 792 && t->image.h == 2);
    CHECK(t->palette.size() == 32 && t->pixels == Bytes({'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}));
    CHECK(!patch::art::read_tim(d, 0));
    CHECK(!patch::art::read_tim(d, d.size() - 2));  // cut short: no TIM, no read past the end
}

void test_compose() {
    // The help plate: JP circle (index 2) on the Enter row; US X (index 3) there, triangle below.
    const std::vector<uint16_t> us_pal = {0, 0x1484, 0x2129, 0x4969, 0x03E0, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const std::vector<uint16_t> jp_pal = {0, 0x1484, 0x35DC, 0x4969, 0x03E0, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto plate = [](int enter, int menu) {
        Rows rows(80, std::vector<int>(88, 1));
        for (int y = 26; y < 37; ++y)
            for (int x = 13; x < 24; ++x) rows[static_cast<size_t>(y)][static_cast<size_t>(x)] = enter;
        for (int y = 41; y < 52; ++y)
            for (int x = 13; x < 24; ++x) rows[static_cast<size_t>(y)][static_cast<size_t>(x)] = menu;
        return pack4(rows);
    };
    Tim us, jp;
    us.bpp = jp.bpp = 4;
    us.image = jp.image = {808, 0, 22, 80};
    us.has_clut = jp.has_clut = true;
    us.pixels = plate(3, 4);
    jp.pixels = plate(2, 3);
    us.palette = le16(us_pal);
    jp.palette = le16(jp_pal);
    const std::vector<patch::art::Block> blocks = {{false, 13, 26, 11, 11, 13, 41}, {true, 13, 26, 11, 11, 13, 26}};
    const auto [px, pal] = patch::art::compose_image(us, jp, blocks);
    const Rows rows = unpack4(px, 88);
    CHECK(rows[26][13] == 2);                        // slot 2: no US pixel uses it any more
    CHECK(pal[4] == (0x35DC & 0xFF) && pal[5] == (0x35DC >> 8));  // ... and it takes the JP red
    CHECK(rows[41][13] == 3);                        // the US X, one row down
    CHECK(rows[0][0] == 1);                          // the rest is the US plate
    CHECK(pal[6] == (0x4969 & 0xFF));                // colours still in use are kept
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string c = argv[1];
    if (c == "prewarp") test_prewarp();
    else if (c == "narrow") test_narrow();
    else if (c == "reshape") test_reshape();
    else if (c == "read_tim") test_read_tim();
    else if (c == "compose") test_compose();
    else return 2;
    return 0;
}
