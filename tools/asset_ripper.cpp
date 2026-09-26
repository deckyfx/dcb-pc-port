// dcb_asset_ripper — offline asset pipeline, step 1: rip, convert, index.
//
//   dcb_asset_ripper unpack <extracted/serial|drv-dir|drv-file> -o <out-dir> [--lba-map manifest.json]
//   dcb_asset_ripper pack <asset-dir> <out.pak>
//   dcb_asset_ripper sfx [raw-dir|file.bin] [-o out] [--game ID]
//
// `unpack` reads every *.DRV in the input (plus P.DRV overlays), parses the 32-byte
// container table-of-contents (magic + u32 sector + u32 size + u32 timestamp +
// 16-byte name; payload at sector*2048 — verified against SLPS-03101: \x01BIN,
// \x01PAK, \x01ARC, \x01TIM, \x01MSD, \x01CDD, \x01FNT data entries and \x80 group
// markers whose sector points at a sub-TOC of the same shape), writes each payload
// to a discrete .bin, scans every payload for strictly-validated TIMs, converts
// each (palette, TIM) to RGBA8888 PNG via stb_image_write, and writes
// assets_manifest.json: content hashes (FNV-1a, the runtime HD key) plus DRV/LBA
// provenance (the human/debug key).
//
// `pack` bundles a directory of processed (e.g. AI-upscaled) PNGs into one
// hash-checked .pak for runtime mounting via DCB_HD_PACK.
//
// Needs the player's own dump; writes nothing copyrighted into the repo.
// Default output roots (overridable with -o) are the gitignored assets/raw and
// assets/converted directories.

#include "vfs/hash.hpp"
#include "vfs/pak.hpp"
#include "vfs/tim.hpp"
#include "vfs/toc.hpp"
#include "vfs/vab.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSector = 2048;

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long total = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (total < 0 || total > (1l << 31)) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(total));
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool write_file(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
    const bool closed = std::fclose(f) == 0;
    return ok && closed;
}

std::string sanitize(std::string s) {
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

// ---------------------------------------------------------------------------
// DRV container table (vfs::parse_toc in dcb_vfs; unit-tested there).
// ---------------------------------------------------------------------------

using vfs::TocEntry;

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

struct ManifestEntry {
    uint64_t img = 0;
    uint64_t clut = 0;
    bool has_clut = false;
    int w = 0, h = 0, bpp = 0;
    std::string path;  // VFS-relative, '/'-separated
    std::string drv;
    uint32_t drv_offset = 0;
    uint32_t drv_size = 0;
    uint64_t lba = 0;
    bool has_lba = false;
    std::string alt;  // DRV stem this copy was found under (dedup provenance)
};

struct Ripper {
    fs::path raw_root;        // discrete payload .bins
    fs::path converted_root;  // PNGs + assets_manifest.json
    std::vector<ManifestEntry> manifest;
    std::string game;
    // ISO LBA of each DRV file start (from extracted/<serial>/manifest.json), for provenance.
    std::vector<std::pair<std::string, uint64_t>> drv_lbas;
    // Dedup: FNV-1a of resolved RGBA pixels -> VFS path of the PNG already written.
    // Identical art shared across DRVs rips once; alternates stay as provenance.
    std::unordered_map<uint64_t, std::string> png_by_pixels;

    size_t payloads = 0, tims = 0, pngs = 0, png_reused = 0;

    bool emit_tim_png(const vfs::Tim& tim, const std::string& drv, uint32_t payload_off, size_t tim_off,
                      const std::string& stem);
};

bool Ripper::emit_tim_png(const vfs::Tim& tim, const std::string& drv, uint32_t payload_off, size_t tim_off,
                          const std::string& stem) {
    const int w = tim.pixel_width(), h = tim.pixel_height();
    const unsigned palettes = tim.bpp == 16 ? 1 : static_cast<unsigned>(tim.clut.h);
    if (palettes == 0 || palettes > 512) return false;
    // Image content hash: exactly the bytes the game uploads with GP0(A0h).
    const uint64_t img_hash = vfs::fnv1a64(tim.pixels.data(), tim.pixels.size());
    // Per-row CLUT hashes: the runtime's palette sniffer keys on the uploaded row's
    // content, so multi-palette TIMs need one hash per candidate row, not one for
    // the whole strip.
    std::vector<uint64_t> row_hashes;
    if (tim.has_clut && !tim.clut.entries.empty()) {
        const size_t per = tim.clut.w;
        const size_t rows = tim.clut.entries.size() / per;
        row_hashes.reserve(rows);
        for (size_t row = 0; row < rows; ++row) {
            row_hashes.push_back(
                vfs::fnv1a64(tim.clut.entries.data() + row * per, per * sizeof(uint16_t)));
        }
    }
    for (unsigned pal = 0; pal < palettes; ++pal) {
        std::vector<uint8_t> rgba;
        if (!vfs::tim_to_rgba(tim, pal, rgba)) return false;
        const uint64_t px_hash = vfs::fnv1a64(rgba.data(), rgba.size());
        std::string vfs_path;
        const auto dup = png_by_pixels.find(px_hash);
        if (dup != png_by_pixels.end()) {
            vfs_path = dup->second;  // same art elsewhere: reuse the PNG
            ++png_reused;
        } else {
            char name[128];
            if (palettes == 1) {
                std::snprintf(name, sizeof name, "%s_%dx%d.png", stem.c_str(), w, h);
            } else {
                std::snprintf(name, sizeof name, "%s_%dx%d_pal%u.png", stem.c_str(), w, h, pal);
            }
            const fs::path png_path = converted_root / "textures" / drv / name;
            std::error_code ec;
            fs::create_directories(png_path.parent_path(), ec);  // stbi cannot create dirs
            if (!stbi_write_png(png_path.string().c_str(), w, h, 4, rgba.data(), w * 4)) {
                std::fprintf(stderr, "[ripper] cannot write %s\n", png_path.string().c_str());
                return false;
            }
            vfs_path = std::string("textures/") + drv + "/" + name;
            png_by_pixels.emplace(px_hash, vfs_path);
            ++pngs;
        }
        ManifestEntry e;
        e.img = img_hash;
        // Indexed TIMs upload one palette row per candidate; direct TIMs have none.
        if (tim.has_clut && pal < row_hashes.size()) {
            e.clut = row_hashes[pal];
            e.has_clut = true;
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
        manifest.push_back(std::move(e));
    }
    ++tims;
    return true;
}

void rip_payload(Ripper& r, const std::vector<uint8_t>& drv, uint32_t off, uint32_t size, const std::string& drv_name,
                 const std::string& entry_name) {
    if (off + size < off || off + size > drv.size()) {
        std::fprintf(stderr, "[ripper] %s:%s out of range (off=%u size=%u)\n", drv_name.c_str(), entry_name.c_str(),
                     off, size);
        return;
    }
    const std::string stem = sanitize(drv_name.substr(0, drv_name.find('.')) + "_" + entry_name);
    // 1. Discrete payload file.
    const fs::path bin_path = r.raw_root / drv_name / (stem + ".bin");
    if (!write_file(bin_path, drv.data() + off, size)) {
        std::fprintf(stderr, "[ripper] cannot write %s\n", bin_path.string().c_str());
        return;
    }
    ++r.payloads;

    // 2. Every strictly-validated TIM inside becomes PNGs + manifest entries.
    const uint8_t* base = drv.data() + off;
    vfs::Tim tim;
    for (const auto& [tim_off, used] : vfs::scan_tims(base, size)) {
        if (vfs::parse_tim(base + tim_off, used, tim) == 0) continue;  // cannot happen; be safe
        char name[160];
        std::snprintf(name, sizeof name, "%s_off%08x", stem.c_str(), static_cast<unsigned>(tim_off));
        r.emit_tim_png(tim, drv_name, off, tim_off, name);
    }
}

void rip_toc_level(Ripper& r, const std::vector<uint8_t>& drv, const std::vector<TocEntry>& toc,
                   const std::string& drv_name) {
    size_t groups = 0, payloads = 0;
    for (const TocEntry& t : toc) {
        const uint64_t off64 = static_cast<uint64_t>(t.sector) * kSector;
        if (off64 > drv.size()) {
            std::fprintf(stderr, "[ripper] %s:%s sector %u past end, skipped\n", drv_name.c_str(), t.name.c_str(),
                         t.sector);
            continue;
        }
        const uint32_t off = static_cast<uint32_t>(off64);
        if (t.is_group || t.size == 0) {
            // Sub-TOC of the same 32-byte shape (B.DRV CARD/FONT/..., A.DRV BGM, ...).
            const vfs::TocResult sub = vfs::parse_toc_detailed(drv.data(), drv.size(), off);
            size_t kept = 0;
            for (const TocEntry& s : sub.entries) {
                const uint64_t soff64 = static_cast<uint64_t>(s.sector) * kSector;
                if (soff64 > drv.size()) {
                    std::fprintf(stderr, "[ripper] %s:%s:%s sector %u past end, skipped\n", drv_name.c_str(),
                                 t.name.c_str(), s.name.c_str(), s.sector);
                    continue;
                }
                if (s.is_group) {
                    std::fprintf(stderr, "[ripper] %s:%s:%s nested group, skipped\n", drv_name.c_str(),
                                 t.name.c_str(), s.name.c_str());
                    continue;
                }
                // Same clamp rule as top level: never read past the blob.
                const uint32_t soff = static_cast<uint32_t>(soff64);
                const uint32_t ssize =
                    s.size < drv.size() - soff ? s.size : static_cast<uint32_t>(drv.size() - soff);
                if (ssize == 0) {
                    std::fprintf(stderr, "[ripper] %s:%s:%s empty after clamp, skipped\n", drv_name.c_str(),
                                 t.name.c_str(), s.name.c_str());
                    continue;
                }
                rip_payload(r, drv, soff, ssize, drv_name, t.name + "_" + s.name);
                ++kept;
            }
            std::printf("[ripper] %s:%s: %zu/%zu sub-entries (%s at record %zu)\n", drv_name.c_str(),
                        t.name.c_str(), kept, sub.entries.size(), vfs::toc_stop_name(sub.stop), sub.stop_index);
            ++groups;
            continue;
        }
        const uint32_t size = std::min(t.size, static_cast<uint32_t>(drv.size() - off));
        rip_payload(r, drv, off, size, drv_name, t.name);
        ++payloads;
    }
    if (groups > 0) std::printf("[ripper] %s: %zu payloads + %zu groups\n", drv_name.c_str(), payloads, groups);
}

bool rip_drv(Ripper& r, const fs::path& drv_path) {
    std::vector<uint8_t> drv;
    if (!read_file(drv_path, drv) || drv.size() < 32) {
        std::fprintf(stderr, "[ripper] cannot read %s\n", drv_path.string().c_str());
        return false;
    }
    const std::string drv_name = drv_path.filename().string();
    const vfs::TocResult toc = vfs::parse_toc_detailed(drv.data(), drv.size(), 0);
    if (toc.entries.empty()) {
        // No container table (e.g. MMM.DAT, SLPS_031.01): still scan for TIMs.
        std::fprintf(stderr, "[ripper] %s: no TOC, raw TIM scan\n", drv_name.c_str());
        rip_payload(r, drv, 0, static_cast<uint32_t>(drv.size()), drv_name, "raw");
        return true;
    }
    std::printf("[ripper] %s: %zu TOC entries (%s at record %zu)\n", drv_name.c_str(), toc.entries.size(),
                vfs::toc_stop_name(toc.stop), toc.stop_index);
    rip_toc_level(r, drv, toc.entries, drv_name);
    return true;
}

// Minimal reader for extracted/<serial>/manifest.json: we only need
// files[] -> {path, lba} for DRV provenance. Hand-rolled (no json dep in tools).
void load_lba_map(Ripper& r, const fs::path& manifest_path) {
    std::vector<uint8_t> blob;
    if (!read_file(manifest_path, blob) || blob.empty()) return;
    const std::string text(blob.begin(), blob.end());
    // Find each {"path": "...", "lba": N, ...} object by scanning for "path" then "lba".
    size_t pos = 0;
    while (true) {
        const size_t pk = text.find("\"path\"", pos);
        if (pk == std::string::npos) break;
        const size_t q1 = text.find('"', pk + 6);
        if (q1 == std::string::npos) break;
        const size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const size_t lk = text.find("\"lba\"", q2);
        if (lk == std::string::npos || lk - q2 > 200) {
            pos = q2 + 1;
            continue;
        }
        const size_t colon = text.find(':', lk + 5);
        if (colon == std::string::npos) break;
        const uint64_t lba = std::strtoull(text.c_str() + colon + 1, nullptr, 10);
        r.drv_lbas.emplace_back(text.substr(q1 + 1, q2 - q1 - 1), lba);
        pos = colon + 1;
    }
    std::printf("[ripper] LBA map: %zu files\n", r.drv_lbas.size());
}

void write_manifest(const Ripper& r) {
    std::string json = "{\"version\":1,\"game\":";
    json_escape(json, r.game);
    json += ",\"entries\":[";
    bool first = true;
    for (const ManifestEntry& e : r.manifest) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"img\":\"" + vfs::to_hex16(e.img) + "\",\"w\":" + std::to_string(e.w) +
                ",\"h\":" + std::to_string(e.h) + ",\"bpp\":" + std::to_string(e.bpp) + ",\"path\":";
        json_escape(json, e.path);
        if (e.has_clut) json += ",\"clut\":\"" + vfs::to_hex16(e.clut) + "\"";
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
    const fs::path out = r.converted_root / "assets_manifest.json";
    if (!write_file(out, reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
        std::fprintf(stderr, "[ripper] cannot write %s\n", out.string().c_str());
        return;
    }
    std::printf("[ripper] %s: %zu entries, %zu payloads, %zu TIMs, %zu PNGs\n", out.string().c_str(),
                r.manifest.size(), r.payloads, r.tims, r.pngs);
}

int cmd_unpack(int argc, char** argv) {
    fs::path input;
    fs::path out = "assets";  // project folder; -> assets/raw/<game> + assets/converted/<game>
    fs::path lba_map;
    std::string game;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "--lba-map" && i + 1 < argc) lba_map = argv[++i];
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (!a.empty() && a[0] != '-') input = a;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 1;
        }
    }
    if (input.empty()) {
        std::fprintf(stderr,
                     "usage: dcb_asset_ripper unpack <extracted/serial|drv-dir|file.DRV> [-o out] [--game ID] "
                     "[--lba-map manifest.json]\n");
        return 1;
    }
    Ripper r;
    std::error_code ec;
    std::vector<fs::path> drvs;
    if (fs::is_directory(input, ec)) {
        // extracted/<serial>/: DRVs live in fs/, LBA map is manifest.json.
        const fs::path fsdir = fs::is_directory(input / "fs", ec) ? input / "fs" : input;
        if (lba_map.empty() && fs::is_regular_file(input / "manifest.json", ec)) lba_map = input / "manifest.json";
        if (game.empty()) game = input.filename().string();
        for (const auto& e : fs::directory_iterator(fsdir, ec)) {
            std::string ext = e.path().extension().string();
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (e.is_regular_file() && ext == ".drv") drvs.push_back(e.path());
        }
    } else if (fs::is_regular_file(input, ec)) {
        drvs.push_back(input);
    } else {
        std::fprintf(stderr, "no such input %s\n", input.string().c_str());
        return 1;
    }
    if (game.empty()) game = "unknown";
    r.game = sanitize(game);
    // Project folder layout (all gitignored): assets/raw/<game>/, assets/converted/<game>/.
    // Start from a clean tree: stale PNGs from earlier rips (e.g. pre-dedup
    // names) would otherwise accumulate unused next to the fresh output.
    r.raw_root = out / "raw" / r.game;
    r.converted_root = out / "converted" / r.game;
    fs::remove_all(r.converted_root, ec);
    fs::create_directories(r.converted_root / "textures", ec);
    if (!lba_map.empty()) load_lba_map(r, lba_map);
    std::sort(drvs.begin(), drvs.end());
    bool ok = !drvs.empty();
    if (drvs.empty()) std::fprintf(stderr, "no .DRV files in %s\n", input.string().c_str());
    for (const auto& d : drvs) ok = rip_drv(r, d) && ok;
    write_manifest(r);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// SFX: VAB banks -> per-bank .vab copies + tone-table manifest (sfx_manifest.json).
// A VAB holds the game's instrument/sample bank. The header + tone table parse
// is verified byte-for-byte (see vfs/vab.hpp), but the vag->wave-area mapping
// is NOT yet proven — several placements validate partially and none satisfies
// header counts + flag walks + vag landmarks jointly. So this command extracts
// what is proven (whole-bank copies + per-tone rows) and leaves WAV rendering
// to `decode_vag` once the wave base is nailed down. SEQ music data (pQES
// bundles in the same payloads) is copied verbatim alongside for external
// tooling; the manifest records both.
// ---------------------------------------------------------------------------

struct SfxBank {
    std::string path;  // VFS-relative .vab copy
    std::string drv;
    uint32_t drv_offset = 0;
    uint32_t size = 0;
    uint32_t bank_id = 0;
    uint16_t programs = 0, tones = 0, vags = 0;
};

struct SfxTone {
    size_t bank = 0;  // index into banks
    unsigned program = 0, tone = 0, vag = 0;
    uint8_t vol = 0, pan = 0, center = 0, shift = 0, minimum = 0, maximum = 0;
};

struct SfxSeq {
    std::string path;  // VFS-relative .seq copy
    std::string drv;
    uint32_t drv_offset = 0;
    uint32_t size = 0;
};

void rip_sfx_bank(std::vector<SfxBank>& banks, std::vector<SfxTone>& tones, std::vector<SfxSeq>& seqs,
                  const fs::path& sfx_root, const std::string& drv, uint32_t base_off, const uint8_t* base,
                  size_t size, const std::string& stem) {
    // SEQ music: pQES bundles (Sony sequence + embedded bank) copied verbatim.
    // A bundle starts with u16 id, u16 n, u32 size, then "pQES".
    for (size_t off = 0; off + 12 <= size;) {
        const uint8_t* p = base + off;
        const bool is_seq = p[8] == 'p' && p[9] == 'Q' && p[10] == 'E' && p[11] == 'S';
        uint32_t bundle = 0;
        std::memcpy(&bundle, p + 4, 4);
        if (!is_seq || bundle < 12 || bundle > size - off) {
            ++off;
            continue;
        }
        char name[128];
        std::snprintf(name, sizeof name, "%s_seq%04zx.seq", stem.c_str(), off);
        const fs::path seq_path = sfx_root / "sfx" / drv / name;
        std::error_code ec;
        fs::create_directories(seq_path.parent_path(), ec);
        if (write_file(seq_path, p, bundle)) {
            SfxSeq s;
            s.path = std::string("sfx/") + drv + "/" + name;
            s.drv = drv;
            s.drv_offset = base_off + static_cast<uint32_t>(off);
            s.size = bundle;
            seqs.push_back(std::move(s));
        }
        off += bundle;
    }
    // VAB banks: whole-bank copies + tone rows.
    vfs::Vab vab;
    for (const auto& [vab_off, used] : vfs::scan_vabs(base, size)) {
        if (vfs::parse_vab(base + vab_off, used, vab) == 0) continue;  // cannot happen; be safe
        char name[128];
        std::snprintf(name, sizeof name, "%s_bank%04zx.vab", stem.c_str(), vab_off);
        const fs::path vab_path = sfx_root / "sfx" / drv / name;
        std::error_code ec;
        fs::create_directories(vab_path.parent_path(), ec);
        if (!write_file(vab_path, base + vab_off, used)) {
            std::fprintf(stderr, "[sfx] cannot write %s\n", vab_path.string().c_str());
            continue;
        }
        SfxBank b;
        b.path = std::string("sfx/") + drv + "/" + name;
        b.drv = drv;
        b.drv_offset = base_off + static_cast<uint32_t>(vab_off);
        b.size = static_cast<uint32_t>(used);
        b.bank_id = vab.bank_id;
        b.programs = vab.programs;
        b.tones = vab.tones;
        b.vags = vab.vags;
        const size_t bank_index = banks.size();
        banks.push_back(std::move(b));
        for (const vfs::VabTone& tone : vab.active_tones) {
            SfxTone t;
            t.bank = bank_index;
            t.program = tone.program;
            t.tone = tone.index;
            t.vag = tone.vag;
            t.vol = tone.vol;
            t.pan = tone.pan;
            t.center = tone.center;
            t.shift = tone.shift;
            t.minimum = tone.minimum;
            t.maximum = tone.maximum;
            tones.push_back(t);
        }
    }
}

int cmd_sfx(int argc, char** argv) {
    fs::path input = "assets/raw";  // project folder default
    fs::path out = "assets";        // -> assets/converted/<game>/sfx + sfx_manifest.json
    std::string game;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (!a.empty() && a[0] != '-') input = a;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 1;
        }
    }
    if (game.empty()) game = "SLPS-03101";
    game = sanitize(game);
    // Collect .bin payloads: either a raw/<game> tree or a single file.
    std::vector<std::pair<std::string, fs::path>> bins;  // (drv, path)
    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        const fs::path root = fs::is_directory(input / game, ec) ? input / game : input;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".bin") continue;
            // DRV name = parent dir (A.DRV/...) or "raw" for flat inputs.
            std::string drv = e.path().parent_path().filename().string();
            if (drv.empty() || drv == root.filename()) drv = "raw";
            bins.emplace_back(drv, e.path());
        }
    } else if (fs::is_regular_file(input, ec)) {
        bins.emplace_back("raw", input);
    } else {
        std::fprintf(stderr, "no such input %s\n", input.string().c_str());
        return 1;
    }
    std::sort(bins.begin(), bins.end());
    const fs::path sfx_root = out / "converted" / game;
    // Clean tree, like unpack: stale .vab/.seq from earlier rips must not linger.
    fs::remove_all(sfx_root / "sfx", ec);
    fs::create_directories(sfx_root / "sfx", ec);
    std::vector<SfxBank> banks;
    std::vector<SfxTone> tones;
    std::vector<SfxSeq> seqs;
    for (const auto& [drv, path] : bins) {
        std::vector<uint8_t> blob;
        if (!read_file(path, blob) || blob.empty()) continue;
        const std::string stem = sanitize(path.stem().string());
        rip_sfx_bank(banks, tones, seqs, sfx_root, drv, 0, blob.data(), blob.size(), stem);
    }
    std::string json = "{\"version\":1,\"game\":";
    json_escape(json, game);
    json += ",\"banks\":[";
    bool first = true;
    for (const SfxBank& b : banks) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"path\":";
        json_escape(json, b.path);
        json += ",\"drv\":";
        json_escape(json, b.drv);
        json += ",\"drv_offset\":" + std::to_string(b.drv_offset) + ",\"size\":" + std::to_string(b.size) +
                ",\"bank_id\":" + std::to_string(b.bank_id) + ",\"programs\":" + std::to_string(b.programs) +
                ",\"tones\":" + std::to_string(b.tones) + ",\"vags\":" + std::to_string(b.vags) + "}";
    }
    json += "],\"tones\":[";
    first = true;
    for (const SfxTone& t : tones) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"bank\":" + std::to_string(t.bank) + ",\"program\":" + std::to_string(t.program) +
                ",\"tone\":" + std::to_string(t.tone) + ",\"vag\":" + std::to_string(t.vag) +
                ",\"vol\":" + std::to_string(t.vol) + ",\"pan\":" + std::to_string(t.pan) +
                ",\"center\":" + std::to_string(t.center) + ",\"shift\":" + std::to_string(t.shift) +
                ",\"minimum\":" + std::to_string(t.minimum) + ",\"maximum\":" + std::to_string(t.maximum) + "}";
    }
    json += "],\"seq\":[";
    first = true;
    for (const SfxSeq& s : seqs) {
        if (!first) json.push_back(',');
        first = false;
        json += "{\"path\":";
        json_escape(json, s.path);
        json += ",\"drv\":";
        json_escape(json, s.drv);
        json += ",\"drv_offset\":" + std::to_string(s.drv_offset) + ",\"size\":" + std::to_string(s.size) + "}";
    }
    json += "]}\n";
    const fs::path mpath = sfx_root / "sfx_manifest.json";
    if (!write_file(mpath, reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
        std::fprintf(stderr, "[sfx] cannot write %s\n", mpath.string().c_str());
        return 1;
    }
    std::printf("[sfx] %s: %zu banks, %zu tones, %zu seq\n", mpath.string().c_str(), banks.size(), tones.size(),
                seqs.size());
    return banks.empty() ? 1 : 0;
}

int cmd_pack(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dcb_asset_ripper pack <asset-dir> <out.pak>\n");
        return 1;
    }
    const fs::path root = argv[0];
    vfs::PakWriter pak;
    std::error_code ec;
    size_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path rel = fs::relative(e.path(), root, ec);
        if (ec) continue;
        std::string name = rel.generic_string();
        std::vector<uint8_t> data;
        if (!read_file(e.path(), data)) {
            std::fprintf(stderr, "[pack] cannot read %s\n", e.path().string().c_str());
            return 1;
        }
        if (!pak.add(std::move(name), data)) {
            std::fprintf(stderr, "[pack] %s\n", pak.error().c_str());
            return 1;
        }
        ++n;
    }
    if (!pak.write(argv[1])) {
        std::fprintf(stderr, "[pack] cannot write %s\n", argv[1]);
        return 1;
    }
    std::printf("[pack] %s: %zu files\n", argv[1], n);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "unpack") == 0) return cmd_unpack(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "pack") == 0) return cmd_pack(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "sfx") == 0) return cmd_sfx(argc - 2, argv + 2);
    std::fprintf(stderr,
                 "dcb_asset_ripper — offline PSX asset pipeline\n"
                 "  unpack <extracted/serial|drv-dir|file.DRV> [-o out] [--lba-map manifest.json]\n"
                 "  pack <asset-dir> <out.pak>\n"
                 "  sfx [raw-dir|file.bin] [-o out] [--game ID]\n");
    return 1;
}
