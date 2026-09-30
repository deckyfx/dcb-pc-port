// SHA-1 (src/hle/cdrom/sha1.*) against the FIPS 180 / RFC 3174 test vectors, fed whole and in
// odd-sized pieces, and the dump check built on it (hle::import::verify_data_track) on a small
// file: good, wrong size, wrong content, cancelled.

#include "cdrom/importer.hpp"
#include "cdrom/sha1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

std::string sha1_of(const std::string& s) { return hle::Sha1::hex_of(s.data(), s.size()); }

/// The same data fed `step` bytes at a time.
std::string sha1_pieces(const std::string& s, size_t step) {
    hle::Sha1 sha;
    for (size_t i = 0; i < s.size(); i += step) sha.update(s.data() + i, std::min(step, s.size() - i));
    return hle::Sha1::to_hex(sha.finish());
}

void test_vectors() {
    CHECK(sha1_of("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_of("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha1_of(two_blocks) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(sha1_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqr"
                  "lmnopqrsmnopqrstnopqrstu") == "a49b2446a02c645bf419f995b67091253a04a259");
    const std::string million(1000000, 'a');
    CHECK(sha1_of(million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    for (size_t step : {1u, 3u, 63u, 64u, 65u, 1000u}) CHECK(sha1_pieces(million, step) == sha1_of(million));
    // Padding edges: 55, 56 and 64 bytes.
    CHECK(sha1_pieces(std::string(55, 'x'), 7) == sha1_of(std::string(55, 'x')));
    CHECK(sha1_of(std::string(64, 'a')) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    // reset() makes the object reusable.
    hle::Sha1 sha;
    sha.update("junk", 4);
    (void)sha.finish();
    sha.reset();
    sha.update("abc", 3);
    CHECK(hle::Sha1::to_hex(sha.finish()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

/// The error verify_data_track throws, or nullopt when it accepts the file.
std::optional<imp::ErrorCode> verify_code(const fs::path& file, uint64_t size, const char* sha1, const imp::ProgressFn& progress = {}) {
    try {
        imp::verify_data_track(file, size, sha1, "test disc", progress);
    } catch (const imp::ImportError& e) {
        return e.code();
    }
    return std::nullopt;
}

void test_verify(const fs::path& work) {
    const fs::path file = work / "track.bin";
    const std::string data(3u << 20, 'a');  // several read chunks
    std::ofstream(file, std::ios::binary) << data;
    const std::string good = sha1_of(data);
    CHECK(!verify_code(file, data.size(), good.c_str()));

    uint64_t last = 0, calls = 0;
    imp::verify_data_track(file, data.size(), good, "test disc", [&](const imp::Progress& p) {
        CHECK(p.total == data.size() && p.done > last);
        last = p.done;
        ++calls;
        return true;
    });
    CHECK(last == data.size() && calls >= 3);

    CHECK(verify_code(file, data.size() + 1, good.c_str()) == imp::ErrorCode::BadDump);
    CHECK(verify_code(file, data.size(), "0000000000000000000000000000000000000000") == imp::ErrorCode::BadDump);
    CHECK(verify_code(file, data.size(), good.c_str(), [](const imp::Progress&) { return false; }) ==
          imp::ErrorCode::Cancelled);
    CHECK(verify_code(work / "missing.bin", 1, good.c_str()) == imp::ErrorCode::Io);

    // The known-good dumps are in the table with sizes and hashes.
    for (const char* serial : {"SLPS-03101", "SLUS-01328"}) {
        const imp::KnownGame* g = imp::known_game(serial);
        CHECK(g != nullptr && g->data_size % 2352 == 0 && std::string(g->data_sha1).size() == 40);
    }
}

}  // namespace

int main() {
    test_vectors();
    const fs::path work = fs::temp_directory_path() / ("dcb_test_sha1_" + std::to_string(std::rand()));
    fs::create_directories(work);
    test_verify(work);
    fs::remove_all(work);
    std::printf("sha1: ok\n");
    return 0;
}
