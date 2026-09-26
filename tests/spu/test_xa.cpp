// XA-ADPCM: sector decode, 4/8-bit, mono/stereo, 37800/18900 Hz resampling to 44100 Hz.

#include "spu/xa_adpcm.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using Sector = std::array<uint8_t, hle::XaDecoder::kRawSector>;

/// Synthetic Mode 2 Form 2 audio sector. Every sound unit uses shift 0 / filter 0 and every data
/// word is `word`, so each block holds a constant value.
Sector make_sector(uint8_t coding, uint32_t word, uint8_t submode = 0x64) {
    Sector s{};
    s[0] = 0x00;
    for (int i = 1; i <= 10; ++i) s[static_cast<size_t>(i)] = 0xFF;
    s[15] = 2;  // mode 2
    for (int copy = 0; copy < 2; ++copy) {
        const size_t o = 16 + static_cast<size_t>(copy) * 4;
        s[o] = 1;          // file
        s[o + 1] = 3;      // channel
        s[o + 2] = submode;  // audio | form2 | real-time
        s[o + 3] = coding;
    }
    for (size_t p = 0; p < 18; ++p) {
        uint8_t* portion = s.data() + 24 + p * 128;
        for (size_t i = 0; i < 16; ++i) portion[i] = 0x00;  // shift 0, filter 0
        for (size_t j = 0; j < 28; ++j) {
            for (size_t b = 0; b < 4; ++b) portion[16 + j * 4 + b] = static_cast<uint8_t>(word >> (8 * b));
        }
    }
    return s;
}

}  // namespace

int main() {
    hle::XaDecoder xa;
    std::vector<int16_t> out;

    // 4-bit stereo 37800 Hz: 18 portions x 4 blocks x 28 = 2016 samples/channel -> x7/6 = 2352.
    // Left blocks (even nibbles) = 1 -> 1000h, right blocks (odd nibbles) = Fh -> -1000h.
    Sector s = make_sector(0x01, 0xF1F1F1F1u);
    CHECK(hle::XaDecoder::is_audio_sector(s.data()));
    CHECK(hle::XaDecoder::file_of(s.data()) == 1 && hle::XaDecoder::channel_of(s.data()) == 3);
    CHECK(xa.decode(s.data(), out));
    CHECK(out.size() == 2352 * 2);
    // Steady state after the zigzag filter: DC gain of the tables is ~29700/32768.
    const int16_t l = out[2000 * 2], r = out[2000 * 2 + 1];
    CHECK(l > 3650 && l < 3760);
    CHECK(r < -3650 && r > -3760);
    CHECK(std::abs(out[0]) < 16 && std::abs(out[1]) < 16);  // filter delay: the ring starts silent

    // State carries over: a second sector continues without a new ramp-up.
    CHECK(xa.decode(s.data(), out));
    CHECK(out.size() == 2352 * 2 * 2);
    CHECK(out[(2352 + 2000) * 2] == l && out[(2352 + 2000) * 2 + 1] == r);  // 2352 = 0 mod 7 tables
    CHECK(std::abs(out[2352 * 2] - l) < 16);

    // Mono 4-bit 37800 Hz: 4032 samples -> 4704 frames, both channels equal.
    xa.reset();
    out.clear();
    s = make_sector(0x00, 0x12345678u);
    CHECK(xa.decode(s.data(), out));
    CHECK(out.size() == 4704 * 2);
    bool nonzero = false;
    for (size_t i = 0; i < 4704; ++i) {
        CHECK(out[i * 2] == out[i * 2 + 1]);
        nonzero |= out[i * 2] != 0;
    }
    CHECK(nonzero);

    // Stereo 18900 Hz: every sample twice through the 37800 Hz path -> 4704 frames.
    xa.reset();
    out.clear();
    CHECK(xa.decode(make_sector(0x05, 0xF1F1F1F1u).data(), out));
    CHECK(out.size() == 4704 * 2);
    CHECK(out[4000 * 2] > 3650 && out[4000 * 2 + 1] < -3650);

    // 8-bit stereo 37800 Hz: 4 blocks per portion -> 1008 samples/channel -> 1176 frames.
    // Bytes 10h (left) and F0h (right) -> +1000h / -1000h at shift 0.
    xa.reset();
    out.clear();
    CHECK(xa.decode(make_sector(0x11, 0xF010F010u).data(), out));
    CHECK(out.size() == 1176 * 2);
    CHECK(out[1000 * 2] > 3650 && out[1000 * 2 + 1] < -3650);

    // Not audio: data sector (submode 08h) is rejected and nothing is appended.
    out.clear();
    CHECK(!xa.decode(make_sector(0x01, 0, 0x08).data(), out));
    CHECK(out.empty());

    // CD volume matrix: 80h identity, swap, mono mix, mute.
    std::array<int16_t, 2> f{1000, -2000};
    hle::apply_cd_volume(f.data(), 1, 0x80, 0, 0, 0x80);
    CHECK(f[0] == 1000 && f[1] == -2000);
    hle::apply_cd_volume(f.data(), 1, 0, 0x80, 0x80, 0);
    CHECK(f[0] == -2000 && f[1] == 1000);
    hle::apply_cd_volume(f.data(), 1, 0x40, 0x40, 0x40, 0x40);
    CHECK(f[0] == -500 && f[1] == -500);
    f = {30000, 30000};
    hle::apply_cd_volume(f.data(), 1, 0xFF, 0, 0, 0xFF);
    CHECK(f[0] == 0x7FFF);
    hle::apply_cd_volume(f.data(), 1, 0x80, 0, 0, 0x80, true);
    CHECK(f[0] == 0 && f[1] == 0);

    std::puts("spu.xa_adpcm: ok");
    return 0;
}
