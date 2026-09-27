// Asset importer tests on a tiny synthetic PlayStation disc built here (no game data needed):
// cue parsing, serial detection, sector/subheader handling, the layout written, a round trip
// through ExtractedDisc, and the error cases (not a PS1 disc, truncated, audio track, cooked ISO,
// unknown serial, existing destination, cancellation).
//
// Also a helper for tests/import/python_parity.cmake:
//   test_importer --write-image <file.bin>      write the synthetic disc
//   test_importer --import <image> <dest>       import it (accepting its serial)

#include "cdrom/disc.hpp"
#include "cdrom/importer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace fs = std::filesystem;
namespace imp = hle::import;

namespace {

constexpr uint32_t kRaw = 2352;
constexpr const char* kSerial = "SLPS-99999";
const std::string kSystemCnf = "BOOT = cdrom:\\SLPS_999.99;1\r\nTCB = 4\r\nEVENT = 10\r\n";

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

void put_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_be32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[3 - i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_both32(uint8_t* p, uint32_t v) {
    put_le32(p, v);
    put_be32(p + 4, v);
}

/// A raw Mode 2 disc under construction. EDC/ECC stay zero (as ExtractedDisc rebuilds them),
/// so a round trip must reproduce every sector exactly.
struct DiscBuilder {
    std::vector<uint8_t> data;
    explicit DiscBuilder(uint32_t sectors) : data(static_cast<size_t>(sectors) * kRaw, 0) {
        for (uint32_t lba = 0; lba < sectors; ++lba) header(lba, 0x08);
    }
    uint8_t* sector(uint32_t lba) { return data.data() + static_cast<size_t>(lba) * kRaw; }
    uint8_t* user(uint32_t lba) { return sector(lba) + 24; }
    void header(uint32_t lba, uint8_t submode) {
        uint8_t* s = sector(lba);
        s[0] = 0;
        std::memset(s + 1, 0xFF, 10);
        s[11] = 0;
        const uint32_t abs = lba + 150;
        s[12] = bcd(abs / 4500);
        s[13] = bcd(abs / 75 % 60);
        s[14] = bcd(abs % 75);
        s[15] = 2;
        s[16] = s[20] = 0;  // file
        s[17] = s[21] = 0;  // channel
        s[18] = s[22] = submode;
        s[19] = s[23] = 0;
    }
    /// A Form 1 file: `size` bytes of a pattern, EOF/EOR submode on its last sector.
    void form1_file(uint32_t lba, uint32_t size, uint8_t seed) {
        const uint32_t n = (size + 2047) / 2048;
        for (uint32_t i = 0; i < n; ++i) {
            header(lba + i, i + 1 == n ? 0x89 : 0x08);
            const uint32_t take = std::min<uint32_t>(2048, size - i * 2048);
            for (uint32_t k = 0; k < take; ++k) user(lba + i)[k] = static_cast<uint8_t>(seed + k * 7 + i);
        }
    }
    void form2_sector(uint32_t lba, uint8_t seed) {
        header(lba, 0x64);  // Form 2, audio, real-time
        sector(lba)[17] = sector(lba)[21] = 1;
        for (uint32_t k = 0; k < 2328; ++k) sector(lba)[24 + k] = static_cast<uint8_t>(seed ^ k);
    }
};

/// Append a directory record; returns its length.
size_t dir_record(uint8_t* at, const std::string& name, uint32_t lba, uint32_t size, bool dir, int xa = -1) {
    const size_t name_len = name.size();
    size_t len = 33 + name_len + (name_len % 2 == 0 ? 1 : 0);
    const size_t su = len;
    if (xa >= 0) len += 14;
    at[0] = static_cast<uint8_t>(len);
    put_both32(at + 2, lba);
    put_both32(at + 10, size);
    at[25] = dir ? 0x02 : 0x00;
    at[32] = static_cast<uint8_t>(name_len);
    std::memcpy(at + 33, name.data(), name_len);
    if (xa >= 0) {
        at[su + 4] = static_cast<uint8_t>(xa >> 8);
        at[su + 5] = static_cast<uint8_t>(xa);
        at[su + 6] = 'X';
        at[su + 7] = 'A';
    }
    return len;
}

/// The test disc (37 sectors):
///   0-15 system area   16 PVD   17 terminator   18 root   19 DATA/   20 DATA/SUB/
///   21 SYSTEM.CNF   22-23 SLPS_999.99   24-26 DATA/INNER.BIN   27 DATA/SUB/DEEP.TXT
///   28-31 XA.STR (XA Form 2 attribute)   32-33 MIX.BIN (Form 1 attribute, one Form 2 sector)
///   34 gap   35-36 post-gap; EMPTY.DAT (0 bytes) and TRACK2.DA (CD-DA, outside the image).
std::vector<uint8_t> make_disc() {
    DiscBuilder d(37);
    std::memcpy(d.user(4), "Licensed by Sony Computer Entertainment", 39);

    uint8_t* pvd = d.user(16);
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memset(pvd + 8, ' ', 64);
    std::memcpy(pvd + 8, "PLAYSTATION", 11);
    std::memcpy(pvd + 40, "TESTDISC", 8);
    put_both32(pvd + 80, 35);            // volume space size (the post-gap is outside)
    pvd[128] = 0x00, pvd[129] = 0x08;    // logical block size 2048 (LE)
    pvd[130] = 0x08, pvd[131] = 0x00;
    dir_record(pvd + 156, std::string(1, '\0'), 18, 2048, true);
    uint8_t* term = d.user(17);
    term[0] = 255;
    std::memcpy(term + 1, "CD001", 5);

    const std::string cnf = kSystemCnf;
    uint8_t* root = d.user(18);
    size_t o = 0;
    o += dir_record(root + o, std::string(1, '\0'), 18, 2048, true);
    o += dir_record(root + o, std::string(1, '\1'), 18, 2048, true);
    o += dir_record(root + o, "SYSTEM.CNF;1", 21, static_cast<uint32_t>(cnf.size()), false, 0x0D55);
    o += dir_record(root + o, "SLPS_999.99;1", 22, 3000, false, 0x0D55);
    o += dir_record(root + o, "DATA", 19, 2048, true, 0x8D55);
    o += dir_record(root + o, "XA.STR;1", 28, 4 * 2048, false, 0x3D55);
    o += dir_record(root + o, "MIX.BIN;1", 32, 2 * 2048, false, 0x0D55);
    o += dir_record(root + o, "EMPTY.DAT;1", 34, 0, false, 0x0D55);
    o += dir_record(root + o, "TRACK2.DA;1", 5000, 30 * 2048, false, 0x4555);
    uint8_t* data_dir = d.user(19);
    o = 0;
    o += dir_record(data_dir + o, std::string(1, '\0'), 19, 2048, true);
    o += dir_record(data_dir + o, std::string(1, '\1'), 18, 2048, true);
    o += dir_record(data_dir + o, "INNER.BIN;1", 24, 5000, false, 0x0D55);
    o += dir_record(data_dir + o, "SUB", 20, 2048, true, 0x8D55);
    uint8_t* sub = d.user(20);
    o = 0;
    o += dir_record(sub + o, std::string(1, '\0'), 20, 2048, true);
    o += dir_record(sub + o, std::string(1, '\1'), 19, 2048, true);
    o += dir_record(sub + o, "DEEP.TXT;1", 27, 10, false);  // no XA system use area
    for (uint32_t lba = 16; lba <= 20; ++lba) d.header(lba, 0x89);

    d.form1_file(21, static_cast<uint32_t>(cnf.size()), 0);
    std::memcpy(d.user(21), cnf.data(), cnf.size());
    d.form1_file(22, 3000, 0x10);
    std::memcpy(d.user(22), "PS-X EXE", 8);
    d.form1_file(24, 5000, 0x20);
    d.form1_file(27, 10, 0x30);
    for (uint32_t i = 0; i < 4; ++i) d.form2_sector(28 + i, static_cast<uint8_t>(0x40 + i));
    d.form1_file(32, 4096, 0x50);
    d.form2_sector(33, 0x60);
    std::memcpy(d.user(34), "gap", 3);
    d.header(35, 0x20);  // post-gap: Form 2, empty
    d.header(36, 0x20);
    return d.data;
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}
std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string read_text(const fs::path& path) {
    const std::vector<uint8_t> b = read_file(path);
    return {b.begin(), b.end()};
}

imp::Options test_options() {
    imp::Options o;
    o.accepted_serials = {kSerial};
    return o;
}

imp::ErrorCode import_error(const fs::path& image, const fs::path& dest, const imp::Options& options) {
    try {
        imp::import_disc(image, dest, options);
    } catch (const imp::ImportError& e) {
        std::printf("  expected error: %s\n", e.what());
        return e.code();
    }
    std::fprintf(stderr, "import of %s unexpectedly succeeded\n", image.string().c_str());
    std::exit(1);
}

/// Nothing but `names` in `dir` (no temporary leftovers).
bool only(const fs::path& dir, std::vector<std::string> names) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) found.push_back(e.path().filename().string());
    std::sort(found.begin(), found.end());
    std::sort(names.begin(), names.end());
    return found == names;
}

void test_cue(const fs::path& tmp) {
    auto t = imp::parse_cue(
        "\xEF\xBB\xBF" "FILE \"Game (Track 1).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n"
        "FILE track2.bin BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n",
        "/discs");
    CHECK(t.size() == 2);
    CHECK(t[0].number == 1 && t[0].type == "MODE2/2352" && t[0].file == fs::path("/discs") / "Game (Track 1).bin");
    CHECK(t[1].number == 2 && t[1].type == "AUDIO" && t[1].file == fs::path("/discs") / "track2.bin");

    // Data track after an audio track in the sheet, file name case differs from the disk.
    write_file(tmp / "DISC.BIN", {1, 2, 3});
    write_text(tmp / "a.cue", "FILE \"x.bin\" BINARY\nTRACK 01 AUDIO\nFILE \"disc.bin\" BINARY\nTRACK 02 MODE2/2352\n");
    CHECK(imp::resolve_data_track(tmp / "a.cue") == tmp / "DISC.BIN");
    CHECK(imp::resolve_data_track(tmp / "DISC.BIN") == tmp / "DISC.BIN");

    write_text(tmp / "audio.cue", "FILE \"music.bin\" BINARY\n  TRACK 01 AUDIO\n");
    try {
        imp::resolve_data_track(tmp / "audio.cue");
        CHECK(false);
    } catch (const imp::ImportError& e) {
        CHECK(e.code() == imp::ErrorCode::AudioTrack);
    }
    write_text(tmp / "missing.cue", "FILE \"gone.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
    try {
        imp::resolve_data_track(tmp / "missing.cue");
        CHECK(false);
    } catch (const imp::ImportError& e) {
        CHECK(e.code() == imp::ErrorCode::Io);
    }
}

void test_serial() {
    CHECK(imp::serial_from_boot("cdrom:\\SLPS_031.01;1") == "SLPS-03101");
    CHECK(imp::serial_from_boot("  cdrom:\\SLUS_013.28;1 arg") == "SLUS-01328");
    CHECK(imp::serial_from_boot("cdrom:\\GAME\\sces_123.45;1") == "SCES-12345");
    CHECK(imp::serial_from_boot("cdrom:PSX.EXE;1").empty());
    CHECK(imp::serial_from_boot("").empty());
    CHECK(imp::known_title("SLPS-03101") != nullptr && imp::known_title("SLUS-01328") != nullptr);
    CHECK(imp::known_title("SLPS-99999") == nullptr);
}

void test_import(const fs::path& tmp) {
    const std::vector<uint8_t> disc = make_disc();
    const fs::path bin = tmp / "test.bin";
    write_file(bin, disc);
    write_text(tmp / "test.cue", "FILE \"test.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");

    const imp::DiscInfo info = imp::identify(tmp / "test.cue");
    CHECK(info.serial == kSerial && info.volume_id == "TESTDISC" && info.sectors == 37);

    // The default serial list (the real game) refuses it, writing nothing.
    const fs::path dest = tmp / "extracted";
    CHECK(import_error(bin, dest, {}) == imp::ErrorCode::UnknownSerial);
    CHECK(!fs::exists(dest / kSerial));

    imp::Options options = test_options();
    uint64_t last_done = 0, total = 0;
    bool monotonic = true;
    options.progress = [&](const imp::Progress& p) {
        monotonic = monotonic && p.done >= last_done && p.done <= p.total;
        last_done = p.done;
        total = p.total;
        return true;
    };
    const imp::Result r = imp::import_disc(tmp / "test.cue", dest, options);
    CHECK(monotonic && last_done == total && total > 0);
    CHECK(r.dir == dest / kSerial && r.files == 7);
    CHECK(only(dest, {kSerial}));
    const fs::path out = r.dir;

    // layout.txt: meta ranges (system area + descriptors + directories; gap + post-gap), then
    // files by LBA with their first/last subheaders; empty and CD-DA files are not listed.
    const std::string expected =
        "# dcb extracted disc layout v1\n"
        "sectors 37\n"
        "meta 0 21 0\n"
        "meta 34 3 21\n"
        "file 21 1 " + std::to_string(kSystemCnf.size()) + " form1 00008900 00008900 fs/SYSTEM.CNF\n"
        "file 22 2 3000 form1 00000800 00008900 fs/SLPS_999.99\n"
        "file 24 3 5000 form1 00000800 00008900 fs/DATA/INNER.BIN\n"
        "file 27 1 10 form1 00008900 00008900 fs/DATA/SUB/DEEP.TXT\n"
        "file 28 4 8192 raw2352 00016400 00016400 fs/XA.STR.raw2352\n"
        "file 32 2 4096 raw2352 00000800 00016400 fs/MIX.BIN.raw2352\n";
    const std::string layout = read_text(out / "layout.txt");
    if (layout != expected) std::fprintf(stderr, "layout.txt:\n%s", layout.c_str());
    CHECK(layout == expected);

    // iso_meta.bin: the uncovered sectors, raw.
    const std::vector<uint8_t> meta = read_file(out / "iso_meta.bin");
    CHECK(meta.size() == 24u * kRaw);
    CHECK(std::equal(meta.begin(), meta.begin() + 21 * kRaw, disc.begin()));
    CHECK(std::equal(meta.begin() + 21 * kRaw, meta.end(), disc.begin() + 34 * kRaw));

    // Files: Form 1 trimmed to their size, Form 2 as whole sectors, the empty one present.
    CHECK(read_file(out / "fs/DATA/INNER.BIN").size() == 5000);
    CHECK(read_file(out / "fs/DATA/SUB/DEEP.TXT").size() == 10);
    CHECK(read_text(out / "fs/SYSTEM.CNF").rfind("BOOT = cdrom:", 0) == 0);
    const std::vector<uint8_t> xa = read_file(out / "fs/XA.STR.raw2352");
    CHECK(xa.size() == 4u * kRaw && std::equal(xa.begin(), xa.end(), disc.begin() + 28 * kRaw));
    CHECK(fs::exists(out / "fs/EMPTY.DAT") && fs::file_size(out / "fs/EMPTY.DAT") == 0);
    CHECK(!fs::exists(out / "fs/TRACK2.DA"));

    // Round trip: every sector rebuilt from the imported files equals the original.
    hle::ExtractedDisc extracted(out);
    CHECK(extracted.sector_count() == 37);
    uint8_t sector[kRaw];
    for (uint32_t lba = 0; lba < 37; ++lba) {
        CHECK(extracted.read(lba, sector));
        if (std::memcmp(sector, disc.data() + static_cast<size_t>(lba) * kRaw, kRaw) != 0) {
            std::fprintf(stderr, "sector %u differs after the round trip\n", lba);
            CHECK(false);
        }
    }
    CHECK(extracted.read_boot_exe().size() == 3000);

    // A second import needs overwrite; with it, the tree is replaced atomically.
    CHECK(import_error(bin, dest, test_options()) == imp::ErrorCode::AlreadyExists);
    write_text(out / "stale.txt", "x");
    imp::Options again = test_options();
    again.overwrite = true;
    imp::import_disc(bin, dest, again);
    CHECK(!fs::exists(out / "stale.txt") && fs::exists(out / "layout.txt"));
    CHECK(only(dest, {kSerial}));

    // Cancelling part-way leaves nothing behind (not even the temporary directory).
    const fs::path dest2 = tmp / "cancelled";
    imp::Options cancel = test_options();
    int calls = 0;
    cancel.progress = [&](const imp::Progress&) { return ++calls < 12; };  // stops while copying
    CHECK(import_error(bin, dest2, cancel) == imp::ErrorCode::Cancelled);
    CHECK(only(dest2, {}));
}

void test_errors(const fs::path& tmp) {
    const std::vector<uint8_t> disc = make_disc();
    const fs::path dest = tmp / "errors";

    // Cut off inside the filesystem.
    write_file(tmp / "short.bin", std::vector<uint8_t>(disc.begin(), disc.begin() + 30 * kRaw));
    CHECK(import_error(tmp / "short.bin", dest, test_options()) == imp::ErrorCode::Truncated);
    // Cut off in the post-gap only: still complete (the post-gap is outside the filesystem).
    write_file(tmp / "nogap.bin", std::vector<uint8_t>(disc.begin(), disc.begin() + 35 * kRaw));
    CHECK(imp::identify(tmp / "nogap.bin").sectors == 35);

    // Raw sectors but no ISO9660 volume.
    std::vector<uint8_t> no_pvd = disc;
    no_pvd[16 * kRaw + 24 + 1] = 'X';
    write_file(tmp / "nopvd.bin", no_pvd);
    CHECK(import_error(tmp / "nopvd.bin", dest, test_options()) == imp::ErrorCode::NotPlayStation);

    // Mode 1 (a PC CD-ROM).
    std::vector<uint8_t> mode1 = disc;
    mode1[15] = 1;
    write_file(tmp / "mode1.bin", mode1);
    CHECK(import_error(tmp / "mode1.bin", dest, test_options()) == imp::ErrorCode::NotPlayStation);

    // No SYSTEM.CNF: an ISO9660 data disc that is not a PlayStation game.
    std::vector<uint8_t> no_cnf = disc;
    std::memcpy(no_cnf.data() + 18 * kRaw + 24 + 34 * 2 + 33, "SYSTEM.CNG", 10);
    write_file(tmp / "nocnf.bin", no_cnf);
    CHECK(import_error(tmp / "nocnf.bin", dest, test_options()) == imp::ErrorCode::NotPlayStation);

    // An audio track (no sync pattern), a cooked 2048-byte ISO, a CHD.
    write_file(tmp / "track02.bin", std::vector<uint8_t>(20 * kRaw, 0x11));
    CHECK(import_error(tmp / "track02.bin", dest, test_options()) == imp::ErrorCode::AudioTrack);
    std::vector<uint8_t> iso(20 * 2048, 0);
    std::memcpy(iso.data() + 16 * 2048, "\1CD001", 6);
    write_file(tmp / "game.iso", iso);
    CHECK(import_error(tmp / "game.iso", dest, test_options()) == imp::ErrorCode::UnsupportedFormat);
    write_file(tmp / "game.chd", {1});
    CHECK(import_error(tmp / "game.chd", dest, test_options()) == imp::ErrorCode::UnsupportedFormat);
    CHECK(import_error(tmp / "absent.bin", dest, test_options()) == imp::ErrorCode::Io);

    CHECK(!fs::exists(dest) || only(dest, {}));
}

}  // namespace


/// overrides/<name>: a same-size file replaces the disc file (raw sectors re-stamped with the
/// position they are served at); any other size is ignored.
void test_overrides(const fs::path& tmp) {
    const fs::path dir = tmp / "overrides_disc";
    fs::create_directories(dir / "fs");
    fs::create_directories(dir / "overrides");
    // Two raw sectors at LBA 5 and a 3000-byte Form 1 file at LBA 7 (2 sectors).
    write_text(dir / "layout.txt",
               "sectors 16\n"
               "file 5 2 4704 raw2352 00000000 00000000 fs/MOVIE.RAW\n"
               "file 7 2 3000 form1 00000800 00008900 fs/DATA.BIN\n");
    std::vector<uint8_t> original(2 * 2352, 0x11), replacement(2 * 2352, 0x22);
    replacement[12] = 0x99;  // a header from another disc position
    write_file(dir / "fs" / "MOVIE.RAW", original);
    write_file(dir / "overrides" / "MOVIE.RAW", replacement);
    write_file(dir / "fs" / "DATA.BIN", std::vector<uint8_t>(3000, 0x33));
    write_file(dir / "overrides" / "DATA.BIN", std::vector<uint8_t>(2999, 0x44));  // wrong size

    hle::ExtractedDisc disc(dir);
    uint8_t sector[2352];
    CHECK(disc.read(6, sector));
    CHECK(sector[100] == 0x22);                                         // served from overrides/
    CHECK(sector[12] == 0x00 && sector[13] == 0x02 && sector[14] == 0x06);  // MSF of LBA 6: 00:02:06
    CHECK(disc.read(7, sector));
    CHECK(sector[24] == 0x33);  // wrong-size override ignored: the disc file is used
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--write-image") == 0) {
        write_file(argv[2], make_disc());
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--import") == 0) {
        try {
            imp::import_disc(argv[2], argv[3], test_options());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
        return 0;
    }

    const fs::path tmp = fs::temp_directory_path() /
                         ("dcb_import_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    test_serial();
    test_cue(tmp);
    test_import(tmp);
    test_errors(tmp);
    test_overrides(tmp);
    fs::remove_all(tmp);
    std::printf("import tests passed\n");
    return 0;
}
