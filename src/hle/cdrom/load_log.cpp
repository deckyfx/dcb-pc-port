// Named asset-load log. See load_log.hpp for the format and the guest-neutral
// contract. Sector runs coalesce so a 500-sector file read prints once, with
// the DRV entry name resolved lazily (TOC parse on first touch per file).

#include "cdrom/load_log.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// vfs::parse_toc lives in dcb_vfs, which psx_hle already links.
#include "vfs/toc.hpp"

namespace hle {

namespace {

// layout.txt line shapes (see tools/disc/extract_disc.py write_layout()):
//   sectors N
//   meta <lba> <count> <index>
//   file <lba> <sectors> <bytes> <form1|raw2352> <first_sh> <last_sh> <path>
bool parse_file_line(const std::string& line, LoadRange& out) {
    if (line.compare(0, 5, "file ") != 0) return false;
    std::istringstream in(line.substr(5));
    uint32_t lba = 0, sectors = 0, bytes = 0;
    std::string storage, first_sh, last_sh, path;
    if (!(in >> lba >> sectors >> bytes >> storage >> first_sh >> last_sh)) return false;
    std::getline(in >> std::ws, path);
    if (path.empty()) return false;
    out.lba = lba;
    out.count = sectors;
    out.path = path;
    out.is_movie = path.size() >= 7 && path.compare(path.size() - 7, 7, ".raw2352") == 0;
    return true;
}

// layout.txt meta lines: system area, descriptors, directories, gaps.
//   meta <lba> <count> <index>
bool parse_meta_line(const std::string& line, LoadRange& out) {
    if (line.compare(0, 5, "meta ") != 0) return false;
    std::istringstream in(line.substr(5));
    uint32_t lba = 0, count = 0, index = 0;
    if (!(in >> lba >> count >> index)) return false;
    out.lba = lba;
    out.count = count;
    out.path = "";
    out.is_movie = false;
    return true;
}

}  // namespace

bool LoadMap::load_layout(const std::string& dir) {
    ranges_.clear();
    std::ifstream layout(dir + "/layout.txt");
    if (!layout) return false;
    std::string line;
    while (std::getline(layout, line)) {
        LoadRange r;
        if (parse_file_line(line, r) || parse_meta_line(line, r)) ranges_.push_back(std::move(r));
    }
    return !ranges_.empty();
}

const LoadRange* find_range(const std::vector<LoadRange>& ranges, uint32_t lba) {
    for (const LoadRange& r : ranges) {
        if (lba >= r.lba && lba < r.lba + r.count) return &r;
    }
    return nullptr;
}

std::string LoadMap::owner(uint32_t lba) const {
    if (const LoadRange* r = find_range(ranges_, lba)) {
        if (r->path.empty()) return "meta";
        // fs/B.DRV -> B.DRV
        const size_t slash = r->path.find_last_of('/');
        return slash == std::string::npos ? r->path : r->path.substr(slash + 1);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "lba %u", lba);
    return buf;
}

std::string LoadMap::path_for(uint32_t lba) const {
    if (const LoadRange* r = find_range(ranges_, lba)) return r->path;
    return "";
}

std::string DrvEntries::entry_at(const std::string& drv_path, const std::vector<uint8_t>& file_bytes,
                                 uint32_t offset) {
    for (const Cached& c : cache_) {
        if (c.path == drv_path) {
            const std::string* best = nullptr;
            for (const auto& [start, name] : c.spans) {
                if (start <= offset) best = &name;
                else break;
            }
            return best ? *best : "";
        }
    }
    // First touch: parse the file's TOC (32-byte records, sector*2048 rule).
    Cached c;
    c.path = drv_path;
    for (const vfs::TocEntry& e : vfs::parse_toc(file_bytes.data(), file_bytes.size(), 0)) {
        if (e.is_group) continue;
        c.spans.emplace_back(e.sector * 2048u, e.name);
    }
    std::string result;
    for (const auto& [start, name] : c.spans) {
        if (start <= offset) result = name;
        else break;
    }
    cache_.push_back(std::move(c));
    return result;
}

LoadLog& LoadLog::instance() {
    static LoadLog log;
    return log;
}

void LoadLog::set_data_dir(const std::string& dir) {
    if (!dir.empty()) map_.load_layout(dir);
}

void LoadLog::emit_run() {
    emit_spu();  // SPU runs interleave with sector traffic; keep line order
    if (!have_run_) return;
    have_run_ = false;
    const std::string file = map_.owner(run_first_);
    const uint32_t sectors = run_last_ - run_first_ + 1;
    // DRV entry names need the file bytes; the disc layer owns those, so the
    // log prints file + first LBA here and the analyst resolves entries with
    // tools/disc/drv_unpack.py (documented in RE_WORKFLOW.md).
    std::fprintf(stderr, "[load] frame %llu load %s lba %u %u KB\n", static_cast<unsigned long long>(frame_),
                 file.c_str(), run_first_, sectors * 2);
}

void LoadLog::sector(uint32_t lba) {
    if (!enabled_) return;
    emit_spu();  // a sector read breaks any SPU run
    if (have_run_ && lba == run_last_ + 1) {
        run_last_ = lba;
        return;
    }
    emit_run();
    have_run_ = true;
    run_first_ = run_last_ = lba;
}

void LoadLog::file(const std::string& path, uint32_t bytes, bool loose) {
    if (!enabled_) return;
    emit_run();
    std::fprintf(stderr, "[load] frame %llu file %s %u KB%s\n", static_cast<unsigned long long>(frame_), path.c_str(),
                 (bytes + 1023) / 1024, loose ? " (loose file)" : "");
}

void LoadLog::stream(uint32_t lba) {
    if (!enabled_) return;
    emit_run();
    std::fprintf(stderr, "[load] frame %llu stream %s from lba %u\n", static_cast<unsigned long long>(frame_),
                 map_.owner(lba).c_str(), lba);
}

void LoadLog::xa(bool start, uint8_t file, uint8_t channel) {
    if (!enabled_) return;
    emit_run();
    emit_spu();
    std::fprintf(stderr, "[load] frame %llu xa file=%u channel=%u %s\n", static_cast<unsigned long long>(frame_),
                 file, channel & 0x1F, start ? "start" : "stop");
}

void LoadLog::emit_spu() {
    if (!have_spu_) return;
    have_spu_ = false;
    const uint32_t bytes = spu_last_end_ - spu_first_;
    std::fprintf(stderr, "[load] frame %llu spu 0x%X %u KB\n", static_cast<unsigned long long>(frame_), spu_first_,
                 bytes / 1024);
}

void LoadLog::spu(uint32_t addr, uint32_t bytes) {
    if (!enabled_) return;
    if (have_spu_ && addr == spu_last_end_) {
        spu_last_end_ += bytes;
        return;
    }
    emit_spu();
    have_spu_ = true;
    spu_first_ = addr;
    spu_last_end_ = addr + bytes;
}

void LoadLog::mdec(bool start) {
    if (!enabled_) return;
    emit_run();
    emit_spu();
    if (start && !mdec_active_) {
        mdec_active_ = true;
        std::fprintf(stderr, "[load] frame %llu mdec start\n", static_cast<unsigned long long>(frame_));
    }
}

void LoadLog::mdec_frame(uint64_t transfers) {
    if (!enabled_) return;
    if (mdec_active_ && transfers == mdec_seen_) {
        mdec_active_ = false;
        emit_run();
        std::fprintf(stderr, "[load] frame %llu mdec stop\n", static_cast<unsigned long long>(frame_));
    }
    mdec_seen_ = transfers;
}

void LoadLog::flush() {
    if (!enabled_) return;
    emit_run();
}

}  // namespace hle
