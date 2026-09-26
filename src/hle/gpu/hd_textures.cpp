// HD texture replacement. See hd_textures.hpp for the design: hash domain,
// CLUT sniffing, and the fallback contract.

#include "gpu/hd_textures.hpp"

#include "vfs/hash.hpp"
#include "vfs/image.hpp"
#include "vfs/tim.hpp"
#include "vfs/vfs.hpp"

#include <cstdio>
#include <cstring>

namespace hle {

namespace {

// The ripper writes exactly this shape (whitespace-tolerant, no escapes needed
// since paths are plain ASCII). A hand-rolled reader keeps psx_hle
// dependency-free (nlohmann_json is host-tools-only in this repo's CMake).
struct ManifestEntry {
    uint64_t img = 0;
    uint64_t clut = 0;
    bool has_clut = false;
    int w = 0, h = 0, bpp = 0;
    std::string path;
};

const char* skip_ws(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

bool expect(const char*& p, const char* end, char ch) {
    p = skip_ws(p, end);
    if (p >= end || *p != ch) return false;
    return ++p, true;
}

bool parse_string(const char*& p, const char* end, std::string& out) {
    p = skip_ws(p, end);
    if (p >= end || *p != '"') return false;
    ++p;
    out.clear();
    while (p < end && *p != '"') {
        if (*p == '\\') return false;  // the ripper never emits escapes
        out.push_back(*p++);
    }
    if (p >= end) return false;
    return ++p, true;
}

bool parse_int(const char*& p, const char* end, long long& out) {
    p = skip_ws(p, end);
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        ++p;
    }
    if (p >= end || *p < '0' || *p > '9') return false;
    long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    out = neg ? -v : v;
    return true;
}

bool parse_entry(const char*& p, const char* end, ManifestEntry& e) {
    if (!expect(p, end, '{')) return false;
    e = ManifestEntry{};
    while (true) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') {
            ++p;
            return !e.path.empty() && e.w > 0 && e.h > 0 && (e.bpp == 4 || e.bpp == 8 || e.bpp == 16);
        }
        std::string key, str;
        long long num = 0;
        if (!parse_string(p, end, key) || !expect(p, end, ':')) return false;
        if (key == "img" || key == "clut") {
            if (!parse_string(p, end, str)) return false;
            uint64_t h = 0;
            if (!vfs::from_hex16(str, h)) return false;
            if (key == "img") e.img = h;
            else {
                e.clut = h;
                e.has_clut = true;
            }
        } else if (key == "path") {
            if (!parse_string(p, end, e.path)) return false;
        } else if (key == "w" || key == "h" || key == "bpp") {
            if (!parse_int(p, end, num) || num < 0) return false;
            if (key == "w") e.w = static_cast<int>(num);
            else if (key == "h") e.h = static_cast<int>(num);
            else e.bpp = static_cast<int>(num);
        } else {
            // Provenance for humans/debuggers ("drv", "drv_offset", "drv_size",
            // "lba", "alt", ...) skipped generically so the ripper can grow
            // new fields without breaking older game builds. Values are
            // string/int only — anything else is a schema error.
            p = skip_ws(p, end);
            if (p < end && *p == '"') {
                if (!parse_string(p, end, str)) return false;
            } else {
                if (!parse_int(p, end, num)) return false;
            }
        }
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            ++p;
            continue;
        }
        if (p < end && *p == '}') continue;  // loop head consumes it
        return false;
    }
}

bool parse_manifest(const std::vector<uint8_t>& blob, std::vector<ManifestEntry>& out) {
    out.clear();
    const char* p = reinterpret_cast<const char*>(blob.data());
    const char* end = p + blob.size();
    // {"version":1,...,"entries":[{...},{...}]}
    if (!expect(p, end, '{')) return false;
    while (true) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') return true;  // trailing "}"
        std::string key, str;
        long long num = 0;
        if (!parse_string(p, end, key) || !expect(p, end, ':')) return false;
        if (key == "entries") {
            if (!expect(p, end, '[')) return false;
            p = skip_ws(p, end);
            if (p < end && *p == ']') {
                ++p;
            } else {
                while (true) {
                    ManifestEntry e;
                    if (!parse_entry(p, end, e)) return false;
                    out.push_back(std::move(e));
                    p = skip_ws(p, end);
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (!expect(p, end, ']')) return false;
                    break;
                }
            }
        } else if (key == "version") {
            if (!parse_int(p, end, num) || num != 1) return false;
        } else if (key == "game") {
            if (!parse_string(p, end, str)) return false;  // informational only
        } else {
            return false;  // top-level shape is fixed; entries carry provenance
        }
        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') {
            ++p;  // final "}": only trailing whitespace may follow
            return skip_ws(p, end) == end;
        }
        return false;
    }
}

}  // namespace

HdTextures::HdTextures() = default;
HdTextures::~HdTextures() = default;

bool HdTextures::load(const std::string& manifest_path, const std::string& art_path) {
    index_.clear();
    clut_hashes_.clear();
    clut_cache_.clear();
    clut_lru_.clear();
    fit_cache_.clear();
    fit_lru_.clear();
    fit_bytes_ = 0;
    vfs_.reset();
    have_clut_ = false;
    entry_total_ = 0;
    hits_ = misses_ = fit_hits_ = 0;

    if (!manifest_path.empty()) {
        FILE* f = std::fopen(manifest_path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[hd] cannot open manifest %s\n", manifest_path.c_str());
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long total = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> blob;
        if (total > 0 && total < (1l << 30)) {
            blob.resize(static_cast<size_t>(total));
            if (std::fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
        }
        std::fclose(f);
        std::vector<ManifestEntry> entries;
        if (blob.empty() || !parse_manifest(blob, entries)) {
            std::fprintf(stderr, "[hd] bad manifest %s\n", manifest_path.c_str());
            return false;
        }
        for (auto& e : entries) {
            index_[e.img].push_back({std::move(e.path), e.w, e.h, e.bpp, e.clut, e.has_clut});
            if (e.has_clut) clut_hashes_.insert(e.clut);
            ++entry_total_;
        }
        std::printf("[hd] manifest %s: %zu entries\n", manifest_path.c_str(), entries.size());
    }

    if (!art_path.empty()) {
        auto vfs = std::make_unique<vfs::Vfs>();
        if (!vfs->mount(art_path)) {
            std::fprintf(stderr, "[hd] cannot mount art %s\n", art_path.c_str());
            if (index_.empty()) return false;
        } else {
            std::printf("[hd] mounted %s\n", art_path.c_str());
        }
        vfs_ = std::move(vfs);
    }
    return !index_.empty();
}

bool HdTextures::upload_rect(int pixel_w, int pixel_h, int bpp, int& rw, int& rh) {
    // The game uploads a TIM image block verbatim into an rw x rh word rect.
    switch (bpp) {
        case 16: rw = pixel_w; rh = pixel_h; return true;
        case 8:  // two pixels per word
            if (pixel_w & 1) return false;
            rw = pixel_w / 2;
            rh = pixel_h;
            return true;
        case 4:  // four pixels per word
            if (pixel_w & 3) return false;
            rw = pixel_w / 4;
            rh = pixel_h;
            return true;
        default: return false;
    }
}

void HdTextures::note_clut(uint64_t hash, const uint32_t* staged, size_t staged_words) {
    // Reconstruct the palette bytes (low half first) and cache them.
    const size_t bytes = staged_words * 4;
    if (bytes == 0 || bytes > 4096 || (bytes & 1)) return;
    std::vector<uint16_t> entries(bytes / 2);
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t word = staged[i / 2];
        entries[i] = static_cast<uint16_t>(word >> ((i & 1) * 16));
    }
    if (vfs::fnv1a64(entries.data(), bytes) != hash) return;  // odd-size padding: ignore
    clut_cache_[hash] = std::move(entries);
    clut_lru_.remove(hash);
    clut_lru_.push_back(hash);
    while (clut_lru_.size() > kMaxCachedCluts) {
        clut_cache_.erase(clut_lru_.front());
        clut_lru_.pop_front();
    }
    last_clut_ = hash;
    have_clut_ = true;
}

const std::vector<uint16_t>* HdTextures::maybe_replace(int x, int y, int w, int h, const uint32_t* staged,
                                                       size_t staged_words) {
    (void)x;
    (void)y;
    if (!enabled() || !staged || w <= 0 || h <= 0 || w > 1024 || h > 512) {
        ++misses_;
        return nullptr;
    }
    // Hash the consumed upload bytes (low half first per word). Each word holds
    // two 16-bit units; for odd word counts the final half is padding and is
    // excluded — byte-identical to what the ripper hashes. (TIMS are word-sized
    // in practice; this is belt-and-braces against corrupt headers.)
    const size_t units = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t words = (units + 1) / 2;
    if (staged_words < words) {  // short transfer: never touch it
        ++misses_;
        return nullptr;
    }
    uint64_t hash = vfs::kFnvOffsetBasis;
    for (size_t i = 0; i < units; ++i) {
        const uint16_t unit = static_cast<uint16_t>(staged[i / 2] >> ((i & 1) * 16));
        hash ^= static_cast<uint64_t>(unit & 0xFFu);
        hash *= vfs::kFnvPrime;
        hash ^= static_cast<uint64_t>(unit >> 8);
        hash *= vfs::kFnvPrime;
    }

    auto it = index_.find(hash);
    if (it != index_.end()) {
        // Image hit wins over CLUT sniffing (a CLUT-sized upload that is also a
        // TIM image must replace, not just refresh the palette cache).
        const Candidate* pick = nullptr;
        size_t shape_ok = 0;
        for (const auto& c : it->second) {
            int rw = 0, rh = 0;
            // The TIM's pixel dims must predict exactly this VRAM rect (packed for
            // indexed modes); anything else is a same-bytes coincidence — skip it.
            if (!upload_rect(c.w, c.h, c.bpp, rw, rh) || rw != w || rh != h) continue;
            ++shape_ok;
            if (it->second.size() == 1) {
                pick = &c;
                break;
            }
            // Multi-palette TIM: only the candidate whose CLUT matches the live one.
            // No guessing: an unknown palette falls back to the original bytes.
            if (c.has_clut && have_clut_ && c.clut == last_clut_) {
                pick = &c;
                break;
            }
        }
        if (pick) return replace(*pick, units);
        // Shape mismatches are pure coincidences; still sniff CLUTs below so a
        // palette upload with TIM-identical bytes refreshes the cache.
        if (shape_ok == 0) {
            if (clut_hashes_.find(hash) != clut_hashes_.end()) note_clut(hash, staged, words);
            ++misses_;
            return nullptr;
        }
        // Right shape, no live palette: keep the original bytes (never guess).
        ++misses_;
        return nullptr;
    }

    // CLUT sniffing: palette uploads refresh the remembered palette *contents*.
    if (clut_hashes_.find(hash) != clut_hashes_.end()) note_clut(hash, staged, words);
    ++misses_;
    return nullptr;
}

const std::vector<uint16_t>* HdTextures::cached_fit(const FitKey& key, size_t units) {
    auto it = fit_cache_.find(key);
    if (it == fit_cache_.end()) return nullptr;
    if (it->second.pixels.size() != units) return nullptr;  // shape changed: refit
    fit_lru_.splice(fit_lru_.end(), fit_lru_, it->second.lru);
    ++fit_hits_;
    ++hits_;
    return &it->second.pixels;
}

void HdTextures::store_fit(FitKey key, std::vector<uint16_t> pixels) {
    fit_bytes_ += pixels.size() * sizeof(uint16_t);
    fit_lru_.push_back(key);
    FitEntry entry{std::move(pixels), std::prev(fit_lru_.end())};
    // Replace-in-place keeps the LRU position of an existing key.
    auto it = fit_cache_.find(key);
    if (it != fit_cache_.end()) {
        fit_bytes_ -= it->second.pixels.size() * sizeof(uint16_t);
        fit_lru_.erase(it->second.lru);
        it->second = std::move(entry);
    } else {
        fit_cache_.emplace(std::move(key), std::move(entry));
    }
    while (fit_bytes_ > kMaxFitBytes && !fit_lru_.empty()) {
        const FitKey& old = fit_lru_.front();
        auto oit = fit_cache_.find(old);
        if (oit != fit_cache_.end()) {
            fit_bytes_ -= oit->second.pixels.size() * sizeof(uint16_t);
            fit_cache_.erase(oit);
        }
        fit_lru_.pop_front();
    }
}

HdTextures::Snapshot HdTextures::save() const {
    Snapshot snap;
    snap.last_clut = last_clut_;
    snap.have_clut = have_clut_;
    snap.clut_cache = clut_cache_;
    snap.hits = hits_;
    snap.misses = misses_;
    snap.fit_hits = fit_hits_;
    return snap;
}

void HdTextures::load_snapshot(const Snapshot& snap) {
    last_clut_ = snap.last_clut;
    have_clut_ = snap.have_clut;
    clut_cache_ = snap.clut_cache;
    clut_lru_.clear();
    for (const auto& [hash, _] : clut_cache_) {
        clut_lru_.push_back(hash);
        if (clut_lru_.size() >= kMaxCachedCluts) break;
    }
    hits_ = snap.hits;
    misses_ = snap.misses;
    fit_hits_ = snap.fit_hits;
    fit_cache_.clear();  // fits re-derive deterministically from palette + PNG
    fit_lru_.clear();
    fit_bytes_ = 0;
}

void HdTextures::reset_runtime() {
    last_clut_ = 0;
    have_clut_ = false;
    clut_cache_.clear();
    clut_lru_.clear();
    fit_cache_.clear();
    fit_lru_.clear();
    fit_bytes_ = 0;
    hits_ = misses_ = fit_hits_ = 0;
}

const std::vector<uint16_t>* HdTextures::replace(const Candidate& pick, size_t units) {
    // Indexed art re-quantizes against the live palette: key the fit on it so a
    // palette change refits instead of serving stale indices.
    FitKey key{pick.path, 0};
    const std::vector<uint16_t>* pal_ptr = nullptr;
    size_t per = 0;
    if (pick.bpp != 16) {
        if (!pick.has_clut || !have_clut_) {
            ++misses_;  // no live palette: keep the original bytes (never guess)
            return nullptr;
        }
        key.clut = last_clut_;
        const auto cache = clut_cache_.find(pick.clut);
        if (cache == clut_cache_.end() || cache->first != last_clut_) {
            ++misses_;
            return nullptr;
        }
        pal_ptr = &cache->second;
        per = pick.bpp == 4 ? 16 : 256;  // entries per palette row
        // NOTE: only single-row palettes are handled (see below).
        if (pal_ptr->size() != per) {
            ++misses_;
            return nullptr;
        }
    }
    if (const std::vector<uint16_t>* hit = cached_fit(key, units)) return hit;

    int png_w = 0, png_h = 0;
    std::vector<uint8_t> rgba;
    if (!vfs::load_rgba(*vfs_, pick.path, png_w, png_h, rgba)) {
        if (!logged_missing_art_) {
            logged_missing_art_ = true;
            std::fprintf(stderr, "[hd] art missing for %s (further misses silent)\n", pick.path.c_str());
        }
        ++misses_;
        return nullptr;
    }
    std::vector<uint8_t> fit;
    if (!vfs::downsample_rgba(rgba, png_w, png_h, pick.w, pick.h, fit)) {
        ++misses_;  // PNG smaller than the TIM: refuse to upscale, keep original
        return nullptr;
    }
    scratch_.resize(units);
    if (pick.bpp == 16) {
        for (size_t i = 0; i < units; ++i) {
            scratch_[i] = vfs::rgba_to_psx15(fit[i * 4], fit[i * 4 + 1], fit[i * 4 + 2], fit[i * 4 + 3]);
        }
    } else {
        // Re-quantize to the live palette row (hoisted above): same shape the game
        // uploaded (packed indices), but sampled from fitted HD pixels.
        // NOTE: only single-row palettes (clut.w == 16/256, one row) are handled:
        // multi-row strips share one upload but select rows per-primitive via the
        // CLUT id, which we don't track yet. Those fall back until row tracking lands.
        const size_t pixels = static_cast<size_t>(pick.w) * static_cast<size_t>(pick.h);
        std::vector<uint8_t> indices(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t r = fit[i * 4], g = fit[i * 4 + 1], b = fit[i * 4 + 2], a = fit[i * 4 + 3];
            unsigned best = 0;
            if (a >= 128) {
                // Exact 16-bit palette match first (RGB *and* STP): identity art
                // round-trips bit-for-bit, and artist edits that reuse exact
                // palette colors keep their STP. Nearest RGB only as fallback.
                const uint16_t want =
                    vfs::rgba_to_psx15(r, g, b, a >= 254 ? vfs::kStpAlpha : 255);
                const std::vector<uint16_t>& pal = *pal_ptr;
                bool exact = false;
                for (size_t k = 0; k < per; ++k) {
                    if (pal[k] == want) {
                        best = static_cast<unsigned>(k);
                        exact = true;
                        break;
                    }
                }
                if (!exact) {
                    // Nearest palette RGB; index 0 conventionally transparent.
                    unsigned best_d = 0xFFFFFFFFu;
                    for (size_t k = 0; k < per; ++k) {
                        const uint16_t e = pal[k];
                        if (e == 0) continue;
                        const int dr = static_cast<int>(r) - vfs::expand5(static_cast<uint16_t>(e & 0x1F));
                        const int dg = static_cast<int>(g) - vfs::expand5(static_cast<uint16_t>((e >> 5) & 0x1F));
                        const int db = static_cast<int>(b) - vfs::expand5(static_cast<uint16_t>((e >> 10) & 0x1F));
                        const unsigned d =
                            static_cast<unsigned>(dr * dr + dg * dg + db * db);
                        if (d < best_d) {
                            best_d = d;
                            best = static_cast<unsigned>(k);
                        }
                    }
                    // If the best match is worse than pure black-is-zero... keep it:
                    // entry 0 is transparent so a black HD pixel maps to the nearest
                    // non-zero dark entry, same as the artist's TIM would.
                }
            }
            indices[i] = static_cast<uint8_t>(best);
        }
        // Re-pack indices into words in VRAM order (low unit first).
        for (size_t i = 0; i < units; ++i) {
            uint16_t unit = 0;
            if (pick.bpp == 8) {
                unit = static_cast<uint16_t>(indices[i * 2] | (indices[i * 2 + 1] << 8));
            } else {
                for (int k = 0; k < 4; ++k) unit |= static_cast<uint16_t>(indices[i * 4 + k] << (4 * k));
            }
            scratch_[i] = unit;
        }
    }
    ++hits_;
    // Cache the fitted result; return the cached copy so the pointer stays
    // valid across later replacements reusing scratch_.
    store_fit(key, scratch_);
    auto it = fit_cache_.find(key);
    return it != fit_cache_.end() ? &it->second.pixels : &scratch_;
}

}  // namespace hle
