// Native replacements for the game's file API (config/SLPS-03101/overrides.json), so game data is
// read straight from the game files instead of through the CD drive model.
//
// The game opens files by path, "X:\DIR\NAME.EXT": X selects the archive X.DRV in the disc's root,
// then each component is looked up in that archive's table of contents (vfs::parse_toc: \x80
// groups are directories, \x01EXT records are files). The original routines (Ghidra):
//   80015C24 open(path, mode)          fills one of four 0x1030-byte handles at 800842F0
//   80016080 read(handle, bytes, dest) CdRead + CdReadSync, yielding a frame while it waits
//   80015FF8 close(handle)
//   80015994 chdir(path)               caches a directory's table for relative paths
// The native ones keep the handle fields callers read (+0x00 in use, +0x24 size) and answer at
// once. Bytes come from assets/<serial>/files/<X>/<DIR>/<NAME.EXT> when that loose file exists
// (any size: modded files need not fit the disc), else from the archive entry, read through
// hle::Disc (which honours assets/<serial>/disc/ overrides) without touching the drive.
//
// DCB_CD_FILES=1 keeps the original CD path (for comparison); DCB_LOG_FILES=1 logs every open.

#include "cdrom/disc.hpp"
#include "cdrom/load_log.hpp"
#include "vfs/toc.hpp"

#include "native_files.hpp"

#include <psx/backtrace.hpp>
#include <psx/recomp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kHandles = 0x800842F0u, kHandleSize = 0x1030u, kHandleCount = 4;
constexpr uint32_t kCwdLoaded = 0x800883B0u;  // set once a directory is cached (relative paths ok)
constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kV0 = 2;

// Handle fields (words), as the original routines use them.
constexpr uint32_t kInUse = 0x00, kSector = 0x1C, kRemaining = 0x20, kSize = 0x24, kBuffered = 0x28;
// Native bookkeeping in the (unused by us) read-ahead buffer area: source id and read offset.
constexpr uint32_t kSource = 0x30, kOffset = 0x34;

hle::Disc* g_disc = nullptr;
std::string g_serial;

bool use_cd() {
    static const bool cd = std::getenv("DCB_CD_FILES") != nullptr;
    return cd || g_disc == nullptr;
}
bool log_files() {
    static const bool on = std::getenv("DCB_LOG_FILES") != nullptr;
    return on;
}

/// One opened file: either an archive entry (disc sector + size) or a loose file's bytes.
/// Sources are append-only for the whole run, so an id stored in guest RAM stays valid across
/// save states (which are in-process only).
struct Source {
    uint32_t lba = 0;             ///< archive entry: first disc sector
    uint32_t size = 0;
    std::vector<uint8_t> bytes;   ///< loose file contents (empty for archive entries)
    bool loose = false;
};
std::vector<Source> g_sources;
std::map<std::string, uint32_t> g_source_ids;  ///< resolved path -> id

/// A table of contents: where it is on disc and its records.
struct Dir {
    uint32_t lba = 0;             ///< archive start sector (entries' sectors are relative to it)
    std::vector<vfs::TocEntry> entries;
    std::string drive_path;       ///< "B", "B/FONT": for loose-file lookups
};
std::map<std::pair<uint32_t, uint32_t>, Dir> g_dirs;  ///< (archive lba, table sector) -> table
bool g_have_cwd = false;
Dir g_cwd;

bool read_sectors(uint32_t lba, uint32_t count, std::vector<uint8_t>& out) {
    uint8_t raw[hle::Disc::kRawSector];
    for (uint32_t i = 0; i < count; ++i) {
        if (!g_disc->read(lba + i, raw)) return false;
        out.insert(out.end(), raw + 24, raw + 24 + 2048);  // Mode 2 Form 1 user data
    }
    return true;
}

/// The table of contents at `table_sector` inside the archive starting at `archive_lba`.
const Dir* load_dir(uint32_t archive_lba, uint32_t table_sector, const std::string& drive_path) {
    const auto key = std::make_pair(archive_lba, table_sector);
    if (const auto it = g_dirs.find(key); it != g_dirs.end()) return &it->second;
    std::vector<uint8_t> blob;
    // Tables are small; read on until the terminator shows up (at most 64 KB).
    for (uint32_t n = 0; n < 32; ++n) {
        if (!read_sectors(archive_lba + table_sector + n, 1, blob)) break;
        const vfs::TocResult r = vfs::parse_toc_detailed(blob.data(), blob.size());
        if (r.stop != vfs::TocStop::EndOfBlob) {
            Dir d{archive_lba, r.entries, drive_path};
            return &g_dirs.emplace(key, std::move(d)).first->second;
        }
    }
    return nullptr;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) parts.push_back(upper(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(upper(cur));
    return parts;
}

/// Record matching `name` ("CARD2.CDD" -> name CARD2 with magic \x01CDD; "FONT" -> \x80 group).
const vfs::TocEntry* find(const Dir& dir, const std::string& part, bool want_dir) {
    const size_t dot = part.find('.');
    const std::string name = part.substr(0, dot), ext = dot == std::string::npos ? "" : part.substr(dot + 1);
    for (const vfs::TocEntry& e : dir.entries) {
        if (upper(e.name) != name || e.is_group != want_dir) continue;
        if (!want_dir && !ext.empty() && upper(e.magic.substr(1)) != ext.substr(0, 3)) continue;
        return &e;
    }
    return nullptr;
}

/// Resolve a game path to a source id; false if it does not exist.
bool resolve(const std::string& path, bool want_dir, uint32_t& id, Dir* dir_out) {
    Dir dir;
    std::vector<std::string> parts;
    if (path.size() >= 2 && path[1] == ':') {
        const std::string drive(1, static_cast<char>(std::toupper(static_cast<unsigned char>(path[0]))));
        uint32_t lba = 0, size = 0;
        if (!g_disc->find_root_file(drive + ".DRV", lba, size)) return false;
        const Dir* root = load_dir(lba, 0, drive);
        if (!root) return false;
        dir = *root;
        parts = split(path.substr(2));
    } else {
        if (!g_have_cwd) return false;
        dir = g_cwd;
        parts = split(path);
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        const vfs::TocEntry* e = find(dir, parts[i], !last || want_dir);
        if (!e) return false;
        if (!last || want_dir) {
            const Dir* sub = load_dir(dir.lba, e->sector, dir.drive_path + "/" + parts[i]);
            if (!sub) return false;
            dir = *sub;
            continue;
        }
        const std::string key = dir.drive_path + "/" + parts[i];
        if (const auto it = g_source_ids.find(key); it != g_source_ids.end()) {
            id = it->second;
            return true;
        }
        Source src;
        const fs::path loose = fs::path("assets") / g_serial / "files" / key;
        std::error_code ec;
        if (fs::is_regular_file(loose, ec)) {
            std::ifstream in(loose, std::ios::binary);
            src.bytes.assign(std::istreambuf_iterator<char>(in), {});
            src.size = static_cast<uint32_t>(src.bytes.size());
            src.loose = true;
        } else {
            src.lba = dir.lba + e->sector;
            src.size = e->size;
        }
        id = static_cast<uint32_t>(g_sources.size());
        g_sources.push_back(std::move(src));
        g_source_ids.emplace(key, id);
        if (log_files())
            std::printf("[file] %s: %u bytes from %s\n", path.c_str(), g_sources[id].size,
                        g_sources[id].loose ? loose.string().c_str() : "the game data");
        return true;
    }
    if (want_dir && dir_out) *dir_out = dir;
    return want_dir;
}

std::string guest_string(PsxContext& ctx, uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 256; ++i) {
        const char c = static_cast<char>(psx_read8(&ctx, addr + i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

uint32_t field(PsxContext& ctx, uint32_t handle, uint32_t off) { return psx_read32(&ctx, handle + off); }
void set_field(PsxContext& ctx, uint32_t handle, uint32_t off, uint32_t v) { psx_write32(&ctx, handle + off, v); }

}  // namespace

namespace dcb {

void attach_native_files(hle::Disc* disc, const std::string& serial) {
    g_disc = disc;
    g_serial = serial;
    if (!use_cd()) std::printf("[file] game data is read directly (DCB_CD_FILES=1 reads it through the CD drive)\n");
}

}  // namespace dcb

extern "C" {

// 80015C24: open(path, mode) -> handle (0 if missing). mode 0 opens a directory's table.
void dcb_file_open(PsxContext* ctx) {
    if (use_cd()) return psx_call_original(ctx, 0x80015C24u);
    const std::string path = guest_string(*ctx, ctx->r[kA0]);
    const uint32_t mode = ctx->r[kA1];
    ctx->r[kV0] = 0;
    uint32_t handle = 0;
    for (uint32_t i = 0; i < kHandleCount; ++i) {
        const uint32_t h = kHandles + i * kHandleSize;
        if (field(*ctx, h, kInUse) == 0) {
            handle = h;
            break;
        }
    }
    if (!handle) return;
    uint32_t id = 0;
    if (!resolve(path, mode == 0, id, nullptr) || mode == 0) {
        if (mode != 0 && log_files()) std::printf("[file] %s: not found\n", path.c_str());
        return;  // directories are opened through chdir (80015994), which is native too
    }
    const Source& src = g_sources[id];
    hle::LoadLog::instance().note_file(path);
    if (hle::LoadLog::instance().enabled())  // who asked for it: the guest call chain
        hle::LoadLog::instance().file(path, src.size, src.loose, psx::backtrace_string(ctx));
    set_field(*ctx, handle, kInUse, mode);
    set_field(*ctx, handle, kSector, src.lba);
    set_field(*ctx, handle, kRemaining, src.size);
    set_field(*ctx, handle, kSize, src.size);
    set_field(*ctx, handle, kBuffered, 0);
    set_field(*ctx, handle, kSource, id);
    set_field(*ctx, handle, kOffset, 0);
    ctx->r[kV0] = handle;
}

// 80016080: read(handle, bytes, dest) -> bytes read.
void dcb_file_read(PsxContext* ctx) {
    if (use_cd()) return psx_call_original(ctx, 0x80016080u);
    const uint32_t handle = ctx->r[kA0], dest = ctx->r[kA2];
    const uint32_t id = field(*ctx, handle, kSource), offset = field(*ctx, handle, kOffset);
    const uint32_t remaining = field(*ctx, handle, kRemaining);
    const uint32_t want = std::min(ctx->r[kA1], remaining);
    ctx->r[kV0] = 0;
    if (id >= g_sources.size() || want == 0) return;
    const Source& src = g_sources[id];
    std::vector<uint8_t> bytes;
    if (src.loose) {
        bytes.assign(src.bytes.begin() + offset, src.bytes.begin() + offset + want);
    } else {
        const uint32_t first = offset / 2048, last = (offset + want + 2047) / 2048;
        if (!read_sectors(src.lba + first, last - first, bytes)) return;
        bytes.erase(bytes.begin(), bytes.begin() + (offset % 2048));
        bytes.resize(want);
    }
    const int32_t at = psx_ram_offset(dest);
    if (at >= 0 && static_cast<size_t>(at) + want <= PSX_RAM_SIZE) {
        std::memcpy(ctx->ram + at, bytes.data(), want);
    } else {
        for (uint32_t i = 0; i < want; ++i) psx_write8(ctx, dest + i, bytes[i]);
    }
    set_field(*ctx, handle, kOffset, offset + want);
    set_field(*ctx, handle, kRemaining, remaining - want);
    ctx->r[kV0] = want;
}

// 80015FF8: close(handle).
void dcb_file_close(PsxContext* ctx) {
    if (use_cd()) return psx_call_original(ctx, 0x80015FF8u);
    if (ctx->r[kA0]) set_field(*ctx, ctx->r[kA0], kInUse, 0);
}

// 80059D30: libcd CdSearchFile(CdlFILE* fp, const char* name) -> fp, or 0 if absent.
// CdlFILE: CdlLOC pos (BCD minute, second, sector, track), u32 size, char name[16].
void dcb_cd_search_file(PsxContext* ctx) {
    if (use_cd()) return psx_call_original(ctx, 0x80059D30u);
    const uint32_t fp = ctx->r[kA0];
    std::string name = guest_string(*ctx, ctx->r[kA1]);
    while (!name.empty() && (name.front() == '\\' || name.front() == '/')) name.erase(0, 1);
    const std::string plain = upper(name.substr(0, name.find(';')));
    uint32_t lba = 0, size = 0;
    ctx->r[kV0] = 0;
    if (plain.find('\\') != std::string::npos || !g_disc->find_root_file(plain, lba, size)) return;  // root only
    const uint32_t abs = lba + 150;
    const auto bcd = [](uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); };
    psx_write8(ctx, fp + 0, bcd(abs / 4500));
    psx_write8(ctx, fp + 1, bcd(abs / 75 % 60));
    psx_write8(ctx, fp + 2, bcd(abs % 75));
    psx_write8(ctx, fp + 3, 0);
    psx_write32(ctx, fp + 4, size);
    for (uint32_t i = 0; i < 16; ++i) psx_write8(ctx, fp + 8 + i, i < name.size() ? static_cast<uint8_t>(name[i]) : 0);
    if (log_files()) std::printf("[file] CdSearchFile %s: sector %u, %u bytes\n", name.c_str(), lba, size);
    ctx->r[kV0] = fp;
}

// 80015994: chdir(path) -> 0 on success, 1 if the directory does not exist.
void dcb_file_chdir(PsxContext* ctx) {
    if (use_cd()) return psx_call_original(ctx, 0x80015994u);
    const std::string path = guest_string(*ctx, ctx->r[kA0]);
    uint32_t unused = 0;
    Dir dir;
    if (!resolve(path, true, unused, &dir)) {
        ctx->r[kV0] = 1;
        return;
    }
    g_cwd = dir;
    g_have_cwd = true;
    psx_write32(ctx, kCwdLoaded, 1);
    ctx->r[kV0] = 0;
}

}  // extern "C"
