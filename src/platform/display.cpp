// VRAM display area -> RGBA8888 conversion shared by every backend.

#include "platform.hpp"

#include <algorithm>

namespace platform {

namespace {

constexpr uint32_t kOpaqueBlack = 0xFF000000u;

constexpr uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b) {
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

/// 5-bit channel -> 8-bit, replicating the top bits so 31 maps to 255.
constexpr uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }

/// One 15-bit VRAM pixel (bit 15 = mask, ignored) -> RGBA8888.
constexpr uint32_t convert15(uint16_t px) {
    return pack_rgba(expand5(px & 0x1Fu), expand5((px >> 5) & 0x1Fu), expand5((px >> 10) & 0x1Fu));
}

/// Byte `index` (0..2047) of a VRAM line; VRAM words are little-endian.
inline uint32_t line_byte(const uint16_t* line, int index) {
    const uint16_t word = line[(index >> 1) & (kVramWidth - 1)];
    return (index & 1) ? static_cast<uint32_t>(word >> 8) : static_cast<uint32_t>(word & 0xFFu);
}

}  // namespace

void convert_display(const uint16_t* vram, const DisplayArea& area, uint32_t* out_rgba) {
    if (area.width <= 0 || area.height <= 0) return;
    const size_t count = static_cast<size_t>(area.width) * static_cast<size_t>(area.height);
    if (!area.enabled || vram == nullptr) {
        std::fill_n(out_rgba, count, kOpaqueBlack);
        return;
    }

    for (int row = 0; row < area.height; ++row) {
        // Both X and Y wrap around VRAM, as the display fetcher does on hardware.
        const uint16_t* line = vram + static_cast<size_t>((area.y + row) & (kVramHeight - 1)) * kVramWidth;
        uint32_t* out = out_rgba + static_cast<size_t>(row) * static_cast<size_t>(area.width);
        if (!area.rgb24) {
            for (int col = 0; col < area.width; ++col)
                out[col] = convert15(line[(area.x + col) & (kVramWidth - 1)]);
        } else {
            // 24-bit: the line is a byte stream starting at byte x*2, three bytes (R,G,B) per pixel.
            const int base = area.x * 2;
            for (int col = 0; col < area.width; ++col) {
                const int b = base + col * 3;
                out[col] = pack_rgba(line_byte(line, b), line_byte(line, b + 1), line_byte(line, b + 2));
            }
        }
    }
}

}  // namespace platform
