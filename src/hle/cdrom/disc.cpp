#include "cdrom/disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hle {

namespace fs = std::filesystem;

Disc::Disc(const fs::path& image) {
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

bool Disc::read(uint32_t lba, uint8_t* out) {
    if (lba >= sectors_) return false;
    file_.seekg(static_cast<std::streamoff>(lba) * kRawSector);
    file_.read(reinterpret_cast<char*>(out), kRawSector);
    return static_cast<bool>(file_);
}

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

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

}  // namespace

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

fs::path Disc::locate(const std::string& serial, const fs::path& hint) {
    if (!hint.empty()) return hint;
    if (const char* env = std::getenv("DCB_DISC")) return env;
    for (const fs::path& dir : {fs::path("disc") / serial, fs::current_path()}) {
        if (fs::path image = find_image(dir); !image.empty()) return image;
    }
    throw std::runtime_error("no disc image found: pass the .cue/.bin as the first argument, put it in disc/" +
                             serial + "/ or next to the program, or set DCB_DISC");
}

}  // namespace hle
