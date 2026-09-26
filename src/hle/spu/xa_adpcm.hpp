#pragma once
// XA-ADPCM decoding for CD audio streams (FMV soundtracks, music). The CD-ROM controller decodes
// XA audio sectors itself and feeds 44100 Hz PCM to the SPU's CD input; this class does that part.
// Reference: psx-spx "CDROM XA Subheader, File, Channel, Interleave" and "CDROM XA Audio ADPCM
// Compression" (sector layout, 4/8-bit, mono/stereo, 37800/18900 Hz, 25-point zigzag resampling).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hle {

/// Decodes XA-ADPCM audio sectors to 16-bit PCM (stateful: keeps the per-channel filter history
/// and the resampler state across sectors of one stream).
class XaDecoder {
public:
    static constexpr size_t kRawSector = 2352;
    /// Output frames per sector: 37800 Hz stereo 4-bit = 2016 * 7/6; mono or 18900 Hz doubles it.
    static constexpr size_t kMaxFramesPerSector = 4032 * 7 / 6 * 2;

    /// `sector` is a raw 2352-byte CD sector (sync+header+subheader+2324 data). Appends decoded
    /// frames to `out` as interleaved stereo s16 at 44100 Hz (resampled from 37800/18900 Hz;
    /// mono is duplicated to both channels). Returns false if it is not an XA audio sector.
    bool decode(const uint8_t* sector, std::vector<int16_t>& out);
    /// Forget filter history and resampler state (new stream, seek).
    void reset();

    /// Mode 2 sector with the Audio and Form2 submode bits set.
    static bool is_audio_sector(const uint8_t* sector);
    static uint8_t file_of(const uint8_t* sector) { return sector[16]; }
    static uint8_t channel_of(const uint8_t* sector) { return sector[17] & 0x1F; }

private:
    struct Channel {
        int16_t old = 0, older = 0;       ///< ADPCM filter history
        std::array<int16_t, 32> ring{};   ///< recent 37800 Hz samples for the zigzag filter
    };
    std::array<Channel, 2> ch_{};
    uint32_t ring_pos_ = 0;
    int six_step_ = 6;

    void push(int16_t l, int16_t r, std::vector<int16_t>& out);
};

/// The CD-ROM controller's audio volume matrix (psx-spx "CDROM Controller I/O Ports"): ATV0 L->L
/// (1F801802h bank 2), ATV1 L->R (1F801803h bank 2), ATV2 R->R (1F801801h bank 3), ATV3 R->L
/// (1F801802h bank 3), latched when ADPCTL (1F801803h bank 3) bit 5 is written. Volumes are 0..FFh
/// with 80h = 100%. Scales interleaved stereo in place, saturating; `mute` (Mute command, or
/// ADPCTL bit 0 for XA) silences it.
void apply_cd_volume(int16_t* stereo, size_t frames, uint8_t l_to_l, uint8_t l_to_r, uint8_t r_to_l,
                     uint8_t r_to_r, bool mute = false);

}  // namespace hle
