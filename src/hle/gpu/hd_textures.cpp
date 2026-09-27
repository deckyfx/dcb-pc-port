// HD texture replacement. See hd_textures.hpp for the design: hash domain,
// CLUT sniffing, and the fallback contract.

#include "gpu/hd_textures.hpp"

#include "vfs/hash.hpp"

#include <psx/backtrace.hpp>
#include "vfs/image.hpp"
#include "vfs/tim.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hle {

namespace {

/// DCB_LOG_HD=1: log each replacement, and each known texture kept as it was (with the reason).
bool log_hd() {
    static const bool on = std::getenv("DCB_LOG_HD") != nullptr;
    return on;
}

// The ripper writes exactly this shape (whitespace-tolerant, no escapes needed
// since paths are plain ASCII). A hand-rolled reader keeps psx_hle
// dependency-free (nlohmann_json is host-tools-only in this repo's CMake).
struct ManifestEntry {
    uint64_t img = 0;
    uint64_t clut = 0;
    bool has_clut = false;
    int w = 0, h = 0, bpp = 0;
    std::string path;
    std::vector<uint16_t> pal;  ///< the image's own palette ("pal"), if the manifest has it
};

/// "pal": 4 hex digits per entry, at most 256 entries.
bool parse_palette_hex(const std::string& hex, std::vector<uint16_t>& out) {
    if (hex.empty() || hex.size() % 4 != 0 || hex.size() > 4 * 256) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 4) {
        uint16_t v = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = hex[i + k];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = static_cast<uint16_t>(v << 4 | d);
        }
        out.push_back(v);
    }
    return true;
}

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
        if (*p == '\\') {
            // The ripper escapes `"`, `\` and control chars; decode the
            // standard JSON escapes (TOC names may contain `"` and `\`).
            ++p;
            if (p >= end) return false;
            switch (*p) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - p < 5) return false;
                    unsigned cp = 0;
                    for (int i = 1; i <= 4; ++i) {
                        const char c = p[i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                        else return false;
                    }
                    // BMP only (surrogates rejected); encode UTF-8.
                    if (cp == 0 || cp > 0xFFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    p += 4;
                    break;
                }
                default: return false;
            }
            ++p;
        } else {
            out.push_back(*p++);
        }
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
    // Saturate instead of overflowing: values only feed checked ranges.
    unsigned long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        const unsigned d = static_cast<unsigned>(*p++ - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10) {
            v = 0xFFFFFFFFFFFFFFFFull;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            break;
        }
        v = v * 10 + d;
    }
    constexpr unsigned long long kMax = static_cast<unsigned long long>(0x7FFFFFFFFFFFFFFFull);
    out = neg ? (v > kMax + 1 ? -0x7FFFFFFFFFFFFFFFLL - 1 : -static_cast<long long>(v))
              : (v > kMax ? 0x7FFFFFFFFFFFFFFFLL : static_cast<long long>(v));
    return true;
}

/// Skip one JSON value of any shape (object/array nesting included).
bool skip_value(const char*& p, const char* end) {
    p = skip_ws(p, end);
    if (p >= end) return false;
    if (*p == '"') {
        std::string ignored;
        return parse_string(p, end, ignored);
    }
    if (*p == '{' || *p == '[') {
        const char open = *p++, close = open == '{' ? '}' : ']';
        p = skip_ws(p, end);
        if (p < end && *p == close) {
            ++p;
            return true;
        }
        while (true) {
            if (open == '{') {
                std::string ignored;
                if (!parse_string(p, end, ignored) || !expect(p, end, ':')) return false;
            }
            if (!skip_value(p, end)) return false;
            p = skip_ws(p, end);
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == close) {
                ++p;
                return true;
            }
            return false;
        }
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        long long ignored = 0;
        if (!parse_int(p, end, ignored)) return false;
        // Fractions/exponents: consume (provenance only, value dropped).
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        return true;
    }
    for (const char* lit : {"true", "false", "null"}) {
        const size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) >= n && std::memcmp(p, lit, n) == 0) {
            p += n;
            return true;
        }
    }
    return false;
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
        } else if (key == "pal") {
            std::string hex;
            if (!parse_string(p, end, hex) || !parse_palette_hex(hex, e.pal)) return false;
        } else if (key == "w" || key == "h" || key == "bpp") {
            if (!parse_int(p, end, num) || num < 0) return false;
            if (key == "w") e.w = static_cast<int>(num);
            else if (key == "h") e.h = static_cast<int>(num);
            else e.bpp = static_cast<int>(num);
        } else {
            // Provenance for humans/debuggers ("drv", "drv_offset", "drv_size",
            // "lba", "alt", ...) skipped generically — any JSON shape — so the
            // ripper can grow new fields without breaking older game builds.
            if (!skip_value(p, end)) return false;
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
            // Unknown top-level keys skipped like entry provenance: newer
            // manifests stay loadable; only "version" gates compatibility.
            if (!skip_value(p, end)) return false;
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

/// Map one fitted RGBA pixel to a palette index. Exact 16-bit match first
/// (RGB *and* STP, via the same rgba_to_psx15 the PNG round-trips through),
/// so identity art — including transparent 0x0000 wherever it sits, not just
/// at index 0 — returns its original index. Nearest-RGB fallback skips the
/// transparent entry only when it really is 0x0000; STP is compared, not
/// ignored, so duplicate RGB entries keep their bit.
unsigned quantize_index(const std::vector<uint16_t>& pal, size_t per, uint8_t r, uint8_t g, uint8_t b,
                        uint8_t a) {
    const uint16_t want = vfs::rgba_to_psx15(r, g, b, a);
    for (size_t k = 0; k < per; ++k) {
        if (pal[k] == want) return static_cast<unsigned>(k);
    }
    // No exact entry: transparent stays index 0 only if that entry is 0x0000,
    // else nearest opaque color.
    if (a < 128) {
        if (!pal.empty() && pal[0] == 0) return 0;
    }
    // Exact entry for transparent-black-as-zero when it lives elsewhere.
    if (want == 0) {
        for (size_t k = 0; k < per; ++k) {
            if (pal[k] == 0) return static_cast<unsigned>(k);
        }
    }
    // Nearest colour. The STP bit only breaks ties (same distance, e.g. duplicate RGB entries
    // either side of the bit): colour always wins. Art from another source (such as the US
    // release, where every opaque texel has STP set) must not be pulled onto the few STP
    // entries of this palette; exact matches, and so identity packs, never get here.
    unsigned best = 0;
    uint64_t best_d = UINT64_MAX;
    for (size_t k = 0; k < per; ++k) {
        const uint16_t e = pal[k];
        if (e == 0) continue;
        const int dr = static_cast<int>(r) - vfs::expand5(static_cast<uint16_t>(e & 0x1F));
        const int dg = static_cast<int>(g) - vfs::expand5(static_cast<uint16_t>((e >> 5) & 0x1F));
        const int db = static_cast<int>(b) - vfs::expand5(static_cast<uint16_t>((e >> 10) & 0x1F));
        const uint64_t colour = static_cast<uint64_t>(dr * dr + dg * dg + db * db);
        const uint64_t d = colour * 2 + (((e ^ want) & 0x8000u) ? 1u : 0u);
        if (d < best_d) {
            best_d = d;
            best = static_cast<unsigned>(k);
        }
    }
    return best;
}

}  // namespace

HdTextures::HdTextures() = default;
HdTextures::~HdTextures() = default;

bool HdTextures::read_art(const std::string& name, std::vector<uint8_t>& out) const {
    return vfs_ && vfs_->read(name, out);
}

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
    miss_shape_ = miss_no_palette_ = miss_palette_not_live_ = miss_palette_shape_ = 0;

    // Art first: a self-contained `.pak` (or asset folder) may carry the manifest itself.
    if (!art_path.empty()) {
        auto vfs = std::make_unique<vfs::Vfs>();
        if (!vfs->mount(art_path)) {
            std::fprintf(stderr, "[hd] cannot mount art %s\n", art_path.c_str());
        } else {
            std::printf("[hd] mounted %s\n", art_path.c_str());
            vfs_ = std::move(vfs);
        }
    }

    // The manifest: an explicit file, else kManifestName inside the mounted art.
    std::vector<uint8_t> blob;
    std::string source;
    if (!manifest_path.empty()) {
        FILE* f = std::fopen(manifest_path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[hd] cannot open manifest %s\n", manifest_path.c_str());
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long total = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (total > 0 && total < (1l << 30)) {
            blob.resize(static_cast<size_t>(total));
            if (std::fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
        }
        std::fclose(f);
        source = manifest_path;
    } else if (vfs_ && vfs_->read(kManifestName, blob)) {
        source = art_path + ":" + kManifestName;
    } else {
        std::fprintf(stderr, "[hd] no manifest (none given, and no %s in %s)\n", kManifestName, art_path.c_str());
        return false;
    }
    std::vector<ManifestEntry> entries;
    if (blob.empty() || !parse_manifest(blob, entries)) {
        std::fprintf(stderr, "[hd] bad manifest %s\n", source.c_str());
        return false;
    }
    for (auto& e : entries) {
        index_[e.img].push_back({std::move(e.path), e.w, e.h, e.bpp, e.clut, e.has_clut, std::move(e.pal)});
        if (e.has_clut) clut_hashes_.insert(e.clut);
        ++entry_total_;
    }
    std::printf("[hd] manifest %s: %zu entries\n", source.c_str(), entries.size());
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
        // No variant matches the live palette: every variant shares these indices, so any one that
        // carries its own palette converts correctly (artists edit that one; the game still picks
        // the palette at draw time, so colour cycling keeps working).
        if (!pick && shape_ok) {
            for (const auto& c : it->second) {
                int rw = 0, rh = 0;
                if (!c.pal.empty() && upload_rect(c.w, c.h, c.bpp, rw, rh) && rw == w && rh == h) {
                    pick = &c;
                    break;
                }
            }
        }
        if (pick) return replace(*pick, units, staged, staged_words);
        // Shape mismatches are pure coincidences; still sniff CLUTs below so a
        // palette upload with TIM-identical bytes refreshes the cache.
        if (shape_ok == 0) {
            if (clut_hashes_.find(hash) != clut_hashes_.end()) note_clut(hash, staged, words);
            ++miss_shape_;
            ++misses_;
            return nullptr;
        }
        // Right shape, no live palette: keep the original bytes (never guess).
        if (log_hd())
            std::printf("[hd] kept %s: %zu palette variants, none matches the last palette uploaded%s\n",
                        it->second.front().path.c_str(), it->second.size(), psx::backtrace_string(psx::active_context()).c_str());
        ++miss_palette_not_live_;
        ++misses_;
        return nullptr;
    }

    // CLUT sniffing: palette uploads refresh the remembered palette *contents*.
    const bool known_clut = clut_hashes_.find(hash) != clut_hashes_.end();
    if (known_clut) note_clut(hash, staged, words);
    // DCB_TRACE_HD=<n>: log the first n uploads no manifest entry matches (coverage debugging).
    static const unsigned trace = std::getenv("DCB_TRACE_HD") ? static_cast<unsigned>(std::atoi(std::getenv("DCB_TRACE_HD"))) : 0;
    static unsigned traced = 0;
    if (traced < trace) {
        ++traced;
        std::fprintf(stderr, "[hd] no match: %dx%d at (%d,%d) hash %016llx%s\n", w, h, x, y,
                     static_cast<unsigned long long>(hash), known_clut ? " (known palette)" : "");
    }
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
    snap.clut_lru.assign(clut_lru_.begin(), clut_lru_.end());
    snap.hits = hits_;
    snap.misses = misses_;
    snap.fit_hits = fit_hits_;
    snap.miss_shape = miss_shape_;
    snap.miss_no_palette = miss_no_palette_;
    snap.miss_palette_not_live = miss_palette_not_live_;
    snap.miss_palette_shape = miss_palette_shape_;
    return snap;
}

void HdTextures::load_snapshot(const Snapshot& snap) {
    last_clut_ = snap.last_clut;
    have_clut_ = snap.have_clut;
    clut_cache_ = snap.clut_cache;
    // Restore the exact eviction order (front→back); drop hashes with no
    // cached contents (a snapshot edited by hand, not a runtime state).
    clut_lru_.clear();
    for (uint64_t hash : snap.clut_lru) {
        if (clut_cache_.find(hash) != clut_cache_.end()) clut_lru_.push_back(hash);
    }
    // Any cached palette missing from the order still participates (back =
    // evict-first); this only happens for hand-built snapshots.
    for (const auto& [hash, _] : clut_cache_) {
        if (std::find(clut_lru_.begin(), clut_lru_.end(), hash) == clut_lru_.end()) clut_lru_.push_front(hash);
    }
    while (clut_lru_.size() > kMaxCachedCluts) {
        clut_cache_.erase(clut_lru_.front());
        clut_lru_.pop_front();
    }
    hits_ = snap.hits;
    misses_ = snap.misses;
    fit_hits_ = snap.fit_hits;
    miss_shape_ = snap.miss_shape;
    miss_no_palette_ = snap.miss_no_palette;
    miss_palette_not_live_ = snap.miss_palette_not_live;
    miss_palette_shape_ = snap.miss_palette_shape;
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
    miss_shape_ = miss_no_palette_ = miss_palette_not_live_ = miss_palette_shape_ = 0;
}

const std::vector<uint16_t>* HdTextures::replace(const Candidate& pick, size_t units, const uint32_t* staged,
                                                     size_t staged_words) {
    // Indexed art re-quantizes against the live palette: key the fit on it so a
    // palette change refits instead of serving stale indices.
    FitKey key{pick.path, 0};
    const std::vector<uint16_t>* pal_ptr = nullptr;
    size_t per = 0;
    if (pick.bpp != 16) {
        // The palette is identified by content hash (pick.clut), not by upload
        // recency: an image uploaded before its CLUT still resolves once the
        // CLUT has been sniffed at any point. The fit key uses the same hash
        // so each palette gets its own correct indices.
        if (!pick.pal.empty()) {
            // The image's own palette from the disc: independent of upload order, of palettes
            // packed several to an upload, and of palette animation.
            key.clut = pick.has_clut ? pick.clut : vfs::fnv1a64(pick.pal.data(), pick.pal.size() * sizeof(uint16_t));
            pal_ptr = &pick.pal;
            per = pick.pal.size();
        } else {
            // Older manifests without "pal": convert against the palette the game uploaded.
            if (!pick.has_clut) {
                if (log_hd()) std::printf("[hd] kept %s: no palette in the manifest%s\n", pick.path.c_str(), psx::backtrace_string(psx::active_context()).c_str());
                ++miss_no_palette_;
                ++misses_;
                return nullptr;
            }
            key.clut = pick.clut;
            const auto cache = clut_cache_.find(pick.clut);
            if (cache == clut_cache_.end()) {
                if (log_hd())
                    std::printf("[hd] kept %s: its palette has not been uploaded yet (re-rip to store it)%s\n",
                                pick.path.c_str(), psx::backtrace_string(psx::active_context()).c_str());
                ++miss_palette_not_live_;
                ++misses_;
                return nullptr;
            }
            pal_ptr = &cache->second;
            per = pick.bpp == 4 ? 16 : 256;  // entries per palette row
            // Only single-row palette uploads are handled on this path.
            if (pal_ptr->size() != per) {
                if (log_hd())
                    std::printf("[hd] kept %s: palette upload has %zu entries, a %d-bit image uses %zu per row%s\n",
                                pick.path.c_str(), pal_ptr->size(), pick.bpp, per, psx::backtrace_string(psx::active_context()).c_str());
                ++miss_palette_shape_;
                ++misses_;
                return nullptr;
            }
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
        // STP defaults to the staged texel's bit: AI upscalers and 8-bit PNG
        // quantization don't preserve exact alpha 254, so requiring it would
        // silently drop STP on real HD art. Alpha 0 forces transparent;
        // alpha 254 forces STP set (explicit override); any other opaque
        // alpha inherits. Transparent staged texels stay transparent.
        // (A dedicated STP mask image as override is future work.)
        for (size_t i = 0; i < units; ++i) {
            const uint16_t staged_px =
                (staged && i / 2 < staged_words)
                    ? static_cast<uint16_t>(staged[i / 2] >> ((i & 1) * 16))
                    : 0;
            const uint8_t a = fit[i * 4 + 3];
            uint16_t px;
            if (a < 128 || staged_px == 0) {
                px = 0;
            } else if (a == vfs::kStpAlpha) {
                px = static_cast<uint16_t>(vfs::rgba_to_psx15(fit[i * 4], fit[i * 4 + 1], fit[i * 4 + 2], a) |
                                           0x8000u);
            } else {
                px = static_cast<uint16_t>(vfs::rgba_to_psx15(fit[i * 4], fit[i * 4 + 1], fit[i * 4 + 2], 255) |
                                           (staged_px & 0x8000u));
            }
            scratch_[i] = px;
        }
    } else {
        // Re-quantize to the palette chosen above (the image's own, or the uploaded one): same
        // shape the game uploaded (packed indices), sampled from the fitted art.
        const size_t pixels = static_cast<size_t>(pick.w) * static_cast<size_t>(pick.h);
        std::vector<uint8_t> indices(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t r = fit[i * 4], g = fit[i * 4 + 1], b = fit[i * 4 + 2], a = fit[i * 4 + 3];
            // Pass alpha through unchanged: 0 = transparent (exact-match 0x0000
            // below), 254 = STP set, anything else = opaque STP-clear. Inverting
            // either bit (e.g. mapping 255 -> STP) would corrupt palettes that
            // carry the same RGB with and without STP.
            unsigned best = quantize_index(*pal_ptr, per, r, g, b, a);
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
    // Fresh replacements only: repeats come from the fit cache and stay quiet.
    if (log_hd())
        std::printf("[hd] replaced %s (%dx%d, %d-bit, from a %dx%d PNG)%s\n", pick.path.c_str(), pick.w, pick.h,
                    pick.bpp, png_w, png_h, psx::backtrace_string(psx::active_context()).c_str());
    // Cache the fitted result; return the cached copy so the pointer stays
    // valid across later replacements reusing scratch_.
    store_fit(key, scratch_);
    auto it = fit_cache_.find(key);
    return it != fit_cache_.end() ? &it->second.pixels : &scratch_;
}

}  // namespace hle
