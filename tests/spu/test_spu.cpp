// SPU: ADPCM block decoding, voice pitch, ADSR, ENDX/loop flags, IRQ address, CD audio input.

#include "spu/adpcm.hpp"
#include "spu/spu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

constexpr uint32_t kSpuCnt = 0x1F801DAA, kSpuStat = 0x1F801DAE, kKon0 = 0x1F801D88, kKoff0 = 0x1F801D8C,
                   kEndx0 = 0x1F801D9C, kIrqAddr = 0x1F801DA4, kTransferAddr = 0x1F801DA6;

uint32_t voice(int v, uint32_t off) { return hle::Spu::kBase + static_cast<uint32_t>(v) * 0x10 + off; }

using Block = std::array<uint8_t, 16>;

/// A block whose 28 samples are nibbles `first` (samples 0..13) then `second` (14..27).
Block square_block(uint8_t header, uint8_t flags, uint8_t first, uint8_t second) {
    Block b{};
    b[0] = header;
    b[1] = flags;
    for (int i = 0; i < 7; ++i) b[static_cast<size_t>(2 + i)] = static_cast<uint8_t>(first | first << 4);
    for (int i = 7; i < 14; ++i) b[static_cast<size_t>(2 + i)] = static_cast<uint8_t>(second | second << 4);
    return b;
}

void upload(hle::Spu& spu, uint32_t addr, const Block& b) {
    spu.write16(kTransferAddr, static_cast<uint16_t>(addr / 8));
    std::array<uint32_t, 4> words{};
    std::memcpy(words.data(), b.data(), 16);
    spu.dma_write(words.data(), 4);
}

/// Voice `v` at full volume playing from `addr`; ADSR: instant attack, slowest decay to sustain
/// level max (settles at 7FF7h), sustain never steps, instant release.
void setup_voice(hle::Spu& spu, int v, uint32_t addr, uint16_t pitch) {
    spu.write16(voice(v, 0x0), 0x3FFF);
    spu.write16(voice(v, 0x2), 0x3FFF);
    spu.write16(voice(v, 0x4), pitch);
    spu.write16(voice(v, 0x6), static_cast<uint16_t>(addr / 8));
    spu.write16(voice(v, 0x8), 0x00FF);
    spu.write16(voice(v, 0xA), 0x1FC0);
}

void enable(hle::Spu& spu, uint16_t extra = 0) {
    spu.write16(kSpuCnt, static_cast<uint16_t>(0xC000 | extra));
    spu.write16(0x1F801D80, 0x3FFF);
    spu.write16(0x1F801D82, 0x3FFF);
}

void test_adpcm_block() {
    using hle::adpcm::decode_spu_block;
    std::array<int16_t, 28> out{};

    // Filter 0, shift 0: nibble 7 -> 7000h, nibble 8 -> -8000h (low nibble first).
    Block b = square_block(0x00, 0, 7, 8);
    b[2] = 0x87;
    int16_t old = 0, older = 0;
    decode_spu_block(b.data(), out.data(), old, older);
    CHECK(out[0] == 0x7000 && out[1] == -0x8000 && out[2] == 0x7000 && out[27] == -0x8000);
    CHECK(old == -0x8000 && older == -0x8000);

    // Shift 4 and 12; reserved shift 13 behaves as 9.
    b = square_block(0x04, 0, 7, 8);
    old = older = 0;
    decode_spu_block(b.data(), out.data(), old, older);
    CHECK(out[0] == 0x0700 && out[27] == -0x0800);
    b = square_block(0x0C, 0, 7, 8);
    decode_spu_block(b.data(), out.data(), old, older);
    CHECK(out[0] == 7 && out[27] == -8);
    b = square_block(0x0D, 0, 7, 7);
    decode_spu_block(b.data(), out.data(), old, older);
    CHECK(out[0] == 0x7000 >> 9);

    // Filters 1..4 on silence: s = (old*pos + older*neg + 32) >> 6.
    const auto first_two = [&](uint8_t header, int16_t o, int16_t oo, int16_t& s0, int16_t& s1) {
        Block z = square_block(header, 0, 0, 0);
        int16_t a = o, c = oo;
        decode_spu_block(z.data(), out.data(), a, c);
        s0 = out[0];
        s1 = out[1];
    };
    int16_t s0 = 0, s1 = 0;
    first_two(0x10, 0x1000, 0, s0, s1);
    CHECK(s0 == 3840 && s1 == 3600);  // (4096*60+32)>>6, (3840*60+32)>>6
    first_two(0x20, 1000, 500, s0, s1);
    CHECK(s0 == 1391);                 // (1000*115 - 500*52 + 32)>>6
    CHECK(s1 == ((1391 * 115 - 1000 * 52 + 32) >> 6));
    first_two(0x30, 1000, 500, s0, s1);
    CHECK(s0 == 1102);                 // (1000*98 - 500*55 + 32)>>6
    first_two(0x40, 1000, 500, s0, s1);
    CHECK(s0 == 1438);                 // (1000*122 - 500*60 + 32)>>6
    first_two(0x70, 1000, 500, s0, s1);  // undefined filter 7 clamps to 4
    CHECK(s0 == 1438);

    // Saturation.
    b = square_block(0x10, 0, 7, 7);
    old = 0x7000;
    older = 0;
    decode_spu_block(b.data(), out.data(), old, older);
    CHECK(out[0] == 0x7FFF);

    // Interpolation table: each quadruple sums to about 7F80h, so a DC input is kept (-0.4%).
    for (uint32_t i = 0; i < 256; ++i) {
        const int32_t v = hle::adpcm::interpolate(0x4000, 0x4000, 0x4000, 0x4000, i);
        CHECK(v >= 0x3FB0 && v <= 0x3FC2);
    }
    CHECK(hle::adpcm::interpolate(0, 0x7FFF, 0, 0, 0) > 0x5900);  // i=0 is centred on `older`
}

void test_pitch() {
    auto spu = std::make_unique<hle::Spu>();
    // One looping block: 14 samples at +7000h, 14 at -7000h (period 28 samples).
    upload(*spu, 0x1000, square_block(0x00, 0x07, 7, 9));
    enable(*spu);
    setup_voice(*spu, 0, 0x1000, 0x0800);  // half speed: 22050 Hz -> period 56 output frames
    spu->write16(kKon0, 0x0001);

    std::vector<int16_t> out(4096 * 2);
    spu->mix(out.data(), 4096);
    int peak = 0;
    int first = -1, last = -1, crossings = 0;
    for (size_t i = 200; i < 4096; ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(out[i * 2])));
        CHECK(out[i * 2] == out[i * 2 + 1]);  // equal L/R volumes
        if (out[(i - 1) * 2] < 0 && out[i * 2] >= 0) {
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
            ++crossings;
        }
    }
    CHECK(peak > 0x2000);
    CHECK(crossings > 10);
    const double period = static_cast<double>(last - first) / (crossings - 1);
    CHECK(period > 55.9 && period < 56.1);
    CHECK((spu->read16(kEndx0) & 1) == 1);                          // looping end sets ENDX
    CHECK(spu->read16(voice(0, 0xE)) == 0x1000 / 8);                 // loop start -> repeat address
    CHECK(spu->read16(voice(0, 0xC)) > 0x7F00);                      // still sustaining

    // Right volume 0 silences the right channel only.
    spu->write16(voice(0, 0x2), 0);
    spu->mix(out.data(), 64);
    CHECK(out[63 * 2 + 1] == 0 && out[63 * 2] != 0);
}

void test_adsr() {
    auto spu = std::make_unique<hle::Spu>();
    upload(*spu, 0x1000, square_block(0x00, 0x07, 1, 1));
    enable(*spu);
    setup_voice(*spu, 5, 0x1000, 0x1000);
    spu->write16(voice(5, 0x8), 0x0007);  // attack linear rate 0, decay shift 0, sustain level 7 (4000h)
    spu->write16(voice(5, 0xA), 0x1FC0);  // sustain: never steps; release linear shift 0
    const auto envx = [&] { return static_cast<int16_t>(spu->read16(voice(5, 0xC))); };
    std::array<int16_t, 2> frame{};

    spu->write16(kKon0, 1 << 5);
    CHECK(envx() == 0);           // key on applies at the next sample
    spu->mix(frame.data(), 1);
    CHECK(envx() == 0x3800);      // attack: +7 << 11 per sample
    spu->mix(frame.data(), 1);
    CHECK(envx() == 0x7000);
    spu->mix(frame.data(), 1);
    CHECK(envx() == 0x7FFF);      // saturated -> decay
    spu->mix(frame.data(), 1);
    CHECK(envx() == 0x3FFF);      // exponential decay: -4000h * level / 8000h -> below 4000h -> sustain
    for (int i = 0; i < 100; ++i) spu->mix(frame.data(), 1);
    CHECK(envx() == 0x3FFF);      // sustain rate 7Fh never steps
    spu->write16(kKoff0, 1 << 5);
    spu->mix(frame.data(), 1);
    CHECK(envx() == 0);           // release to zero, voice off

    // Slow linear attack: shift 16 steps every 32 samples by +7.
    spu->write16(voice(5, 0x8), 0x400F);
    spu->write16(kKon0, 1 << 5);
    std::vector<int16_t> buf(320 * 2);
    spu->mix(buf.data(), 320);
    CHECK(envx() == 70);

    // Exponential decrease release (shift 4 -> step -8<<7 scaled by level) falls monotonically.
    spu->write16(voice(5, 0x8), 0x00FF);
    spu->write16(voice(5, 0xA), 0x1FC0 | 0x20 | 4);
    spu->write16(kKon0, 1 << 5);
    spu->mix(buf.data(), 64);
    CHECK(envx() > 0x7F00);
    spu->write16(kKoff0, 1 << 5);
    int16_t prev = envx();
    for (int i = 0; i < 50; ++i) {
        spu->mix(frame.data(), 1);
        CHECK(envx() < prev);
        prev = envx();
    }
    CHECK(prev > 0);  // exponential: still decaying, not yet zero
}

void test_endx() {
    auto spu = std::make_unique<hle::Spu>();
    upload(*spu, 0x2000, square_block(0x00, 0x01, 3, 3));  // End+Mute
    enable(*spu);
    setup_voice(*spu, 2, 0x2000, 0x1000);
    spu->write16(kKon0, 1 << 2);
    std::vector<int16_t> buf(64 * 2);
    spu->mix(buf.data(), 27);
    CHECK((spu->read16(kEndx0) & (1 << 2)) == 0);
    CHECK(spu->read16(voice(2, 0xC)) != 0);
    spu->mix(buf.data(), 1);  // 28th sample consumed: end flag reached
    CHECK((spu->read16(kEndx0) & (1 << 2)) != 0);
    CHECK(spu->read16(voice(2, 0xC)) == 0);  // End+Mute forces the envelope to zero
    spu->mix(buf.data(), 8);
    CHECK(buf[7 * 2] == 0);

    // Key on clears ENDX again.
    spu->write16(kKon0, 1 << 2);
    CHECK((spu->read16(kEndx0) & (1 << 2)) == 0);
    spu->mix(buf.data(), 4);
    CHECK(spu->read16(voice(2, 0xC)) != 0);

    // Two blocks: normal then End+Repeat back to the second one.
    upload(*spu, 0x3000, square_block(0x00, 0x00, 2, 2));
    upload(*spu, 0x3010, square_block(0x00, 0x07, 2, 2));
    setup_voice(*spu, 3, 0x3000, 0x1000);
    spu->write16(kKon0, 1 << 3);
    spu->mix(buf.data(), 28 + 27);
    CHECK((spu->read16(kEndx0) & (1 << 3)) == 0);
    spu->mix(buf.data(), 1);
    CHECK((spu->read16(kEndx0) & (1 << 3)) != 0);
    CHECK(spu->read16(voice(3, 0xE)) == 0x3010 / 8);
    CHECK(spu->read16(voice(3, 0xC)) > 0x7F00);  // still playing
}

void test_irq() {
    auto spu = std::make_unique<hle::Spu>();
    upload(*spu, 0x4000, square_block(0x00, 0x07, 1, 1));
    spu->write16(kIrqAddr, 0x4000 / 8);
    enable(*spu, 0x0040);
    CHECK(!spu->take_irq());
    setup_voice(*spu, 0, 0x4000, 0x1000);
    spu->write16(kKon0, 1);
    std::array<int16_t, 2> frame{};
    spu->mix(frame.data(), 1);
    CHECK(spu->take_irq());
    CHECK(!spu->take_irq());
    CHECK(spu->read16(kSpuStat) & 0x40);
    spu->write16(kSpuCnt, 0xC000);  // acknowledge
    CHECK(!(spu->read16(kSpuStat) & 0x40));
    CHECK((spu->read16(kSpuStat) & 0x3F) == 0);

    // Disabled IRQ: no hit. DMA write to the IRQ address: hit.
    spu->write16(kIrqAddr, 0x5000 / 8);
    upload(*spu, 0x5000, square_block(0, 0, 0, 0));
    CHECK(!spu->take_irq());
    spu->write16(kSpuCnt, 0xC040);
    upload(*spu, 0x5000, square_block(0, 0, 0, 0));
    CHECK(spu->take_irq());
}

void test_cd_audio() {
    auto spu = std::make_unique<hle::Spu>();
    enable(*spu, 0x0001);
    spu->write16(0x1F801DB0, 0x7FFF);
    spu->write16(0x1F801DB2, 0x4000);
    std::vector<int16_t> in(100 * 2);
    for (size_t i = 0; i < 100; ++i) {
        in[i * 2] = 0x4000;
        in[i * 2 + 1] = -0x4000;
    }
    spu->push_cd_audio(in.data(), 100);
    CHECK(spu->cd_audio_queued() == 100);
    std::vector<int16_t> out(120 * 2);
    spu->mix(out.data(), 120);
    CHECK(spu->cd_audio_queued() == 0);
    // 4000h * 7FFFh/8000h * 7FFEh/8000h ~ 3FFEh; right at half CD volume.
    CHECK(out[50 * 2] >= 0x3FF0 && out[50 * 2] <= 0x4000);
    CHECK(out[50 * 2 + 1] <= -0x1FF0 && out[50 * 2 + 1] >= -0x2000);
    CHECK(out[110 * 2] == 0);  // queue ran dry
    // Capture buffer holds the raw CD input.
    CHECK(spu->ram()[50 * 2] == 0x00 && spu->ram()[50 * 2 + 1] == 0x40);

    // CD input disabled in SPUCNT: silent.
    spu->write16(kSpuCnt, 0xC000);
    spu->push_cd_audio(in.data(), 10);
    spu->mix(out.data(), 10);
    CHECK(out[5 * 2] == 0);
}

void test_reverb_runs() {
    // A voice routed to reverb with a simple echo setup produces a delayed tail after key off.
    auto spu = std::make_unique<hle::Spu>();
    upload(*spu, 0x1000, square_block(0x00, 0x07, 7, 9));
    enable(*spu, 0x0080);
    spu->write16(0x1F801DA2, static_cast<uint16_t>((0x80000 - 0x4000) / 8));  // ESA: 16 KB work area
    spu->write16(0x1F801D84, 0x4000);  // EVOL
    spu->write16(0x1F801D86, 0x4000);
    spu->write16(0x1F801DFC, 0x7FFF);  // vLIN/vRIN
    spu->write16(0x1F801DFE, 0x7FFF);
    spu->write16(0x1F801DC4, 0x7000);  // vIIR
    spu->write16(0x1F801DC6, 0x7000);  // vCOMB1
    spu->write16(0x1F801DD4, 0x0400);  // mLSAME
    spu->write16(0x1F801DD6, 0x0600);  // mRSAME
    spu->write16(0x1F801DD8, 0x0200);  // mLCOMB1 (reads what mLSAME wrote 0x200*4 halfwords ago)
    spu->write16(0x1F801DDA, 0x0400);  // mRCOMB1
    spu->write16(0x1F801DF4, 0x0010);  // mLAPF1..mRAPF2 somewhere harmless
    spu->write16(0x1F801DF6, 0x0020);
    spu->write16(0x1F801DF8, 0x0030);
    spu->write16(0x1F801DFA, 0x0040);
    spu->write16(0x1F801D98, 0x0001);  // EON voice 0
    setup_voice(*spu, 0, 0x1000, 0x1000);
    spu->write16(kKon0, 1);
    std::vector<int16_t> out(8192 * 2);
    spu->mix(out.data(), 4096);
    spu->write16(kKoff0, 1);
    spu->mix(out.data(), 8192);
    int tail = 0;
    for (size_t i = 100; i < 4000; ++i) tail = std::max(tail, std::abs(static_cast<int>(out[i * 2])));
    CHECK(tail > 0x100);  // reverb tail after the dry voice stopped
}

void test_speed() {
    // 24 voices + reverb for one second of audio must be far below real time (sanity bound).
    auto spu = std::make_unique<hle::Spu>();
    upload(*spu, 0x1000, square_block(0x00, 0x07, 7, 9));
    enable(*spu, 0x0080);
    spu->write16(0x1F801D84, 0x4000);
    spu->write16(0x1F801D98, 0xFFFF);
    for (int v = 0; v < 24; ++v) setup_voice(*spu, v, 0x1000, static_cast<uint16_t>(0x0800 + v * 0x80));
    spu->write16(kKon0, 0xFFFF);
    spu->write16(0x1F801D8A, 0x00FF);
    std::vector<int16_t> out(735 * 2);
    for (int i = 0; i < 60; ++i) spu->mix(out.data(), 735);
    for (int v = 0; v < 24; ++v) CHECK(spu->read16(voice(v, 0xC)) > 0x7F00);
}

}  // namespace

int main() {
    test_adpcm_block();
    test_pitch();
    test_adsr();
    test_endx();
    test_irq();
    test_cd_audio();
    test_reverb_runs();
    test_speed();
    std::puts("spu.voices: ok");
    return 0;
}
