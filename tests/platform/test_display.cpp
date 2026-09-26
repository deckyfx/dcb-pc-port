// convert_display: 15-bit expansion, mask bit, 24-bit packing, disabled display, offsets, wrap.

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

using platform::DisplayArea;
using platform::kVramHeight;
using platform::kVramWidth;

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b) { return r | (g << 8) | (b << 16) | 0xFF000000u; }

constexpr uint16_t bgr15(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>((b << 10) | (g << 5) | r);
}

struct Vram {
    std::vector<uint16_t> words = std::vector<uint16_t>(static_cast<size_t>(kVramWidth) * kVramHeight, 0);
    uint16_t& at(int x, int y) { return words[static_cast<size_t>(y) * kVramWidth + static_cast<size_t>(x)]; }
    /// Write byte `index` of line `y` (little-endian words), as 24-bit mode reads them.
    void set_byte(int index, int y, uint8_t value) {
        uint16_t& w = at(index / 2, y);
        w = (index & 1) ? static_cast<uint16_t>((w & 0x00FF) | (value << 8))
                        : static_cast<uint16_t>((w & 0xFF00) | value);
    }
};

void test_15bit_expansion() {
    Vram vram;
    vram.at(0, 0) = bgr15(31, 0, 0);
    vram.at(1, 0) = bgr15(0, 31, 0);
    vram.at(2, 0) = bgr15(0, 0, 31);
    vram.at(3, 0) = bgr15(1, 16, 30);
    vram.at(0, 1) = 0x0000;
    vram.at(1, 1) = 0x7FFF;
    DisplayArea area;
    area.width = 4;
    area.height = 2;
    std::vector<uint32_t> out(8, 0);
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == rgba(255, 0, 0));
    CHECK(out[1] == rgba(0, 255, 0));
    CHECK(out[2] == rgba(0, 0, 255));
    CHECK(out[3] == rgba((1 << 3) | 0, (16 << 3) | 4, (30 << 3) | 7));  // (v<<3)|(v>>2)
    CHECK(out[4] == rgba(0, 0, 0));
    CHECK(out[5] == rgba(255, 255, 255));
}

void test_mask_bit_ignored() {
    Vram vram;
    vram.at(0, 0) = static_cast<uint16_t>(0x8000 | bgr15(10, 20, 5));
    vram.at(1, 0) = bgr15(10, 20, 5);
    vram.at(2, 0) = 0x8000;
    DisplayArea area;
    area.width = 3;
    area.height = 1;
    std::vector<uint32_t> out(3, 0);
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == out[1]);
    CHECK(out[2] == rgba(0, 0, 0));
}

void test_area_offset_and_wrap() {
    Vram vram;
    vram.at(100, 50) = bgr15(31, 0, 0);
    vram.at(101, 50) = bgr15(0, 31, 0);
    vram.at(100, 51) = bgr15(0, 0, 31);
    DisplayArea area;
    area.x = 100;
    area.y = 50;
    area.width = 2;
    area.height = 2;
    std::vector<uint32_t> out(4, 0);
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == rgba(255, 0, 0));
    CHECK(out[1] == rgba(0, 255, 0));
    CHECK(out[2] == rgba(0, 0, 255));
    CHECK(out[3] == rgba(0, 0, 0));

    // X and Y wrap around the 1024x512 VRAM.
    vram.at(1023, 511) = bgr15(31, 31, 0);
    vram.at(0, 511) = bgr15(0, 31, 31);
    vram.at(1023, 0) = bgr15(31, 0, 31);
    vram.at(0, 0) = bgr15(31, 31, 31);
    area.x = 1023;
    area.y = 511;
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == rgba(255, 255, 0));
    CHECK(out[1] == rgba(0, 255, 255));
    CHECK(out[2] == rgba(255, 0, 255));
    CHECK(out[3] == rgba(255, 255, 255));
}

void test_24bit_packing() {
    Vram vram;
    // Display starts at VRAM x=10 (16-bit units) -> byte 20 of each line; 3 bytes per pixel.
    const int x = 10, y = 7;
    const uint8_t line0[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    const uint8_t line1[] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03, 0xFE, 0xFD, 0xFC};
    for (int i = 0; i < 9; ++i) {
        vram.set_byte(x * 2 + i, y, line0[i]);
        vram.set_byte(x * 2 + i, y + 1, line1[i]);
    }
    // First pixel R=0x11,G=0x22,B=0x33: word x holds 0x2211, low byte of word x+1 is 0x33.
    CHECK(vram.at(x, y) == 0x2211);
    CHECK((vram.at(x + 1, y) & 0xFF) == 0x33);

    DisplayArea area;
    area.x = x;
    area.y = y;
    area.width = 3;
    area.height = 2;
    area.rgb24 = true;
    std::vector<uint32_t> out(6, 0);
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == rgba(0x11, 0x22, 0x33));
    CHECK(out[1] == rgba(0x44, 0x55, 0x66));
    CHECK(out[2] == rgba(0x77, 0x88, 0x99));
    CHECK(out[3] == rgba(0xAA, 0xBB, 0xCC));
    CHECK(out[4] == rgba(0x01, 0x02, 0x03));
    CHECK(out[5] == rgba(0xFE, 0xFD, 0xFC));

    // Start X in 16-bit units: x=11 begins at byte 22 = line0[2..4].
    area.x = 11;
    area.width = 1;
    area.height = 1;
    platform::convert_display(vram.words.data(), area, out.data());
    CHECK(out[0] == rgba(0x33, 0x44, 0x55));

    // A 640-pixel-wide FMV line spans 1920 bytes (960 words); the last pixel is read correctly.
    Vram big;
    big.set_byte(1917, 0, 0x12);
    big.set_byte(1918, 0, 0x34);
    big.set_byte(1919, 0, 0x56);
    DisplayArea fmv;
    fmv.width = 640;
    fmv.height = 1;
    fmv.rgb24 = true;
    std::vector<uint32_t> line(640, 0);
    platform::convert_display(big.words.data(), fmv, line.data());
    CHECK(line[639] == rgba(0x12, 0x34, 0x56));
}

void test_disabled_is_black() {
    Vram vram;
    for (uint16_t& w : vram.words) w = 0x7FFF;
    DisplayArea area;
    area.enabled = false;
    std::vector<uint32_t> out(static_cast<size_t>(area.width) * static_cast<size_t>(area.height), 0x12345678u);
    platform::convert_display(vram.words.data(), area, out.data());
    for (uint32_t px : out) CHECK(px == rgba(0, 0, 0));
}

}  // namespace

int main() {
    test_15bit_expansion();
    test_mask_bit_ignored();
    test_area_offset_and_wrap();
    test_24bit_packing();
    test_disabled_is_black();
    std::puts("platform.display: ok");
    return 0;
}
