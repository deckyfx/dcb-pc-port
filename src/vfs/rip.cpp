// The asset ripper's DRV walk and texture index. See rip.hpp.

#include "vfs/rip.hpp"

#include "vfs/hash.hpp"
#include "vfs/tim.hpp"
#include "vfs/toc.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace vfs {

namespace {
constexpr uint32_t kSector = 2048;
}  // namespace

std::string sanitize_name(std::string s) {
    for (char& ch : s) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == '-' || ch == '.';
        if (!ok) ch = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

void json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char ch : s) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (ch >= 0x20) {
            out.push_back(ch);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", ch);
            out += buf;
        }
    }
    out.push_back('"');
}

void Ripper::log(bool error, const char* fmt, ...) {
    if (!on_log) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    on_log(error, buf);
}

bool Ripper::emit_tim(const Tim& tim, const std::string& drv, uint32_t payload_off, size_t tim_off,
                      const std::string& stem) {
    const int w = tim.pixel_width(), h = tim.pixel_height();
    const unsigned palettes = tim.bpp == 16 ? 1 : static_cast<unsigned>(tim.clut.h);
    if (palettes == 0 || palettes > 512) return false;
    // Image content hash: exactly the bytes the game uploads with GP0(A0h).
    const uint64_t img_hash = fnv1a64(tim.pixels.data(), tim.pixels.size());
    // Per-row CLUT hashes: the runtime's palette sniffer keys on the uploaded row's
    // content, so multi-palette TIMs need one hash per candidate row, not one for
    // the whole strip.
    std::vector<uint64_t> row_hashes;
    if (tim.has_clut && !tim.clut.entries.empty()) {
        const size_t per = tim.clut.w;
        const size_t rows = tim.clut.entries.size() / per;
        row_hashes.reserve(rows);
        for (size_t row = 0; row < rows; ++row) {
            row_hashes.push_back(fnv1a64(tim.clut.entries.data() + row * per, per * sizeof(uint16_t)));
        }
    }
    for (unsigned pal = 0; pal < palettes; ++pal) {
        std::vector<uint8_t> rgba;
        if (!tim_to_rgba(tim, pal, rgba)) return false;
        const uint64_t px_hash = fnv1a64(rgba.data(), rgba.size());
        std::string vfs_path;
        const auto dup = png_by_pixels_.find(px_hash);
        if (dup != png_by_pixels_.end()) {
            vfs_path = dup->second;  // same art elsewhere: reuse the PNG
            ++png_reused;
        } else {
            char name[128];
            if (palettes == 1) {
                std::snprintf(name, sizeof name, "%s_%dx%d.png", stem.c_str(), w, h);
            } else {
                std::snprintf(name, sizeof name, "%s_%dx%d_pal%u.png", stem.c_str(), w, h, pal);
            }
            vfs_path = std::string("textures/") + drv + "/" + name;
            if (on_image && !on_image(vfs_path, w, h, rgba)) return false;
            png_by_pixels_.emplace(px_hash, vfs_path);
            ++pngs;
        }
        RipEntry e;
        e.img = img_hash;
        // Indexed TIMs upload one palette row per candidate; direct TIMs have none.
        if (tim.has_clut && pal < row_hashes.size()) {
            e.clut = row_hashes[pal];
            e.has_clut = true;
            // The palette itself (the entries an index can reach), so the runtime can convert art
            // against the image's own colours instead of whatever palette was uploaded last.
            const size_t row = static_cast<size_t>(pal) * tim.clut.w;
            const size_t reach = std::min<size_t>(tim.clut.w, tim.bpp == 4 ? 16 : 256);
            if (row + reach <= tim.clut.entries.size())
                e.pal.assign(tim.clut.entries.begin() + static_cast<std::ptrdiff_t>(row),
                             tim.clut.entries.begin() + static_cast<std::ptrdiff_t>(row + reach));
        }
        e.w = w;
        e.h = h;
        e.bpp = tim.bpp;
        e.path = vfs_path;
        e.drv = drv;
        e.drv_offset = payload_off + static_cast<uint32_t>(tim_off);
        e.drv_size = static_cast<uint32_t>(tim.pixels.size());
        e.alt = stem;  // the name this copy was found under (dedup provenance)
        for (const auto& [file, lba] : drv_lbas) {
            if (file == drv) {
                e.lba = lba + payload_off / kSector;  // ISO LBA of the containing sector
                e.has_lba = true;
                break;
            }
        }
        manifest_.push_back(std::move(e));
    }
    ++tims;
    return true;
}

void Ripper::rip_payload(const std::vector<uint8_t>& drv, uint32_t off, uint32_t size, const std::string& drv_name,
                         const std::string& entry_name) {
    if (off + size < off || off + size > drv.size()) {
        log(true, "[ripper] %s:%s out of range (off=%u size=%u)", drv_name.c_str(), entry_name.c_str(), off, size);
        return;
    }
    const std::string stem = sanitize_name(drv_name.substr(0, drv_name.find('.')) + "_" + entry_name);
    // 1. Discrete payload file.
    if (on_payload && !on_payload(drv_name, stem, drv.data() + off, size)) return;
    ++payloads;

    // 2. Every strictly-validated TIM inside becomes images + manifest entries.
    const uint8_t* base = drv.data() + off;
    Tim tim;
    for (const auto& [tim_off, used] : scan_tims(base, size)) {
        if (parse_tim(base + tim_off, used, tim) == 0) continue;  // cannot happen; be safe
        char name[160];
        std::snprintf(name, sizeof name, "%s_off%08x", stem.c_str(), static_cast<unsigned>(tim_off));
        emit_tim(tim, drv_name, off, tim_off, name);
    }
}

void Ripper::rip_toc_level(const std::vector<uint8_t>& drv, const std::vector<TocEntry>& toc,
                           const std::string& drv_name) {
    size_t groups = 0, direct = 0;
    for (const TocEntry& t : toc) {
        const uint64_t off64 = static_cast<uint64_t>(t.sector) * kSector;
        if (off64 > drv.size()) {
            log(true, "[ripper] %s:%s sector %u past end, skipped", drv_name.c_str(), t.name.c_str(), t.sector);
            continue;
        }
        const uint32_t off = static_cast<uint32_t>(off64);
        if (t.is_group || t.size == 0) {
            // Sub-TOC of the same 32-byte shape (B.DRV CARD/FONT/..., A.DRV BGM, ...).
            const TocResult sub = parse_toc_detailed(drv.data(), drv.size(), off);
            size_t kept = 0;
            for (const TocEntry& s : sub.entries) {
                const uint64_t soff64 = static_cast<uint64_t>(s.sector) * kSector;
                if (soff64 > drv.size()) {
                    log(true, "[ripper] %s:%s:%s sector %u past end, skipped", drv_name.c_str(), t.name.c_str(),
                        s.name.c_str(), s.sector);
                    continue;
                }
                if (s.is_group) {
                    log(true, "[ripper] %s:%s:%s nested group, skipped", drv_name.c_str(), t.name.c_str(),
                        s.name.c_str());
                    continue;
                }
                // Same clamp rule as top level: never read past the blob.
                const uint32_t soff = static_cast<uint32_t>(soff64);
                const uint32_t ssize = s.size < drv.size() - soff ? s.size : static_cast<uint32_t>(drv.size() - soff);
                if (ssize == 0) {
                    log(true, "[ripper] %s:%s:%s empty after clamp, skipped", drv_name.c_str(), t.name.c_str(),
                        s.name.c_str());
                    continue;
                }
                rip_payload(drv, soff, ssize, drv_name, t.name + "_" + s.name);
                ++kept;
            }
            log(false, "[ripper] %s:%s: %zu/%zu sub-entries (%s at record %zu)", drv_name.c_str(), t.name.c_str(),
                kept, sub.entries.size(), toc_stop_name(sub.stop), sub.stop_index);
            ++groups;
            continue;
        }
        const uint32_t size = std::min(t.size, static_cast<uint32_t>(drv.size() - off));
        rip_payload(drv, off, size, drv_name, t.name);
        ++direct;
    }
    if (groups > 0) log(false, "[ripper] %s: %zu payloads + %zu groups", drv_name.c_str(), direct, groups);
}

bool Ripper::rip_drv(const std::string& drv_name, const std::vector<uint8_t>& drv) {
    if (drv.size() < 32) return false;
    const TocResult toc = parse_toc_detailed(drv.data(), drv.size(), 0);
    if (toc.entries.empty()) {
        // No container table (e.g. MMM.DAT, SLPS_031.01): still scan for TIMs.
        log(true, "[ripper] %s: no TOC, raw TIM scan", drv_name.c_str());
        rip_payload(drv, 0, static_cast<uint32_t>(drv.size()), drv_name, "raw");
        return true;
    }
    log(false, "[ripper] %s: %zu TOC entries (%s at record %zu)", drv_name.c_str(), toc.entries.size(),
        toc_stop_name(toc.stop), toc.stop_index);
    rip_toc_level(drv, toc.entries, drv_name);
    return true;
}

std::string Ripper::manifest_json(const std::string& game) const {
    std::string json = "{\"version\":1,\"game\":";
    json_escape(json, game);
    json += ",\"entries\":[";
    bool first = true;
    for (const RipEntry& e : manifest_) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"img\":\"" + to_hex16(e.img) + "\",\"w\":" + std::to_string(e.w) + ",\"h\":" + std::to_string(e.h) +
                ",\"bpp\":" + std::to_string(e.bpp) + ",\"path\":";
        json_escape(json, e.path);
        if (e.has_clut) json += ",\"clut\":\"" + to_hex16(e.clut) + "\"";
        if (!e.pal.empty()) {  // 4 hex digits per 15-bit entry, in palette order
            json += ",\"pal\":\"";
            char hex[5];
            for (const uint16_t v : e.pal) {
                std::snprintf(hex, sizeof hex, "%04x", v);
                json += hex;
            }
            json += "\"";
        }
        json += ",\"drv\":";
        json_escape(json, e.drv);
        json += ",\"drv_offset\":" + std::to_string(e.drv_offset) + ",\"drv_size\":" + std::to_string(e.drv_size);
        if (e.has_lba) json += ",\"lba\":" + std::to_string(e.lba);
        if (!e.alt.empty()) {
            json += ",\"alt\":";
            json_escape(json, e.alt);
        }
        json += "}";
    }
    json += "]}\n";
    return json;
}

}  // namespace vfs
