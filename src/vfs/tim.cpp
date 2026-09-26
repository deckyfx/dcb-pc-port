// PSX .TIM parser + CLUT resolver. See tim.hpp for the format summary.

#include "vfs/tim.hpp"

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

}  // namespace

size_t parse_tim(const uint8_t* data, size_t size, Tim& out) {
    out = Tim{};
    if (data == nullptr || size < 8) return 0;
    if (load32(data) != 0x10) return 0;
    const uint32_t flags = load32(data + 4);
    const unsigned mode = flags & 7u;
    if (mode > 2u) return 0;  // 3 = mixed, never used by real TIMs
    const bool has_clut = (flags & 8u) != 0;
    if ((flags & ~0xFu) != 0) return 0;  // bits 4+ are undefined; refuse rather than guess

    size_t off = 8;
    Tim tim;
    tim.flags = flags;
    tim.bpp = mode == 0 ? 4 : mode == 1 ? 8 : 16;

    if (has_clut) {
        if (size - off < 12) return 0;
        const uint32_t clen = load32(data + off);
        if (clen < 12 || clen > size - off) return 0;
        const uint16_t cw = load16(data + off + 8), ch = load16(data + off + 10);
        // Sanity: 4-bit TIMs use 16-entry palettes, 8-bit TIMs 256-entry ones.
        if (cw == 0 || ch == 0 || cw > 256 || ch > 512) return 0;
        const size_t entries = static_cast<size_t>(cw) * ch;
        if (entries * 2 + 12 != clen) return 0;
        tim.clut.x = load16(data + off + 4);
        tim.clut.y = load16(data + off + 6);
        tim.clut.w = cw;
        tim.clut.h = ch;
        tim.clut.entries.resize(entries);
        const uint8_t* src = data + off + 12;
        for (size_t i = 0; i < entries; ++i) tim.clut.entries[i] = load16(src + i * 2);
        tim.has_clut = true;
        off += clen;
    } else if (tim.bpp != 16) {
        return 0;  // indexed TIM without a palette is corrupt
    }

    if (size - off < 12) return 0;
    const uint32_t ilen = load32(data + off);
    if (ilen < 12 || ilen > size - off) return 0;
    const uint16_t iw = load16(data + off + 8), ih = load16(data + off + 10);
    if (iw == 0 || ih == 0 || iw > 512 || ih > 512) return 0;
    const size_t want = static_cast<size_t>(iw) * ih * 2;
    if (want + 12 != ilen) return 0;
    tim.img_x = load16(data + off + 4);
    tim.img_y = load16(data + off + 6);
    tim.img_w = iw;
    tim.img_h = ih;
    tim.pixels.assign(data + off + 12, data + off + 12 + want);
    off += ilen;

    out = std::move(tim);
    return off;
}

std::vector<std::pair<size_t, size_t>> scan_tims(const uint8_t* data, size_t size) {
    std::vector<std::pair<size_t, size_t>> out;
    if (!data) return out;
    size_t pos = 0;
    Tim tim;
    while (pos + 8 <= size) {
        // Fast-path: TIM magic is 0x10 little-endian.
        if (data[pos] != 0x10 || data[pos + 1] != 0 || data[pos + 2] != 0 || data[pos + 3] != 0) {
            ++pos;
            continue;
        }
        const size_t used = parse_tim(data + pos, size - pos, tim);
        if (used == 0) {
            ++pos;  // coincidental 0x10: keep scanning (e.g. pixel data containing it)
            continue;
        }
        out.emplace_back(pos, used);
        pos += used;
    }
    return out;
}

bool tim_to_rgba(const Tim& tim, unsigned palette, std::vector<uint8_t>& rgba) {
    const int w = tim.pixel_width(), h = tim.pixel_height();
    if (w <= 0 || h <= 0) return false;
    rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);

    if (tim.bpp == 16) {
        if (tim.pixels.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 2) return false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 2;
                const uint16_t px = static_cast<uint16_t>(tim.pixels[i] | (tim.pixels[i + 1] << 8));
                uint8_t* dst = &rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
                if (px == 0) continue;  // fully transparent, like the GPU's texel == 0 rule
                dst[0] = expand5(static_cast<uint16_t>(px & 0x1F));
                dst[1] = expand5(static_cast<uint16_t>((px >> 5) & 0x1F));
                dst[2] = expand5(static_cast<uint16_t>((px >> 10) & 0x1F));
                dst[3] = (px & 0x8000u) ? kStpAlpha : 255;
            }
        }
        return true;
    }

    if (!tim.has_clut || tim.clut.entries.empty()) return false;
    const size_t per = tim.clut.w;  // entries per palette
    if (per == 0) return false;
    const size_t palettes = tim.clut.entries.size() / per;
    if (palette >= palettes) return false;
    const uint16_t* pal = tim.clut.entries.data() + static_cast<size_t>(palette) * per;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned index = 0;
            if (tim.bpp == 4) {
                const size_t byte = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) / 2;
                // TIM 4-bit texels are little-nibble-first: even x = low nibble.
                const uint8_t b = tim.pixels[byte / 1];
                index = (x & 1) ? (b >> 4) : (b & 0xF);
            } else {
                index = tim.pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            }
            if (index >= per) return false;
            const uint16_t entry = pal[index];
            uint8_t* dst = &rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
            if (entry == 0) continue;  // transparent CLUT entry
            dst[0] = expand5(static_cast<uint16_t>(entry & 0x1F));
            dst[1] = expand5(static_cast<uint16_t>((entry >> 5) & 0x1F));
            dst[2] = expand5(static_cast<uint16_t>((entry >> 10) & 0x1F));
            dst[3] = (entry & 0x8000u) ? kStpAlpha : 255;
        }
    }
    return true;
}

uint16_t rgba_to_psx15(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a < 128) return 0;
    const uint16_t px =
        static_cast<uint16_t>(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
    // 254 marks STP-set pixels (from tim_to_rgba or artist-authored HD art).
    // RGB 0,0,0 with alpha 255 stays STP-clear (plain opaque black 0x0000 would
    // be transparent, so black art uses STP-set 0x8000 — as the originals do).
    return a == kStpAlpha ? static_cast<uint16_t>(px | 0x8000u) : px;
}

}  // namespace vfs
