// Load-log mapping tests (sector -> file, no game data needed).

#include "cdrom/load_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

namespace fs = std::filesystem;

fs::path scratch() {
    fs::path dir = fs::temp_directory_path() / "dcb_load_log_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

int main() {
    // Synthetic layout.txt: meta, two files, a movie.
    const fs::path dir = scratch();
    {
        std::ofstream out(dir / "layout.txt");
        out << "# test\nsectors 100\nmeta 0 16 0\n"
               "file 16 1 67 form1 00008900 00008900 fs/SYSTEM.CNF\n"
               "file 20 10 20000 form1 00000800 00008900 fs/B.DRV\n"
               "file 40 8 10000 raw2352 00014800 01016401 fs/DIGIMON.MOV.raw2352\n";
    }
    hle::LoadMap map;
    CHECK(map.load_layout(dir.string()));
    CHECK(map.owner(0) == "meta");
    CHECK(map.owner(16) == "SYSTEM.CNF");
    CHECK(map.owner(21) == "B.DRV");
    CHECK(map.owner(29) == "B.DRV");
    CHECK(map.owner(40) == "DIGIMON.MOV.raw2352");
    CHECK(map.owner(99) == "lba 99");
    CHECK(map.path_for(21) == "fs/B.DRV");
    CHECK(map.path_for(0) == "");
    CHECK(!map.load_layout((dir / "missing").string()));

    // DRV entry cache: minimal TOC (one file record + terminator).
    std::vector<uint8_t> drv(4 * 32, 0);
    drv[0] = 0x01;
    drv[1] = 'T';
    drv[2] = 'I';
    drv[3] = 'M';
    drv[4] = 12;  // sector 12 -> offset 24576
    drv[8] = 0x10;
    std::memcpy(drv.data() + 16, "TITLE", 5);
    hle::DrvEntries entries;
    CHECK(entries.entry_at("B.DRV", drv, 24576) == "TITLE");
    CHECK(entries.entry_at("B.DRV", drv, 24577) == "TITLE");
    CHECK(entries.entry_at("B.DRV", drv, 0) == "");  // before any entry
    CHECK(entries.entry_at("OTHER.DRV", drv, 24576) == "TITLE");

    std::printf("load_log: ok\n");
    return 0;
}
