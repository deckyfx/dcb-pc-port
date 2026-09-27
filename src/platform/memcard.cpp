// Memory-card file management (see memcard.hpp).

#include "memcard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace memcard {

namespace fs = std::filesystem;

namespace {

bool is_card_file(const fs::path& p) {
    if (p.extension() != ".mcd") return false;
    const std::string name = p.filename().string();
    return name == "card1.mcd" || (name.compare(0, 6, "card1-") == 0);
}

std::string timestamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

uint64_t file_bytes(const fs::path& p) {
    std::error_code ec;
    const uint64_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

bool copy_file(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    FILE* in = std::fopen(from.string().c_str(), "rb");
    if (!in) {
        error = "cannot open " + from.string();
        return false;
    }
    FILE* out = std::fopen(to.string().c_str(), "wb");
    if (!out) {
        std::fclose(in);
        error = "cannot write " + to.string();
        return false;
    }
    char buf[65536];
    size_t n = 0;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (std::ferror(in)) ok = false;
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) error = "copy failed";
    return ok;
}

}  // namespace

std::vector<CardInfo> list_cards(const fs::path& save_dir) {
    std::vector<CardInfo> out;
    std::error_code ec;
    if (!fs::is_directory(save_dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(save_dir, ec)) {
        if (!e.is_regular_file() || !is_card_file(e.path())) continue;
        CardInfo info;
        info.name = e.path().filename().string();
        info.bytes = file_bytes(e.path());
        info.is_backup = info.name != "card1.mcd";
        out.push_back(std::move(info));
    }
    // Live card first, then backups newest-first (timestamps sort lexically).
    std::sort(out.begin(), out.end(), [](const CardInfo& a, const CardInfo& b) {
        if (a.is_backup != b.is_backup) return !a.is_backup;
        return a.name > b.name;
    });
    return out;
}

std::string backup_card(const fs::path& save_dir) {
    const fs::path live = save_dir / "card1.mcd";
    std::error_code ec;
    if (!fs::is_regular_file(live, ec)) return "";
    const std::string name = "card1-" + timestamp() + ".mcd";
    std::string error;
    if (!copy_file(live, save_dir / name, error)) return "";
    return name;
}

std::string restore_card(const fs::path& save_dir, const std::string& backup_name) {
    if (backup_name.empty() || backup_name.find('/') != std::string::npos ||
        backup_name.find('\\') != std::string::npos || backup_name == "card1.mcd" ||
        !is_card_file(save_dir / backup_name)) {
        return "not a backup: " + backup_name;
    }
    std::string error;
    if (!copy_file(save_dir / backup_name, save_dir / "card1.mcd", error)) return error;
    return "";
}

}  // namespace memcard
