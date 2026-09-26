#pragma once
// PSX .TIM parser + CLUT resolver.
// Reference: psx-spx "TIM File Format".
//
// Layout: u32 magic (0x10), u32 flags, optional CLUT block, image block.
//   flags bit 0-1: 0 = 4-bit indexed, 1 = 8-bit indexed, 2 = 16-bit direct.
//   flags bit 3:   CLUT block present.
// Each block: u32 length (including its own 12-byte header), u16 x, u16 y, u16 w, u16 h,
// then payload. Image w is counted in 16-bit units regardless of bit depth, so the pixel
// width is w*4 (4-bit), w*2 (8-bit) or w (16-bit). (x,y) is the VRAM position the game
// uploads the data to with GP0(A0h) — the runtime HD hook keys replacements off the
// pixel *content* hash, not the position, so a TIM matches wherever it is uploaded.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace vfs {

struct TimClut {
    uint16_t x = 0, y = 0;  ///< VRAM position of the CLUT
    uint16_t w = 0, h = 0;  ///< w entries per palette, h palettes
    std::vector<uint16_t> entries;
};

struct Tim {
    uint32_t flags = 0;
    int bpp = 0;  ///< 4, 8 or 16
    TimClut clut;  ///< valid when has_clut
    bool has_clut = false;
    uint16_t img_x = 0, img_y = 0;  ///< VRAM position of the pixel data
    uint16_t img_w = 0, img_h = 0;  ///< w in 16-bit units, h in pixels
    std::vector<uint8_t> pixels;    ///< raw image bytes (img_w * img_h * 2)

    int pixel_width() const { return bpp == 4 ? img_w * 4 : bpp == 8 ? img_w * 2 : img_w; }
    int pixel_height() const { return img_h; }
};

/// Parse one TIM at `data[0,size)`. Returns bytes consumed, or 0 when invalid
/// (bad magic/flags, truncated blocks, insane dimensions). Never reads past `size`.
size_t parse_tim(const uint8_t* data, size_t size, Tim& out);

/// Scan for every non-overlapping valid TIM in a blob (e.g. a DRV payload holding
/// a concatenated TIM strip). Returns (offset, consumed-bytes) pairs in order.
/// False-positive resistant: parse_tim validates lengths, flags and dimensions.
std::vector<std::pair<size_t, size_t>> scan_tims(const uint8_t* data, size_t size);

/// Resolve palette `palette` (clut row) to RGBA8888 (R,G,B,A byte order).
/// Direct-color: raw pixel 0x0000 is transparent, matching the GPU's `texel == 0`
/// rule (Gpu::plot in src/hle/gpu/gpu.cpp). Indexed: a CLUT *entry* of 0x0000
/// means transparent regardless of the pixel index.
///
/// STP (semi-transparency) bit 15 round-trips through the alpha channel so an
/// identity pack (ripped PNGs, unmodified) reproduces VRAM uploads bit-for-bit:
/// alpha 0 = texel 0x0000 (transparent), 255 = opaque (STP clear), 254 = STP set
/// (opaque black 0x8000, semi-transparent texels). Artists: keep alpha in
/// {0, 254, 255}; other values decode as opaque without STP. Box-downsampling
/// averages alpha, so STP survives only in uniform-alpha boxes — author HD art
/// at exact multiples of the TIM size or touch up the alpha afterwards.
bool tim_to_rgba(const Tim& tim, unsigned palette, std::vector<uint8_t>& rgba);

/// 15-bit PSX texel (5:5:5 BGR, red in bits 0-4) -> 8-bit channel, replicating top bits.
inline uint8_t expand5(uint16_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }

/// RGBA -> PSX texel for HD-pack injection: alpha < 128 becomes transparent
/// 0x0000; alpha == 254 (`kStpAlpha`) sets bit 15; anything else is opaque RGB15
/// with the bit clear. Inverse of tim_to_rgba's encoding for {0, 254, 255}.
inline constexpr uint8_t kStpAlpha = 254;
uint16_t rgba_to_psx15(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

}  // namespace vfs
