// dcb_asset_ripper — offline asset pipeline, step 1: rip, convert, index.
//
//   dcb_asset_ripper unpack <extracted/serial|drv-dir|drv-file> -o <out-dir> [--lba-map manifest.json]
//   dcb_asset_ripper pack <asset-dir> <out.pak>
//   dcb_asset_ripper sfx [raw-dir|file.bin] [-o out] [--game ID]
//   dcb_asset_ripper embed <program> <bundle-dir> <out-program>
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
// `embed` makes the single-file build (vfs/payload.hpp): <program> with every file under
// <bundle-dir> appended as a .pak plus the trailer the game looks for at startup.
//
// Needs the player's own dump; writes nothing copyrighted into the repo.
// Default output roots (overridable with -o) are the gitignored assets/raw and
// assets/converted directories.

#include "vfs/hash.hpp"
#include "vfs/pak.hpp"
#include "vfs/payload.hpp"
#include "vfs/rip.hpp"
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
#include <vector>

namespace fs = std::filesystem;

namespace {

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

using vfs::json_escape;

std::string sanitize(std::string s) { return vfs::sanitize_name(std::move(s)); }

// ---------------------------------------------------------------------------
// DRV walk + manifest: vfs::Ripper (src/vfs/rip.hpp, shared with the English-data builder).
// This file adds the output: payload .bins, PNGs, the manifest file, the log.
// ---------------------------------------------------------------------------

struct Ripper : vfs::Ripper {
    fs::path raw_root;        // discrete payload .bins
    fs::path converted_root;  // PNGs + assets_manifest.json
    std::string game;

    Ripper() {
        on_payload = [this](const std::string& drv, const std::string& stem, const uint8_t* data, size_t size) {
            const fs::path bin_path = raw_root / drv / (stem + ".bin");
            if (write_file(bin_path, data, size)) return true;
            std::fprintf(stderr, "[ripper] cannot write %s\n", bin_path.string().c_str());
            return false;
        };
        on_image = [this](const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
            const fs::path png_path = converted_root / path;
            std::error_code ec;
            fs::create_directories(png_path.parent_path(), ec);  // stbi cannot create dirs
            if (stbi_write_png(png_path.string().c_str(), w, h, 4, rgba.data(), w * 4)) return true;
            std::fprintf(stderr, "[ripper] cannot write %s\n", png_path.string().c_str());
            return false;
        };
        on_log = [](bool error, const std::string& line) {
            std::fprintf(error ? stderr : stdout, "%s\n", line.c_str());
        };
    }
};

bool rip_drv(Ripper& r, const fs::path& drv_path) {
    std::vector<uint8_t> drv;
    if (!read_file(drv_path, drv) || drv.size() < 32) {
        std::fprintf(stderr, "[ripper] cannot read %s\n", drv_path.string().c_str());
        return false;
    }
    return r.rip_drv(drv_path.filename().string(), drv);
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
    const std::string json = r.manifest_json(r.game);
    const fs::path out = r.converted_root / "assets_manifest.json";
    if (!write_file(out, reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
        std::fprintf(stderr, "[ripper] cannot write %s\n", out.string().c_str());
        return;
    }
    std::printf("[ripper] %s: %zu entries, %zu payloads, %zu TIMs, %zu PNGs\n", out.string().c_str(),
                r.manifest().size(), r.payloads, r.tims, r.pngs);
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

int cmd_embed(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: dcb_asset_ripper embed <program> <bundle-dir> <out-program>\n");
        return 1;
    }
    const fs::path program = argv[0], root = argv[1], out = argv[2];
    // Sorted, so the same tree always gives the same archive and content id.
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path rel = fs::relative(e.path(), root, ec);
        if (!ec) files.emplace_back(rel.generic_string(), e.path());
    }
    if (ec || files.empty()) {
        std::fprintf(stderr, "[embed] nothing to bundle in %s\n", root.string().c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());
    vfs::PakWriter pak;
    for (const auto& [name, path] : files) {
        if (!pak.add_file(name, path)) {
            std::fprintf(stderr, "[embed] %s\n", pak.error().c_str());
            return 1;
        }
    }
    std::string error;
    if (!vfs::write_payload_program(out, program, pak, error)) {
        std::fprintf(stderr, "[embed] %s\n", error.c_str());
        return 1;
    }
    fs::permissions(out, fs::status(program, ec).permissions(), ec);  // keep it executable
    std::printf("[embed] %s: %zu files, %llu MB, payload id %s\n", out.string().c_str(), pak.file_count(),
                static_cast<unsigned long long>(pak.byte_size() >> 20), vfs::to_hex16(pak.content_id()).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "unpack") == 0) return cmd_unpack(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "pack") == 0) return cmd_pack(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "sfx") == 0) return cmd_sfx(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "embed") == 0) return cmd_embed(argc - 2, argv + 2);
    std::fprintf(stderr,
                 "dcb_asset_ripper — offline PSX asset pipeline\n"
                 "  unpack <extracted/serial|drv-dir|file.DRV> [-o out] [--lba-map manifest.json]\n"
                 "  pack <asset-dir> <out.pak>\n"
                 "  sfx [raw-dir|file.bin] [-o out] [--game ID]\n"
                 "  embed <program> <bundle-dir> <out-program>\n");
    return 1;
}
