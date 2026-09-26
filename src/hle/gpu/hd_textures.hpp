#pragma once
// HD texture replacement: hashes each GP0(A0h) VRAM upload, looks the hash up in
// the manifest index, and — on a hit — substitutes the upscaled PNG (downsampled
// to the TIM's pixel size) so the game renders HD art while running unmodified.
//
// Why here: the engine keeps a single 1024x512 VRAM image with no per-asset
// SDL_Texture path (src/platform/sdl3.cpp presents the whole display rect), so
// the only place an HD asset can enter the frame is the CPU->VRAM upload.
// Gpu stages the words, then calls HdTextures::maybe_replace() before committing
// them to VRAM. A miss costs one hash-table lookup per upload; the fallback is
// the original bytes, untouched.
//
// Hash domain (must match tools/asset_ripper byte-for-byte):
//   * 16-bit direct TIMs: FNV-1a over the image block bytes (img_w*img_h*2 LE bytes).
//   * 4/8-bit indexed TIMs: FNV-1a over the packed index block bytes (same size).
//     The game uploads the packed block verbatim (rect img_w x img_h words) and the
//     CLUT separately, so replacement re-quantizes fitted HD pixels to the palette
//     the game actually uploaded (nearest RGB; alpha<128 -> transparent entry).
//   * CLUT uploads: FNV-1a over the cw*ch entry bytes. Every upload whose hash is
//     a known CLUT hash refreshes the palette-content cache (last upload wins);
//     multi-palette TIMs pick the candidate whose CLUT matches the cached one.
//
// Manifest (`assets_manifest.json`, written by dcb_asset_ripper):
//   { "version": 1, "game": "SLPS-03101",
//     "entries": [ { "img": "<16 hex>", "w": <pixel width>, "h": <height>,
//                    "bpp": 4|8|16, "path": "textures/....png",
//                    "clut": "<16 hex>", "drv": "B.DRV", "drv_offset": N,
//                    "drv_size": M, "lba": L }, ... ] }
// Only "img", "w", "h", "bpp", "path" are load-bearing; the rest is provenance.

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vfs {
class Vfs;
}

namespace hle {

class HdTextures {
public:
    HdTextures();
    ~HdTextures();  // defined in hd_textures.cpp where vfs::Vfs is complete

    /// Load the manifest and mount the HD pack/dir. Both arguments are optional:
    /// a manifest without art (or art without a manifest) simply never replaces.
    /// Returns true when at least one manifest entry loaded.
    bool load(const std::string& manifest_path, const std::string& art_path);

    bool enabled() const { return !index_.empty() && vfs_ != nullptr; }
    size_t entry_count() const { return entry_total_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

    /// Called by Gpu after staging a GP0(A0h) upload, before committing to VRAM.
    /// `staged` holds the raw upload words (ceil(w*h/2)) in VRAM order, low half
    /// first — exactly as write_transfer_word() consumes them.
    /// Returns replacement PSX15/index words on a hit, nullptr on any miss/failure
    /// (caller must then use the original bytes).
    const std::vector<uint16_t>* maybe_replace(int x, int y, int w, int h, const uint32_t* staged,
                                               size_t staged_words);

private:
    struct Candidate {
        std::string path;
        int w = 0, h = 0;  ///< pixel dimensions (PNG fitting target)
        int bpp = 0;       ///< 4, 8 or 16
        uint64_t clut = 0;
        bool has_clut = false;
    };

    /// VRAM rect (in 16-bit units) a TIM of these pixel dims occupies when the
    /// game uploads the block verbatim — the only uploads we replace.
    static bool upload_rect(int pixel_w, int pixel_h, int bpp, int& rw, int& rh);

    std::unordered_map<uint64_t, std::vector<Candidate>> index_;
    std::unordered_set<uint64_t> clut_hashes_;  ///< every known palette hash, for O(1) CLUT sniffing
    // Palette contents by hash, from sniffed uploads (bounded; games use few live palettes).
    std::unordered_map<uint64_t, std::vector<uint16_t>> clut_cache_;
    std::list<uint64_t> clut_lru_;
    static constexpr size_t kMaxCachedCluts = 64;

    void note_clut(uint64_t hash, const uint32_t* staged, size_t staged_words);
    /// Substitute PNG art for an image hit; nullptr on any failure (miss counted).
    const std::vector<uint16_t>* replace(const Candidate& pick, size_t units);

    std::unique_ptr<vfs::Vfs> vfs_;
    std::vector<uint16_t> scratch_;  ///< replacement pixels
    uint64_t last_clut_ = 0;         ///< palette hash of the most recent CLUT upload
    bool have_clut_ = false;
    size_t entry_total_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    bool logged_missing_art_ = false;
};

}  // namespace hle
