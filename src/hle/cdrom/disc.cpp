#include "cdrom/disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hle {

namespace fs = std::filesystem;

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

/// First .cue in `dir`, else the first .bin, else empty.
fs::path find_image(const fs::path& dir) {
    fs::path bin;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".cue") return e.path();
        if (ext == ".bin" && bin.empty()) bin = e.path();
    }
    return bin;
}

bool parse_hex4(const std::string& hex, uint8_t* out) {
    if (hex.size() != 8) return false;
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(std::stoul(hex.substr(static_cast<size_t>(i) * 2, 2), nullptr, 16));
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared: ISO9660 access on top of read()

std::vector<uint8_t> Disc::read_root_file(const std::string& name) {
    // ISO9660 in Mode 2 Form 1 sectors: user data at raw offset 24. PVD at sector 16.
    uint8_t raw[kRawSector];
    if (!read(16, raw) || std::memcmp(raw + 24 + 1, "CD001", 5) != 0) return {};
    const uint8_t* root = raw + 24 + 156;
    const uint32_t dir_lba = le32(root + 2), dir_size = le32(root + 10);
    for (uint32_t s = 0; s * 2048 < dir_size; ++s) {
        if (!read(dir_lba + s, raw)) return {};
        const uint8_t* d = raw + 24;
        for (uint32_t off = 0; off < 2048 && d[off] != 0; off += d[off]) {
            const uint8_t len = d[off + 32];
            std::string ident(reinterpret_cast<const char*>(d + off + 33), len);
            ident = ident.substr(0, ident.find(';'));
            if (ident != name) continue;
            const uint32_t lba = le32(d + off + 2), size = le32(d + off + 10);
            std::vector<uint8_t> out;
            out.reserve(size);
            for (uint32_t k = 0; out.size() < size; ++k) {
                if (!read(lba + k, raw)) return {};
                const size_t take = std::min<size_t>(2048, size - out.size());
                out.insert(out.end(), raw + 24, raw + 24 + take);
            }
            return out;
        }
    }
    return {};
}

std::vector<uint8_t> Disc::read_boot_exe() {
    const std::vector<uint8_t> cnf = read_root_file("SYSTEM.CNF");
    std::string text(cnf.begin(), cnf.end());
    std::string boot = "PSX.EXE";
    const auto at = text.find("BOOT");
    if (at != std::string::npos) {
        const auto colon = text.find(':', at);
        const auto end = text.find_first_of(";\r\n", colon);
        boot = text.substr(colon + 1, end - colon - 1);
        while (!boot.empty() && (boot.front() == '\\' || boot.front() == '/')) boot.erase(0, 1);
    }
    std::vector<uint8_t> exe = read_root_file(boot);
    if (exe.empty()) throw std::runtime_error("boot executable " + boot + " not found on the disc");
    return exe;
}

std::unique_ptr<Disc> Disc::open(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return std::make_unique<ExtractedDisc>(path);
    return std::make_unique<ImageDisc>(path);
}

fs::path Disc::locate(const std::string& serial, const fs::path& hint) {
    if (!hint.empty()) return hint;
    if (const char* env = std::getenv("DCB_DISC")) return env;
    std::error_code ec;
    if (const fs::path extracted = fs::path("extracted") / serial; fs::exists(extracted / "layout.txt", ec))
        return extracted;
    for (const fs::path& dir : {fs::path("disc") / serial, fs::current_path()}) {
        if (fs::path image = find_image(dir); !image.empty()) return image;
    }
    throw std::runtime_error("no game data found: extract your dump into extracted/" + serial +
                             "/ (tools/disc/extract_disc.py), pass the .cue/.bin as the first argument, put it in disc/" +
                             serial + "/ or next to the program, or set DCB_DISC");
}

// ---------------------------------------------------------------------------------------------
// ImageDisc

ImageDisc::ImageDisc(const fs::path& image) {
    bin_ = image;
    if (image.extension() == ".cue" || image.extension() == ".CUE") {
        // First FILE entry is the data track for single-track PS1 games.
        std::ifstream cue(image);
        std::string line;
        while (std::getline(cue, line)) {
            const auto q1 = line.find('"');
            if (line.find("FILE") != std::string::npos && q1 != std::string::npos) {
                const auto q2 = line.find('"', q1 + 1);
                bin_ = image.parent_path() / line.substr(q1 + 1, q2 - q1 - 1);
                break;
            }
        }
    }
    file_.open(bin_, std::ios::binary);
    if (!file_) throw std::runtime_error("cannot open disc image " + bin_.string());
    sectors_ = static_cast<uint32_t>(fs::file_size(bin_) / kRawSector);
}

bool ImageDisc::read(uint32_t lba, uint8_t* out) {
    if (lba >= sectors_) return false;
    file_.seekg(static_cast<std::streamoff>(lba) * kRawSector);
    file_.read(reinterpret_cast<char*>(out), kRawSector);
    return static_cast<bool>(file_);
}

// ---------------------------------------------------------------------------------------------
// ExtractedDisc

ExtractedDisc::ExtractedDisc(const fs::path& dir) : dir_(dir) {
    std::ifstream layout(dir / "layout.txt");
    if (!layout) throw std::runtime_error("no layout.txt in " + dir.string() + " (re-run tools/disc/extract_disc.py)");
    std::string line;
    while (std::getline(layout, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        Range r;
        if (kind == "sectors") {
            in >> sectors_;
            continue;
        }
        if (kind == "meta") {
            r.kind = Range::Meta;
            in >> r.lba >> r.count >> r.meta_index;
            r.path = dir / "iso_meta.bin";
        } else if (kind == "file") {
            std::string storage, first, last, rel;
            in >> r.lba >> r.count >> r.bytes >> storage >> first >> last;
            std::getline(in >> std::ws, rel);  // path may contain spaces
            r.kind = storage == "raw2352" ? Range::Raw : Range::Form1;
            if (!parse_hex4(first, r.first_sh) || !parse_hex4(last, r.last_sh))
                throw std::runtime_error("bad subheader in layout.txt: " + line);
            r.path = dir / rel;
        } else {
            continue;
        }
        if (!in && r.count == 0) throw std::runtime_error("bad line in layout.txt: " + line);
        ranges_[r.lba] = std::move(r);
    }
    if (sectors_ == 0 || ranges_.empty()) throw std::runtime_error("empty layout.txt in " + dir.string());
}

std::ifstream& ExtractedDisc::stream(const fs::path& path) {
    auto it = open_.find(path.string());
    if (it == open_.end()) {
        it = open_.emplace(path.string(), std::ifstream(path, std::ios::binary)).first;
        if (!it->second) std::fprintf(stderr, "[disc] cannot open %s\n", path.string().c_str());
    }
    return it->second;
}

bool ExtractedDisc::read(uint32_t lba, uint8_t* out) {
    if (lba >= sectors_) return false;
    std::memset(out, 0, kRawSector);

    auto it = ranges_.upper_bound(lba);
    const Range* r = nullptr;
    if (it != ranges_.begin()) {
        --it;
        if (lba < it->second.lba + it->second.count) r = &it->second;
    }

    if (r && r->kind == Range::Meta) {
        std::ifstream& f = stream(r->path);
        f.clear();
        f.seekg(static_cast<std::streamoff>(r->meta_index + (lba - r->lba)) * kRawSector);
        f.read(reinterpret_cast<char*>(out), kRawSector);
        return true;
    }
    if (r && r->kind == Range::Raw) {
        std::ifstream& f = stream(r->path);
        f.clear();
        f.seekg(static_cast<std::streamoff>(lba - r->lba) * kRawSector);
        f.read(reinterpret_cast<char*>(out), kRawSector);
        return true;
    }

    // Form 1 (or an unused gap): sync, BCD MSF header + mode 2, subheader twice, 2048 data bytes.
    // EDC/ECC stay zero: the drive model never checks them.
    static constexpr uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(out, kSync, sizeof kSync);
    const uint32_t abs = lba + 150;
    out[12] = bcd(abs / 4500);
    out[13] = bcd(abs / 75 % 60);
    out[14] = bcd(abs % 75);
    out[15] = 2;
    if (!r) return true;
    const uint8_t* sh = (lba - r->lba + 1 == r->count) ? r->last_sh : r->first_sh;
    std::memcpy(out + 16, sh, 4);
    std::memcpy(out + 20, sh, 4);
    std::ifstream& f = stream(r->path);
    const uint32_t offset = (lba - r->lba) * 2048;
    if (offset < r->bytes) {
        f.clear();
        f.seekg(offset);
        f.read(reinterpret_cast<char*>(out + 24), std::min<uint32_t>(2048, r->bytes - offset));
    }
    return true;
}

}  // namespace hle
