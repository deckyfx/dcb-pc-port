// Memory-card file management: listing, backup, restore. Uses a temp dir;
// never touches real saves.

#include "memcard.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;

fs::path scratch() {
    fs::path dir = fs::temp_directory_path() / "dcb_memcard_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_bytes(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    using namespace memcard;
    // Empty / missing dir: no cards, backup fails cleanly.
    const fs::path dir = scratch();
    CHECK(list_cards(dir / "missing").empty());
    CHECK(backup_card(dir / "missing").empty());

    // Live card + two backups + junk: junk ignored, live first, newest first.
    write_bytes(dir / "card1.mcd", std::string(128 * 1024, 'A'));
    write_bytes(dir / "card1-20240101-120000.mcd", std::string(128 * 1024, 'B'));
    write_bytes(dir / "card1-20240201-120000.mcd", std::string(128 * 1024, 'C'));
    write_bytes(dir / "notes.txt", "junk");
    write_bytes(dir / "card2.mcd", "wrong slot");
    std::vector<CardInfo> cards = list_cards(dir);
    CHECK(cards.size() == 3);
    CHECK(cards[0].name == "card1.mcd" && !cards[0].is_backup && cards[0].bytes == 128 * 1024);
    CHECK(cards[1].name == "card1-20240201-120000.mcd" && cards[1].is_backup);
    CHECK(cards[2].name == "card1-20240101-120000.mcd" && cards[2].is_backup);

    // Backup: timestamped copy of the live card's bytes.
    const std::string name = backup_card(dir);
    CHECK(!name.empty() && name.compare(0, 6, "card1-") == 0);
    CHECK(read_all(dir / name) == std::string(128 * 1024, 'A'));
    CHECK(list_cards(dir).size() == 4);

    // UseCard: any file becomes live (atomic copy + auto-backup); rejects junk.
    CHECK(use_card(dir, "card1-20240101-120000.mcd").empty());
    CHECK(read_all(dir / "card1.mcd") == std::string(128 * 1024, 'B'));
    CHECK(use_card(dir, "card1.mcd").empty());  // already live: no-op success
    CHECK(!use_card(dir, "notes.txt").empty());
    CHECK(!use_card(dir, "../evil.mcd").empty());
    CHECK(!use_card(dir, "").empty());
    CHECK(read_all(dir / "card1.mcd") == std::string(128 * 1024, 'B'));  // unchanged
    // The auto-backup kept the pre-switch bytes (A), alongside the manual one.
    bool found_auto = false;
    for (const CardInfo& c : list_cards(dir)) {
        if (c.is_backup && read_all(dir / c.name) == std::string(128 * 1024, 'A')) found_auto = true;
    }
    CHECK(found_auto);

    std::printf("memcard: ok\n");
    return 0;
}
