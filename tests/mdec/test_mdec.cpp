// MDEC device model tests: table uploads, run-level decoding, IDCT, YUV->RGB and output packing.
// Expected values follow psx-spx "Macroblock Decoder (MDEC)".

#include "mdec/mdec.hpp"

#include <array>
#include <cmath>
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

/// Standard IDCT scale table that libpress's DecDCTReset uploads (psx-spx "MDEC(3)").
constexpr std::array<uint16_t, 64> kStdScale = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707, 0xB8E3, 0x9592, 0x8275,
    0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641,
    0x6A6D, 0xE707, 0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592,
    0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D, 0x5A82,
    0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3,
    0x30FB, 0x89BE, 0x7641, 0xCF04, 0xCF04, 0x7641, 0x89BE, 0x30FB,
    0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707,
};

/// psx-spx zigzag table (natural index -> zigzag position).
constexpr std::array<int, 64> kZigzag = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
    3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

constexpr uint16_t kEob = 0xFE00;
constexpr uint32_t kDepth4 = 0u << 27, kDepth8 = 1u << 27, kDepth24 = 2u << 27, kDepth15 = 3u << 27;
constexpr uint32_t kSigned = 1u << 26, kBit15 = 1u << 25;

uint16_t dc(unsigned q, int v) { return static_cast<uint16_t>((q << 10) | (static_cast<unsigned>(v) & 0x3FFu)); }
uint16_t ac(unsigned run, int v) { return static_cast<uint16_t>((run << 10) | (static_cast<unsigned>(v) & 0x3FFu)); }

/// Uploads both quant tables (every entry `q`, entry 0 = 8 so a DC field of P gives pixel P) and
/// the standard scale table.
void setup(hle::Mdec& m, uint8_t luma_q = 8, uint8_t chroma_q = 8) {
    m.write_command(0x40000001u);
    for (int t = 0; t < 2; ++t) {
        const uint32_t q = t == 0 ? luma_q : chroma_q;
        for (int i = 0; i < 16; ++i) {
            const uint32_t b0 = i == 0 ? 8u : q;
            m.write_command(b0 | q << 8 | q << 16 | q << 24);
        }
    }
    m.write_command(0x60000000u);
    for (size_t i = 0; i < 64; i += 2) m.write_command(kStdScale[i] | uint32_t{kStdScale[i + 1]} << 16);
}

/// Sends MDEC(1) with `mode` bits and the halfwords (padded to a whole word with FE00h) via DMA0.
void decode(hle::Mdec& m, uint32_t mode, std::vector<uint16_t> hw) {
    if (hw.size() % 2) hw.push_back(kEob);
    std::vector<uint32_t> words;
    words.push_back(0x20000000u | mode | static_cast<uint32_t>(hw.size() / 2));
    for (size_t i = 0; i < hw.size(); i += 2) words.push_back(hw[i] | uint32_t{hw[i + 1]} << 16);
    m.dma_write(words.data(), static_cast<uint32_t>(words.size()));
}

std::vector<uint32_t> drain(hle::Mdec& m) {
    std::vector<uint32_t> out(m.output_words_available());
    m.dma_read(out.data(), static_cast<uint32_t>(out.size()));
    return out;
}

int clamp8(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

/// Status bits 15-0.
uint32_t params(const hle::Mdec& m) { return m.status() & 0xFFFFu; }

void test_tables() {
    hle::Mdec m;
    m.write_command(0x40000001u);
    CHECK(m.status() & (1u << 29));   // busy while parameters are pending
    CHECK(params(m) == 31);
    for (uint32_t i = 0; i < 32; ++i) m.write_command((4 * i) | (4 * i + 1) << 8 | (4 * i + 2) << 16 | (4 * i + 3) << 24);
    CHECK(!(m.status() & (1u << 29)));
    CHECK(params(m) == 0xFFFF);
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(m.luma_quant()[i] == i);
        CHECK(m.chroma_quant()[i] == 64 + i);
    }
    // Bit 0 clear: luminance only (16 words); the colour table is untouched.
    m.write_command(0x40000000u);
    CHECK(params(m) == 15);
    for (int i = 0; i < 16; ++i) m.write_command(0x07070707u);
    CHECK(m.luma_quant()[63] == 7 && m.chroma_quant()[0] == 64);
    // Next word is a new command again: MDEC(3) scale table.
    m.write_command(0x60000000u);
    for (size_t i = 0; i < 64; i += 2) m.write_command(kStdScale[i] | uint32_t{kStdScale[i + 1]} << 16);
    for (size_t i = 0; i < 64; ++i) CHECK(m.scale_table()[i] == static_cast<int16_t>(kStdScale[i]));
    CHECK(params(m) == 0xFFFF);
}

void test_mono_dc() {
    hle::Mdec m;
    setup(m);
    CHECK(m.status() & (1u << 31));
    decode(m, kDepth8, {dc(1, 10), kEob});  // DC 10*qt[0]=80 -> IDCT 80/8 = 10 -> +128
    CHECK(m.output_words_available() == 16);
    CHECK(!(m.status() & (1u << 31)));
    CHECK(((m.status() >> 25) & 3u) == 1);   // depth reflected
    CHECK(((m.status() >> 16) & 7u) == 4);   // mono: current block is always Y (4)
    for (const uint32_t w : drain(m)) CHECK(w == 0x8A8A8A8Au);
    CHECK(m.status() & (1u << 31));
    CHECK(m.read_data() == 0);  // empty FIFO

    decode(m, kDepth8 | kSigned, {dc(1, -40), kEob});
    for (const uint32_t w : drain(m)) CHECK(w == 0xD8D8D8D8u);  // -40 as a signed byte
    CHECK(m.status() & (1u << 24));

    decode(m, kDepth4, {dc(1, -40), kEob});  // 88 = 0x58 -> nibble 5
    const auto out4 = drain(m);
    CHECK(out4.size() == 8);
    for (const uint32_t w : out4) CHECK(w == 0x55555555u);

    // q_scale 0: the value is doubled instead of multiplied by the quant table.
    decode(m, kDepth8, {dc(0, 40), kEob});
    for (const uint32_t w : drain(m)) CHECK(w == 0x8A8A8A8Au);
}

/// Colour macroblock with flat blocks: Cr, Cb, Y1 (top-left), Y2, Y3, Y4 (bottom-right).
std::vector<uint16_t> colour_mb(int cr, int cb, std::array<int, 4> y) {
    return {dc(1, cr), kEob, dc(1, cb), kEob, dc(1, y[0]), kEob,
            dc(1, y[1]), kEob, dc(1, y[2]), kEob, dc(1, y[3]), kEob};
}

void test_colour_15bit() {
    hle::Mdec m;
    setup(m);
    const std::array<int, 4> ys = {16, 24, -16, 0};  // multiples of 8: exact in 5 bits
    for (const uint32_t flags : {0u, kBit15, kSigned}) {
        decode(m, kDepth15 | flags, colour_mb(0, 0, ys));
        CHECK(!(m.status() & (1u << 29)));
        const auto out = drain(m);
        CHECK(out.size() == 128);
        for (unsigned i = 0; i < 256; ++i) {
            const unsigned x = i % 16, y = i / 16;
            const int v = ys[(x / 8) + (y / 8) * 2];
            const uint32_t c = static_cast<uint32_t>(flags & kSigned ? (v & 0xFF) : v + 128) >> 3;
            const uint32_t px = c | c << 5 | c << 10 | (flags & kBit15 ? 0x8000u : 0u);
            CHECK(((out[i / 2] >> (16 * (i % 2))) & 0xFFFFu) == px);
        }
    }
}

void test_colour_24bit() {
    hle::Mdec m;
    setup(m);
    const int cr = 16, cb = -8;
    const std::array<int, 4> ys = {10, -20, 100, -128};
    decode(m, kDepth24, colour_mb(cr, cb, ys));
    const auto out = drain(m);
    CHECK(out.size() == 192);
    // psx-spx yuv_to_rgb, with its float factors truncated toward zero.
    const int r = static_cast<int>(1.402 * cr), b = static_cast<int>(1.772 * cb);
    const int g = static_cast<int>(-0.3437 * cb + -0.7143 * cr);
    CHECK(r == 22 && g == -8 && b == -14);
    std::vector<uint8_t> bytes;
    for (const uint32_t w : out)
        for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<uint8_t>(w >> (8 * k)));
    for (unsigned i = 0; i < 256; ++i) {
        const unsigned x = i % 16, y = i / 16;
        const int lum = ys[(x / 8) + (y / 8) * 2];
        CHECK(bytes[3 * i + 0] == clamp8(lum + r) + 128);
        CHECK(bytes[3 * i + 1] == clamp8(lum + g) + 128);
        CHECK(bytes[3 * i + 2] == clamp8(lum + b) + 128);
    }
    // First words for the top-left quadrant: R,G,B = 160,130,124 repeating.
    CHECK(out[0] == 0xA07C82A0u && out[1] == 0x82A07C82u && out[2] == 0x7C82A07Cu);
}

void test_padding_and_eob() {
    hle::Mdec m;
    setup(m);
    // Leading padding, two EOB-terminated blocks, a block that ends at coefficient 63 without an
    // EOB (the following FE00h counts as padding), and trailing padding inside the word count.
    decode(m, kDepth8, {kEob, kEob, dc(1, 1), kEob, kEob, dc(1, 2), kEob, dc(1, 3), ac(62, 0),
                        kEob, kEob, kEob});
    const auto out = drain(m);
    CHECK(out.size() == 48);
    for (size_t i = 0; i < 16; ++i) CHECK(out[i] == 0x81818181u);
    for (size_t i = 16; i < 32; ++i) CHECK(out[i] == 0x82828282u);
    for (size_t i = 32; i < 48; ++i) CHECK(out[i] == 0x83838383u);
    CHECK(params(m) == 0xFFFF && !(m.status() & (1u << 29)));

    // A block cut short by the word count is dropped, and the next command starts cleanly.
    decode(m, kDepth8, {dc(1, 5), ac(0, 3)});
    CHECK(m.output_words_available() == 0);
    decode(m, kDepth8, {dc(1, 4), kEob});
    for (const uint32_t w : drain(m)) CHECK(w == 0x84848484u);
}

void test_colour_status_block() {
    hle::Mdec m;
    setup(m);
    // Stop after Cr, Cb and Y1 have been sent: the current block is Y2 (status value 1).
    auto hw = colour_mb(0, 0, {0, 0, 0, 0});
    std::vector<uint32_t> words = {0x20000000u | kDepth24 | 6u};
    for (size_t i = 0; i < hw.size(); i += 2) words.push_back(hw[i] | uint32_t{hw[i + 1]} << 16);
    m.dma_write(words.data(), 4);  // command + Cr, Cb, Y1
    CHECK(((m.status() >> 16) & 7u) == 1);
    CHECK((m.status() & (1u << 29)) && params(m) == 2);
    CHECK(m.output_words_available() == 0);
    m.dma_write(words.data() + 4, 3);
    CHECK(m.output_words_available() == 192);
}

void test_idct_ac() {
    hle::Mdec m;
    setup(m);  // AC quant 8: value = (a * 8 * q_scale + 4) / 8 = a * q_scale
    decode(m, kDepth8 | kSigned, {dc(1, 5), ac(0, 100), ac(3, -60), ac(10, 40), kEob});
    const auto out = drain(m);
    CHECK(out.size() == 16);
    // Coefficients by zigzag position, placed in natural order via the psx-spx zigzag table.
    std::array<double, 64> coef{};
    std::array<double, 64> by_zz{};
    by_zz[0] = 40;
    by_zz[1] = 100;
    by_zz[5] = -60;
    by_zz[16] = 40;
    for (int i = 0; i < 64; ++i) coef[static_cast<size_t>(i)] = by_zz[static_cast<size_t>(kZigzag[static_cast<size_t>(i)])];
    const double pi = std::acos(-1.0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double sum = 0;
            for (int v = 0; v < 8; ++v) {
                for (int u = 0; u < 8; ++u) {
                    const double cu = u == 0 ? std::sqrt(0.5) : 1.0, cv = v == 0 ? std::sqrt(0.5) : 1.0;
                    sum += cu * cv / 4 * coef[static_cast<size_t>(v * 8 + u)] *
                           std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
                }
            }
            const int want = clamp8(static_cast<int>(std::lround(sum)));
            const auto i = static_cast<unsigned>(y * 8 + x);
            const int got = static_cast<int8_t>(out[i / 4] >> (8 * (i % 4)));
            CHECK(std::abs(got - want) <= 1);
        }
    }
}

void test_reset_and_nop() {
    hle::Mdec m;
    CHECK(m.status() == hle::Mdec::kResetStatus);
    setup(m);
    decode(m, kDepth8, {dc(1, 1), kEob});
    m.write_command(0x20000000u | kDepth24 | 100u);  // command left waiting for parameters
    CHECK(m.status() & (1u << 29));
    m.write_control(0x80000000u);
    CHECK(m.status() == hle::Mdec::kResetStatus);
    CHECK(m.output_words_available() == 0);
    CHECK(m.scale_table()[0] == 0x5A82);  // tables survive a reset
    decode(m, kDepth8, {dc(1, 2), kEob});
    for (const uint32_t w : drain(m)) CHECK(w == 0x82828282u);

    // DMA enables drive the data-in/data-out request bits.
    m.write_control(0x60000000u);
    CHECK(m.dma_in_enabled() && m.dma_out_enabled());
    CHECK((m.status() & (1u << 28)) && !(m.status() & (1u << 27)));
    decode(m, kDepth8, {dc(1, 2), kEob});
    CHECK(m.status() & (1u << 27));
    drain(m);
    CHECK(!(m.status() & (1u << 27)));

    // MDEC(0)/(4-7): no parameters, bits 15-0 mirrored without the "minus 1".
    for (const uint32_t cmd : {0x00001234u, 0x80000005u, 0xE0000000u}) {
        m.write_command(cmd);
        CHECK(!(m.status() & (1u << 29)));
        CHECK(params(m) == (cmd & 0xFFFFu));
    }
    decode(m, kDepth8, {dc(1, 3), kEob});
    CHECK(m.output_words_available() == 16);
}

}  // namespace

int main() {
    test_tables();
    test_mono_dc();
    test_colour_15bit();
    test_colour_24bit();
    test_padding_and_eob();
    test_colour_status_block();
    test_idct_ac();
    test_reset_and_nop();
    std::puts("mdec: ok");
    return 0;
}
