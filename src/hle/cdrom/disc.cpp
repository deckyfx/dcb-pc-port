#include "cdrom/disc.hpp"

#include <cstdlib>
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

fs::path Disc::locate(const std::string& serial) {
    if (const char* env = std::getenv("DCB_DISC")) return env;
    const fs::path dir = fs::path("disc") / serial;
    fs::path bin;
    if (fs::is_directory(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            const auto ext = e.path().extension();
            if (ext == ".cue") return e.path();
            if (ext == ".bin" && bin.empty()) bin = e.path();
        }
    }
    if (bin.empty()) throw std::runtime_error("no disc image: put the .cue/.bin in " + dir.string() + " or set DCB_DISC");
    return bin;
}

}  // namespace hle
