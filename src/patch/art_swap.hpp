#pragma once
// The US art for the JP game: C++ port of tools/assets/swap_us_images.py (its docstring explains
// the pairing; the rules and tables are in art_swap.cpp, which is now their source of truth: the
// Python script is to be retired once nothing offline needs it). build_art (art.cpp) runs plan()
// and packs the result; the helpers are exposed for tests/patch.
//
// plan() takes the two dumps' fs/ folders, indexes their textures the way dcb_asset_ripper does
// (vfs::Ripper), pairs the JP and US images of the same DRV entries by shape, and returns the
// uploads to replace: for each JP upload hash, the US bytes the texture replacer
// (src/hle/gpu/hd_textures.hpp, ".raw" entries) commits instead, with the manifest fields the
// Python run writes for it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace patch::art {

struct Rect {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    auto operator<=>(const Rect&) const = default;
};

/// One TIM as the game uploads it (swap_us_images.Tim): the shape that must match between the
/// two games, and the two payloads.
struct Tim {
    uint32_t offset = 0;  ///< absolute offset in the DRV (or an index)
    int bpp = 0;
    Rect image;           ///< VRAM x, y, w (16-bit units), h
    bool has_clut = false;
    Rect clut;
    std::vector<uint8_t> pixels;   ///< image block payload
    std::vector<uint8_t> palette;  ///< CLUT block payload
};

/// A TIM at `off` in `drv`, or nothing when there is no TIM magic there (swap_us_images.read_tim:
/// the block lengths are trusted, payloads are cut at the end of the data).
std::optional<Tim> read_tim(const std::vector<uint8_t>& drv, size_t off);

/// 4-bit rows laid out for a draw that reads `read` texels onto `shown` pixels (JP_DRAW_SCALE).
std::vector<uint8_t> prewarp_columns(const std::vector<uint8_t>& pixels, int bpp, int width, int read, int shown);
/// 4-bit rows of `width` texels cut to `new_width`: the left part, then the last `right` texels.
std::vector<uint8_t> narrow_columns(const std::vector<uint8_t>& pixels, int bpp, int width, int new_width,
                                    int right);

/// A copy block for compose_image: from the untouched "us" or "jp" image, rect (sx, sy, w, h) in
/// pixels, to (dx, dy).
struct Block {
    bool from_jp = false;
    int sx = 0, sy = 0, w = 0, h = 0, dx = 0, dy = 0;
};
/// The US 4-bit image with `blocks` copied in; returns (pixels, palette) (COMPOSE).
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> compose_image(const Tim& us, const Tim& jp,
                                                                     const std::vector<Block>& blocks);

/// True when `us`'s palette is `jp`'s uploaded in another w x h (the same colours).
bool is_palette_reshape(const Tim& jp, const Tim& us);

/// One upload to replace.
struct Replacement {
    uint64_t key = 0;             ///< the JP upload's hash (manifest "img")
    int w = 0, h = 0, bpp = 0;    ///< manifest fields (pixels; a palette: its CLUT rect, bpp 16)
    std::string us;               ///< where the US data came from ("B.DRV:M_CARD.ARC", "... (palette)")
    std::string alt;              ///< the ripper's name for the JP image
    std::vector<uint8_t> data;    ///< the upload's replacement bytes
};

struct Plan {
    std::vector<Replacement> images;    ///< by key
    std::vector<Replacement> palettes;  ///< by key
    size_t identical = 0;               ///< images the same on both discs
    size_t skipped_entries = 0;         ///< DRV entries left alone (no US entry, layout differs)
};

/// Called between steps with (done, total); return false to stop (plan() then returns nothing).
using StepFn = std::function<bool(uint64_t done, uint64_t total)>;

/// Pair the two dumps' images. `jp_fs` / `us_fs` are the dumps' fs/ folders. Throws
/// std::runtime_error when a DRV cannot be read; std::nullopt when `step` asked to stop.
std::optional<Plan> plan(const std::filesystem::path& jp_fs, const std::filesystem::path& us_fs,
                         const StepFn& step = {});

}  // namespace patch::art
