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
//                    "drv_size": M, "lba": L, "alt": "B_BG_off...." }, ... ] }
// A "path" ending in ".raw" is the upload's own words (w*h 16-bit units, little endian),
// committed as they are: exact pixel indices or palette entries from another build of the game
// (tools/assets/swap_us_images.py), with no palette conversion. CLUT uploads can be replaced
// this way too (bpp 16, w x h = the CLUT rectangle).
// Only "img", "w", "h", "bpp", "path" are load-bearing; unknown entry keys are
// skipped generically so the ripper can grow provenance without breaking older
// game builds. One manifest per game serial (assets/converted/<id>/); content
// hashes make packs non-portable across serials by construction.
//
// STP convention (shared with vfs/tim.hpp): PNG alpha 0 = texel 0x0000,
// 254 = STP bit set (explicit override); other opaque alphas inherit STP from
// the staged texel for 16-bit art (AI upscalers don't preserve exact alpha).
// Identity packs round-trip bit-for-bit. Editing a PNG while the game runs
// keeps serving the previously fitted art until restart (fit cache).
//
// Save states: the index/mounts are load-time configuration; the CLUT sniffer
// cache, fitted-art cache and counters are runtime state — see save(),
// load_snapshot(), reset_runtime(). Fits are never serialized (they re-derive
// deterministically from PNG + palette).
//
// Future hardware renderer: the manifest/hash/.pak design carries over —
// draw-time replacement keys on the same image hashes; only the VRAM-upload
// hook (maybe_replace) gets replaced by texture-cache injection.

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

    /// Mount the art (`.pak` or folder) and load the manifest: `manifest_path` if given, else
    /// kManifestName inside the art, so one `.pak` can be self-contained. Returns true when at
    /// least one manifest entry loaded; replacement also needs the art to be mounted.
    bool load(const std::string& manifest_path, const std::string& art_path);

    /// Name of the manifest inside an asset folder or `.pak` (used when no manifest path is given).
    static constexpr const char* kManifestName = "assets_manifest.json";

    /// Read another file from the mounted art (e.g. sprites.txt). False when absent.
    bool read_art(const std::string& name, std::vector<uint8_t>& out) const;
    bool enabled() const { return !index_.empty() && vfs_ != nullptr; }
    size_t entry_count() const { return entry_total_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t fit_hits() const { return fit_hits_; }  ///< replacements served from the fit cache
    size_t fit_bytes() const { return fit_bytes_; }
    /// Misses by reason (all subsets of misses_; for coverage diagnostics).
    uint64_t miss_shape() const { return miss_shape_; }  ///< hash hit, TIM dims mispredict the rect
    uint64_t miss_no_palette() const { return miss_no_palette_; }  ///< indexed art without a CLUT entry
    uint64_t miss_palette_not_live() const {
        return miss_palette_not_live_;
    }  ///< palette contents not (yet) sniffed
    uint64_t miss_palette_shape() const {
        return miss_palette_shape_;
    }  ///< multi-row CLUT strips (row tracking not implemented)

    /// Called by Gpu after staging a GP0(A0h) upload, before committing to VRAM.
    /// `staged` holds the raw upload words (ceil(w*h/2)) in VRAM order, low half
    /// first — exactly as write_transfer_word() consumes them.
    /// Returns replacement PSX15/index words on a hit, nullptr on any miss/failure
    /// (caller must then use the original bytes).
    /// `out_w`/`out_h` (optional) receive the rectangle actually written, in 16-bit units: larger
    /// than w x h when the manifest gives the art a bigger slot ("slot_w"/"slot_h").
    const std::vector<uint16_t>* maybe_replace(int x, int y, int w, int h, const uint32_t* staged,
                                               size_t staged_words, int* out_w = nullptr, int* out_h = nullptr);

private:
    struct Candidate {
        std::string path;
        int w = 0, h = 0;  ///< pixel dimensions (PNG fitting target)
        int bpp = 0;       ///< 4, 8 or 16
        uint64_t clut = 0;
        bool has_clut = false;
        std::vector<uint16_t> pal;  ///< the image's own palette (manifest "pal"); empty on old manifests
        int slot_w = 0, slot_h = 0;  ///< larger upload size in pixels (manifest "slot_w"/"slot_h"), 0 = w, h
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
    /// `staged`/`staged_words` are the raw upload words (for 16-bit STP
    /// inheritance, see below); `units` is the w*h word count already validated.
    /// `tw` x `th` is the pixel size to fit the art to (the TIM's, or its larger slot); `units`
    /// the matching upload word count.
    const std::vector<uint16_t>* replace(const Candidate& pick, int tw, int th, size_t units, const uint32_t* staged,
                                         size_t staged_words);

    /// Fitted (decoded + downsampled to TIM size) art cache, keyed by manifest
    /// path. Indexed art additionally keys on the live palette hash, since the
    /// same PNG re-quantizes differently per palette. LRU, bounded by bytes so
    /// a texture-heavy scene cannot grow it without limit.
    struct FitKey {
        std::string path;
        uint64_t clut = 0;  ///< live palette hash, or 0 for 16-bit direct art
        bool operator==(const FitKey& o) const { return path == o.path && clut == o.clut; }
    };
    struct FitKeyHash {
        size_t operator()(const FitKey& k) const {
            size_t h = std::hash<std::string>{}(k.path);
            h ^= std::hash<uint64_t>{}(k.clut) + 0x9E3779B9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct FitEntry {
        std::vector<uint16_t> pixels;  ///< final VRAM-ready words (PSX15 or packed indices)
        std::list<FitKey>::iterator lru;
    };
    static constexpr size_t kMaxFitBytes = 64u << 20;  ///< 64 MiB of fitted art
    std::unordered_map<FitKey, FitEntry, FitKeyHash> fit_cache_;
    std::list<FitKey> fit_lru_;
    size_t fit_bytes_ = 0;
    uint64_t fit_hits_ = 0;  ///< cache hits (subset of hits_)

    const std::vector<uint16_t>* cached_fit(const FitKey& key, size_t units);
    void store_fit(FitKey key, std::vector<uint16_t> pixels);

public:
    /// Save-state isolation: the CLUT/palette sniffer state plus cache stats.
    /// VRAM-affecting state only (index_/vfs_ are load-time configuration).
    /// NOTE: save()/load_snapshot()/reset_runtime() are currently called only
    /// from tests — the parallel save-state work will wire them in (it must
    /// also cover Gpu::UploadSnapshot for mid-upload transfers; see below).
    /// Single-threaded by design (same as the rest of the HLE layer); a future
    /// threaded renderer must serialize maybe_replace/replace.
    struct Snapshot {
        uint64_t last_clut = 0;
        bool have_clut = false;
        std::unordered_map<uint64_t, std::vector<uint16_t>> clut_cache;
        std::vector<uint64_t> clut_lru;  ///< front→back eviction order
        uint64_t hits = 0, misses = 0, fit_hits = 0;
        uint64_t miss_shape = 0, miss_no_palette = 0, miss_palette_not_live = 0, miss_palette_shape = 0;
    };
    /// Capture the runtime state (re-sniffs naturally on load if dropped).
    Snapshot save() const;
    /// Restore it; clears the fitted-art cache (fits re-derive deterministically).
    void load_snapshot(const Snapshot& snap);
    /// Drop all runtime state (CLUT cache, fits, counters) without unloading
    /// the manifest/art mounts. Called on state load when no snapshot exists.
    void reset_runtime();

private:

    std::unique_ptr<vfs::Vfs> vfs_;
    std::vector<uint16_t> scratch_;  ///< replacement pixels
    uint64_t last_clut_ = 0;         ///< palette hash of the most recent CLUT upload
    bool have_clut_ = false;
    size_t entry_total_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    uint64_t miss_shape_ = 0, miss_no_palette_ = 0, miss_palette_not_live_ = 0, miss_palette_shape_ = 0;
    bool logged_missing_art_ = false;
};

}  // namespace hle
