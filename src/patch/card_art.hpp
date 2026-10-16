#pragma once
// A new card's art (src/patch/cards.hpp, docs/wiki/Cards.md) made from one image:
//   - the card picture B:\CARD\LCnnn.TIM: 64x64, 8-bit, its own 256-colour palette;
//   - the mini card in B:\M_CARD.ARC: 40x40, 8-bit, in its group's shared palette (every card of
//     a group uploads the same palette to the same CLUT row), the level badge in the corner;
//   - the Deck Edit icon (24x24, made at run time from the picture: src/game/cards/).
// The image is the character: a plain background touching its edges (white, as wiki art has) is
// cut out, and the character is set on a dark backdrop in the card's square.

#include "patch/containers.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace patch::card_art {

/// RGBA pixels, row by row.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

/// An 8-bit TIM with a 256-entry palette: where the game uploads the palette and the pixels, and
/// both.
struct Tim8 {
    uint16_t clut_x = 0, clut_y = 0, x = 0, y = 0;
    int w = 0, h = 0;  ///< pixels (2 per VRAM halfword)
    std::array<uint16_t, 256> palette{};
    std::vector<uint8_t> pixels;
};

std::optional<Tim8> read_tim8(View tim);
Bytes write_tim8(const Tim8& tim);

/// The card's square picture (`size` x `size`) from the image: the background flood-filled from
/// the edges (colours within `tolerance` of the corner pixel's) made transparent, the character
/// cropped, fitted (aspect kept, standing on the bottom edge) and set on a dark gradient.
/// nullopt when the image is empty.
std::optional<Image> picture(const Image& src, int size, int tolerance = 40);

/// `img` scaled to w x h (box filter when smaller, nearest when larger).
Image resize(const Image& img, int w, int h);

/// The TIM `like` (its upload positions and size) with `img` in it, quantized to its own palette
/// of up to 256 colours (median cut). No 0x0000 entry (the PS1's transparent colour).
Tim8 own_palette(const Tim8& like, const Image& img);

/// The TIM `like` with `img` in it quantized to `like`'s palette, except where `keep[i]`: there
/// `like`'s own pixel stays (the level badge the group's cards share).
Tim8 shared_palette(const Tim8& like, const Image& img, const std::vector<bool>& keep);

/// What the TIMs (same size) have in common: `like` is the first one with each pixel the value most
/// of them have there, `keep` marks the pixels where at least `percent` % have it. Empty `keep`
/// when there are fewer than two.
struct Consensus {
    Tim8 like;
    std::vector<bool> keep;
};
Consensus common_pixels(const std::vector<Tim8>& tims, int percent = 100);

}  // namespace patch::card_art
