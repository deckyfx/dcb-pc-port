#pragma once
// PSX sound-bank (.VAB / `pBAV`) parser + SPU-ADPCM ("BRR") to PCM converter.
// Reference: psx-spx "SPU ADPCM Samples" (same block format the runtime SPU
// decodes in src/hle/spu/adpcm.hpp: 16-byte blocks, shift/filter header,
// flags, 14 data bytes low-nibble-first, 28 samples per block).
//
// VAB layout (all little-endian), verified byte-for-byte against SLPS-03101
// (A_SE0/SE1, A_BGM_BGMxx, X_BGM_BGMxx):
//
//   0x00  char[4]  magic "pBAV"
//   0x04  u32      version (7)
//   0x08  u32      bank id
//   0x0C  u32      fsize: total bank size in bytes (header included)
//   0x10  u32      reserved (0xEEEE)
//   0x12  u16      ps: programs used
//   0x14  u16      ts: tones used
//   0x16  u16      vs: vags (samples) used
//   0x18  u16      ws: wave size id / reserved (opaque, kept for provenance)
//   0x20  128*16B programs table (16 tones per program)
//   +2080 tone rows, 32 bytes each. A row is structurally genuine when any of
//         its first 8 bytes is non-zero AND bytes 24..31 are the reserved tail
//         `c0 00 c1 00 c2 00 c3 00`. Verified: this rule yields exactly `ts`
//         rows in every bank probed (SE0: 161, SE1: 25, BGM01: 11, X47: 20),
//         while the first-8-nonzero rule alone admits ~2000 garbage rows.
//         Tone row byte 22 (u16) is the vag number (1-based, 1..vs).
//         Note: a few banks carry 1-4 extra structurally-genuine rows with
//         vag=0/center=0 (unused slots the game never triggers); the header
//         `ts` is the authoritative used count, consumers should skip vag=0.
//   waves wave data: concatenated BRR vags. The exact table size (and hence
//         the absolute wave base) is NOT yet pinned down — several placements
//         validate partially but none satisfies header counts + flag walks +
//         vag landmarks jointly. `vab_decode_vag` therefore treats the wave
//         base as a caller-supplied hypothesis and validates each vag walk
//         structurally (in-bounds, LoopEnd-terminated); anything else fails
//         instead of emitting garbage audio.
//
// WAV output is raw decoded samples at 22050 Hz mono (pitch/center are
// playback-time concerns, like TIM pixel dims vs VRAM placement). Tones carry
// ADSR/pan/volume bytes that the ripper records for provenance but does not
// apply.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

struct VabTone {
    uint16_t program = 0;  ///< program slot (0..127) * 16 + tone-in-program
    uint16_t index = 0;    ///< tone slot in the table
    uint16_t vag = 0;      ///< vag number from the tone row (1-based)
    uint8_t vol = 0;       ///< tone volume byte (provenance only)
    uint8_t pan = 0;       ///< tone pan byte (provenance only)
    uint8_t center = 0;    ///< base note / center key (provenance only)
    uint8_t shift = 0;     ///< pitch shift byte (provenance only)
    uint8_t minimum = 0;   ///< key range low (provenance only)
    uint8_t maximum = 0;   ///< key range high (provenance only)
};

struct Vab {
    uint32_t version = 0;
    uint32_t bank_id = 0;
    uint16_t programs = 0, tones = 0, vags = 0, wave_id = 0;
    std::vector<VabTone> active_tones;  ///< genuine tone rows, in table order
    size_t total_size = 0;              ///< fsize: whole bank incl. header
};

/// Parse one VAB at `data[0,size)`. Returns bytes consumed (== fsize), or 0
/// when invalid (bad magic/version, truncated tables, fsize overrunning
/// `size`, no genuine tone rows). Never reads past `size`. Does NOT validate
/// wave placement — see the header note.
size_t parse_vab(const uint8_t* data, size_t size, Vab& out);

/// Scan for every valid VAB in a blob (e.g. a BGM bundle holding SEQ + VAB).
/// Returns (offset, consumed-bytes) pairs in order.
std::vector<std::pair<size_t, size_t>> scan_vabs(const uint8_t* data, size_t size);

/// Decode one vag to mono s16 PCM from an explicit wave area `[waves, waves+waves_size)`.
/// `vag_start_block` is the vag's first 16-byte block relative to `waves`
/// (the caller owns the placement hypothesis). The walk follows blocks until
/// (and including) the first LoopEnd flag; caps at `max_blocks` (default 4096
/// = ~5 s at 22050 Hz) so corrupt flags cannot blow up memory. Returns false
/// (and clears `pcm`) when the start is out of range, the walk leaves the
/// area, or no LoopEnd terminates it — never emits partial audio.
bool decode_vag(const uint8_t* waves, size_t waves_size, size_t vag_start_block,
                std::vector<int16_t>& pcm, size_t max_blocks = 4096);

/// Decode one 16-byte SPU-ADPCM block into 28 samples (same formula as
/// hle::adpcm::decode_spu_block; duplicated here because dcb_vfs must not
/// depend on psx_hle — the dependency runs the other way).
void decode_brr_block(const uint8_t* block, int16_t* out, int16_t& old, int16_t& older);

/// Serialize mono s16 PCM as a 16-bit WAV (22050 Hz by default). Returns false
/// only on absurd sizes (> 1 GiB of samples).
bool write_wav(uint32_t sample_rate, const int16_t* pcm, size_t samples, std::vector<uint8_t>& wav);

}  // namespace vfs
