#include "spu/xa_adpcm.hpp"

#include "spu/adpcm.hpp"

#include <algorithm>

namespace hle {

namespace {

constexpr size_t kSubheader = 16;  // after 12 sync bytes + 4 header bytes
constexpr size_t kData = 24;       // after the subheader and its copy
constexpr size_t kPortions = 18;   // 12h 128-byte sound groups per sector
constexpr size_t kPortionBytes = 128;

// psx-spx "25-point Zigzag Interpolation": Table1..Table7, indices 1..29 (index 0 unused).
constexpr std::array<std::array<int16_t, 29>, 7> kZigzag{{
    {0, 0, 0, 0, 0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054, 0x0034, 0x0009, -0x010A, 0x0400, -0x0A78,
     0x234C, 0x6794, -0x1780, 0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010, 0x0011,
     -0x0008, 0x0003, -0x0001},
    {0, 0, 0, -0x0002, 0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2, -0x00E3, 0x0132, -0x0043, -0x0267,
     0x0C9D, 0x74BB, -0x11B4, 0x09B8, -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005, 0x0006,
     -0x0008, 0x0003, -0x0001, 0},
    {0, 0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3, -0x0192, 0x02B1, -0x039E, 0x04F8,
     -0x05A6, 0x7939, -0x05A6, 0x04F8, -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F, -0x0005,
     -0x0002, 0x0003, -0x0001, 0, 0},
    {0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8, 0x0372, -0x05BF, 0x09B8,
     -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C, -0x0013,
     0x0003, 0, -0x0002, 0, 0, 0},
    {-0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D, 0x0350, -0x0623, 0x0BCD,
     -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400, -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022,
     0x000A, -0x0001, 0, 0x0001, 0, 0, 0},
    {0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B, -0x0548, 0x0AFA, -0x16FA,
     0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347, 0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011,
     -0x0005, 0, 0, 0, 0, 0, 0},
    {-0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347, 0x080E, -0x1249, 0x3C07,
     0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B, -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008,
     0x0002, 0, 0, 0, 0, 0, 0},
}};

}  // namespace

bool XaDecoder::is_audio_sector(const uint8_t* sector) {
    const uint8_t submode = sector[kSubheader + 2];
    return sector[15] == 2 && (submode & 0x04) && (submode & 0x20);
}

void XaDecoder::reset() {
    ch_ = {};
    ring_pos_ = 0;
    six_step_ = 6;
}

// psx-spx "25-point Zigzag Interpolation": every six 37800 Hz samples yield seven 44100 Hz ones.
void XaDecoder::push(int16_t l, int16_t r, std::vector<int16_t>& out) {
    ch_[0].ring[ring_pos_ & 31] = l;
    ch_[1].ring[ring_pos_ & 31] = r;
    ++ring_pos_;
    if (--six_step_ != 0) return;
    six_step_ = 6;
    for (const auto& table : kZigzag) {
        for (const Channel& c : ch_) {
            int32_t sum = 0;
            for (uint32_t i = 1; i <= 29; ++i) sum += c.ring[(ring_pos_ - i) & 31] * table[i - 1];
            out.push_back(adpcm::clamp16(sum >> 15));
        }
    }
}

// psx-spx "CDROM XA Audio ADPCM Compression" (decode_sector / decode_28_nibbles).
bool XaDecoder::decode(const uint8_t* sector, std::vector<int16_t>& out) {
    if (!is_audio_sector(sector)) return false;
    const uint8_t coding = sector[kSubheader + 3];
    const bool stereo = coding & 0x01;
    const bool half_rate = coding & 0x04;  // 18900 Hz
    const bool eight_bit = coding & 0x10;
    const size_t blocks = eight_bit ? 4 : 8;  // 28-sample blocks per 128-byte portion

    // Decode one portion at a time: at most 8 x 28 samples.
    std::array<int16_t, 8 * 28> pcm;
    const auto emit = [&](int16_t l, int16_t r) {
        push(l, r, out);
        if (half_rate) push(l, r, out);  // 18900 Hz: each sample twice through the 37800 Hz path
    };

    for (size_t p = 0; p < kPortions; ++p) {
        const uint8_t* src = sector + kData + p * kPortionBytes;
        for (size_t b = 0; b < blocks; ++b) {
            const uint8_t header = src[4 + b];
            const int shift = adpcm::shift_of(header);
            const int filter = (header >> 4) & 3;
            Channel& c = ch_[stereo ? (b & 1) : 0];
            for (size_t j = 0; j < 28; ++j) {
                const uint32_t word = static_cast<uint32_t>(src[16 + j * 4]) | static_cast<uint32_t>(src[17 + j * 4]) << 8 |
                                      static_cast<uint32_t>(src[18 + j * 4]) << 16 |
                                      static_cast<uint32_t>(src[19 + j * 4]) << 24;
                const uint16_t raw = eight_bit ? static_cast<uint16_t>(((word >> (b * 8)) & 0xFF) << 8)
                                               : static_cast<uint16_t>(((word >> (b * 4)) & 0x0F) << 12);
                pcm[b * 28 + j] = adpcm::decode_sample(static_cast<int16_t>(raw), shift, filter, c.old, c.older);
            }
        }
        if (stereo) {
            // Blocks alternate left/right: (0,1), (2,3), ...
            for (size_t b = 0; b < blocks; b += 2) {
                for (size_t j = 0; j < 28; ++j) emit(pcm[b * 28 + j], pcm[(b + 1) * 28 + j]);
            }
        } else {
            for (size_t i = 0; i < blocks * 28; ++i) emit(pcm[i], pcm[i]);
        }
    }
    return true;
}

void apply_cd_volume(int16_t* stereo, size_t frames, uint8_t l_to_l, uint8_t l_to_r, uint8_t r_to_l,
                     uint8_t r_to_r, bool mute) {
    if (mute) {
        std::fill(stereo, stereo + frames * 2, int16_t{0});
        return;
    }
    if (l_to_l == 0x80 && r_to_r == 0x80 && l_to_r == 0 && r_to_l == 0) return;  // identity
    for (size_t i = 0; i < frames; ++i) {
        const int32_t l = stereo[i * 2], r = stereo[i * 2 + 1];
        stereo[i * 2] = adpcm::clamp16((l * l_to_l + r * r_to_l) >> 7);
        stereo[i * 2 + 1] = adpcm::clamp16((l * l_to_r + r * r_to_r) >> 7);
    }
}

}  // namespace hle
