#pragma once
// ADPCM ("BRR") sample decoding shared by SPU voices and XA audio, plus the SPU's 4-point
// interpolation table. Both formats use the same per-sample formula; they differ only in how
// nibbles and headers are laid out.
// Reference: psx-spx "SPU ADPCM Samples", "SPU ADPCM Pitch" (4-Point Gaussian Interpolation),
// "CDROM XA Audio ADPCM Compression".

#include <array>
#include <cstddef>
#include <cstdint>

namespace hle::adpcm {

/// Filter coefficients (x/64) for filters 0..4; XA only uses 0..3.
inline constexpr std::array<int32_t, 5> kPos{0, 60, 115, 98, 122};
inline constexpr std::array<int32_t, 5> kNeg{0, 0, -52, -55, -60};

/// SPU-ADPCM block header flag bits (2nd byte), psx-spx "Flag Bits".
inline constexpr uint8_t kFlagLoopEnd = 0x01, kFlagLoopRepeat = 0x02, kFlagLoopStart = 0x04;
inline constexpr uint32_t kBlockBytes = 16, kBlockSamples = 28;

constexpr int16_t clamp16(int32_t v) {
    return static_cast<int16_t>(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

/// Shift field: 13..15 are reserved and behave like 9 (psx-spx "XA-ADPCM Header Bytes").
constexpr int shift_of(uint8_t header) {
    const int s = header & 0x0F;
    return s > 12 ? 9 : s;
}

/// One sample: `raw` is the nibble/byte already placed in the top bits of a 16-bit value
/// (nibble << 12 or byte << 8); shift it right, add the filtered history, saturate.
inline int16_t decode_sample(int16_t raw, int shift, int filter, int16_t& old, int16_t& older) {
    const auto f = static_cast<size_t>(filter);
    const int32_t s = (static_cast<int32_t>(raw) >> shift) + ((old * kPos[f] + older * kNeg[f] + 32) >> 6);
    older = old;
    old = clamp16(s);
    return old;
}

/// Decode one 16-byte SPU-ADPCM block (shift/filter, flags, 14 data bytes; low nibble first)
/// into 28 samples. `old`/`older` carry the filter history across blocks. Filters 5..7 are
/// undefined; they are clamped to 4 as other emulators do.
void decode_spu_block(const uint8_t* block, int16_t* out, int16_t& old, int16_t& older);

/// The SPU "gaussian" interpolation table (psx-spx "4-Point Gaussian Interpolation").
extern const std::array<int16_t, 512> kGauss;

/// 4-point interpolation of the four most recent samples at 8-bit fraction `i` (0..255).
inline int32_t interpolate(int16_t oldest, int16_t older, int16_t old, int16_t newest, uint32_t i) {
    int32_t out = (kGauss[0x0FF - i] * oldest) >> 15;
    out += (kGauss[0x1FF - i] * older) >> 15;
    out += (kGauss[0x100 + i] * old) >> 15;
    out += (kGauss[0x000 + i] * newest) >> 15;
    return out;
}

}  // namespace hle::adpcm
