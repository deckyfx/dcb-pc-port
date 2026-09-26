// PSX sound-bank parser + BRR decoder. See vab.hpp for the verified layout.

#include "vfs/vab.hpp"

#include <cstring>

namespace vfs {

namespace {

uint32_t load32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint16_t load16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

constexpr size_t kHeaderBytes = 32;
constexpr size_t kProgramBytes = 128 * 16;
constexpr size_t kToneBase = kHeaderBytes + kProgramBytes;  // 2080
constexpr size_t kMaxTones = 2048;

// Reserved tail every genuine tone row carries (bytes 24..31).
constexpr uint8_t kToneTail[8] = {0xC0, 0x00, 0xC1, 0x00, 0xC2, 0x00, 0xC3, 0x00};

constexpr int32_t kPos[5] = {0, 60, 115, 98, 122};
constexpr int32_t kNeg[5] = {0, 0, -52, -55, -60};

int16_t clamp16(int32_t v) { return static_cast<int16_t>(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v)); }

bool is_genuine_tone(const uint8_t* row) {
    bool any = false;
    for (int k = 0; k < 8; ++k) {
        if (row[k] != 0) {
            any = true;
            break;
        }
    }
    return any && std::memcmp(row + 24, kToneTail, sizeof kToneTail) == 0;
}

}  // namespace

void decode_brr_block(const uint8_t* block, int16_t* out, int16_t& old, int16_t& older) {
    int shift = block[0] & 0x0F;
    if (shift > 12) shift = 9;
    int filter = (block[0] >> 4) & 7;
    if (filter > 4) filter = 4;
    for (unsigned j = 0; j < 28; ++j) {
        const unsigned nibble = (block[2 + j / 2] >> ((j & 1) * 4)) & 0x0Fu;
        // Sign-extended 4-bit value in the top bits, shifted down, plus filter.
        const int32_t raw = (static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)))) >> shift;
        const int32_t s = raw + ((old * kPos[filter] + older * kNeg[filter] + 32) >> 6);
        older = old;
        old = clamp16(s);
        out[j] = old;
    }
}

size_t parse_vab(const uint8_t* data, size_t size, Vab& out) {
    out = Vab{};
    if (!data || size < kToneBase + 32) return 0;
    if (std::memcmp(data, "pBAV", 4) != 0) return 0;
    const uint32_t version = load32(data + 4);
    if (version != 7 && version != 8) return 0;  // only versions seen in the wild
    const uint32_t fsize = load32(data + 12);
    if (fsize < kToneBase + 32 || fsize > size) return 0;
    if (load16(data + 16) != 0xEEEE) return 0;  // reserved marker all banks carry

    Vab vab;
    vab.version = version;
    vab.bank_id = load32(data + 8);
    vab.programs = load16(data + 18);
    vab.tones = load16(data + 20);
    vab.vags = load16(data + 22);
    vab.wave_id = load16(data + 24);
    if (vab.programs > 128 || vab.tones > 2048 || vab.vags > 2048) return 0;

    const uint8_t* tones = data + kToneBase;
    const size_t tone_avail = (fsize - kToneBase) / 32;
    const size_t tone_count = tone_avail < kMaxTones ? tone_avail : kMaxTones;
    for (size_t t = 0; t < tone_count; ++t) {
        const uint8_t* row = tones + t * 32;
        if (!is_genuine_tone(row)) continue;
        VabTone tone;
        tone.program = static_cast<uint16_t>(t / 16);
        tone.index = static_cast<uint16_t>(t);
        tone.vag = load16(row + 22);
        tone.vol = row[0];
        tone.pan = row[1];
        tone.center = row[2];
        tone.shift = row[3];
        tone.minimum = row[4];
        tone.maximum = row[5];
        vab.active_tones.push_back(tone);
    }
    if (vab.active_tones.empty()) return 0;

    vab.total_size = fsize;
    out = std::move(vab);
    return fsize;
}

std::vector<std::pair<size_t, size_t>> scan_vabs(const uint8_t* data, size_t size) {
    std::vector<std::pair<size_t, size_t>> out;
    if (!data) return out;
    size_t pos = 0;
    Vab vab;
    while (pos + 8 <= size) {
        if (data[pos] != 'p' || data[pos + 1] != 'B' || data[pos + 2] != 'A' || data[pos + 3] != 'V') {
            ++pos;
            continue;
        }
        const size_t used = parse_vab(data + pos, size - pos, vab);
        if (used == 0) {
            ++pos;  // coincidental bytes (e.g. inside wave data)
            continue;
        }
        out.emplace_back(pos, used);
        pos += used;
    }
    return out;
}

bool decode_vag(const uint8_t* waves, size_t waves_size, size_t vag_start_block, std::vector<int16_t>& pcm,
                size_t max_blocks) {
    pcm.clear();
    if (!waves || max_blocks == 0) return false;
    size_t off = vag_start_block * 16;
    if (off + 16 > waves_size) return false;
    int16_t old = 0, older = 0, block[28];
    for (size_t n = 0; n < max_blocks; ++n) {
        if (off + 16 > waves_size) return false;  // walk left the area: fail, no partial audio
        decode_brr_block(waves + off, block, old, older);
        const bool end = (waves[off + 1] & 0x01) != 0;
        pcm.insert(pcm.end(), block, block + 28);
        off += 16;
        if (end) return true;
    }
    return false;  // no LoopEnd within max_blocks: corrupt flags
}

bool write_wav(uint32_t sample_rate, const int16_t* pcm, size_t samples, std::vector<uint8_t>& wav) {
    wav.clear();
    if (samples > (1u << 30)) return false;
    if (samples > 0 && !pcm) return false;
    const uint32_t bytes = static_cast<uint32_t>(samples * 2);
    auto u32 = [&](uint32_t v) {
        wav.push_back(static_cast<uint8_t>(v));
        wav.push_back(static_cast<uint8_t>(v >> 8));
        wav.push_back(static_cast<uint8_t>(v >> 16));
        wav.push_back(static_cast<uint8_t>(v >> 24));
    };
    auto u16 = [&](uint16_t v) {
        wav.push_back(static_cast<uint8_t>(v));
        wav.push_back(static_cast<uint8_t>(v >> 8));
    };
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    u32(36 + bytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(sample_rate);
    u32(sample_rate * 2);
    u16(2);
    u16(16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    u32(bytes);
    for (size_t i = 0; i < samples; ++i) u16(static_cast<uint16_t>(pcm[i]));
    return true;
}

}  // namespace vfs
