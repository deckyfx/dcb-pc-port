// BIOS memory-card file system (hle::CardFs) over temporary .mcd images.

#include "mcrd/card_fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
using hle::CardFs;
using hle::MemoryCard;

constexpr uint32_t kBlock = CardFs::kBlockSize;

std::vector<uint8_t> image(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

const uint8_t* dir(const std::vector<uint8_t>& img, uint32_t block) { return img.data() + block * 128; }
uint32_t u32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
bool checksum_ok(const uint8_t* f) {
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; ++i) x ^= f[i];
    return x == f[0x7F];
}

std::vector<uint8_t> pattern(uint32_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + seed + i / 251);
    return v;
}

}  // namespace

int main() {
    const fs::path dir_path = fs::temp_directory_path() / "dcb_test_mcrd";
    fs::remove_all(dir_path);
    fs::create_directories(dir_path);
    const fs::path path = dir_path / "card1.mcd";

    {
        MemoryCard card(path);
        CardFs cfs({&card, nullptr});

        // Format, then the directory is empty.
        CHECK(cfs.format("bu00:"));
        CHECK(cfs.free_blocks(0) == 15);
        CardFs::DirEntry e{};
        CHECK(!cfs.firstfile("bu00:*", e));
        CHECK(cfs.last_error() == CardFs::kENoEnt);

        // Missing file / second slot absent / bad device.
        CHECK(cfs.open("bu00:NOPE", CardFs::kFRead) == -1);
        CHECK(cfs.last_error() == CardFs::kENoEnt);
        CHECK(cfs.open("bu10:NOPE", CardFs::kFRead) == -1);
        CHECK(cfs.last_error() == CardFs::kENoDev);
        CHECK(!cfs.firstfile("bu10:*", e));
        CHECK(cfs.last_error() == CardFs::kENoDev);
        CHECK(cfs.open("cdrom:X", CardFs::kFRead) == -1);
        CHECK(cfs.open("bu00:ABCDEFGHIJKLMNOPQRSTU", CardFs::kFCreate) == -1);  // 21 chars
        CHECK(cfs.last_error() == CardFs::kEInval);

        // 1-block file.
        int fd = cfs.open("bu00:BISLPS-03101DCB", CardFs::kFCreate | (1u << 16));
        CHECK(fd >= CardFs::kFirstFd);
        const auto one = pattern(kBlock, 1);
        CHECK(cfs.write(fd, one.data(), kBlock) == static_cast<int>(kBlock));
        CHECK(!cfs.last_was_async());
        CHECK(cfs.write(fd, one.data(), 128) == -1);  // past the end
        CHECK(cfs.last_error() == CardFs::kENoSpc);
        CHECK(cfs.close(fd) == fd);
        CHECK(cfs.close(fd) == -1);
        CHECK(cfs.last_error() == CardFs::kEBadF);
        CHECK(cfs.open("bu00:BISLPS-03101DCB", CardFs::kFCreate | (1u << 16)) == -1);
        CHECK(cfs.last_error() == CardFs::kEExist);

        // 3-block file, async: write across block boundaries in odd-sized chunks.
        fd = cfs.open("bu00:BISLPS-03101DCB2", CardFs::kFCreate | CardFs::kFAsync | (3u << 16));
        CHECK(fd >= CardFs::kFirstFd);
        CHECK(cfs.free_blocks(0) == 11);
        const auto three = pattern(3 * kBlock, 9);
        CHECK(cfs.write(fd, three.data(), kBlock - 256) == static_cast<int>(kBlock - 256));
        CHECK(cfs.last_was_async() && cfs.async_spec() == CardFs::kSpecDone);
        CHECK(cfs.write(fd, three.data() + kBlock - 256, kBlock + 512) == static_cast<int>(kBlock + 512));
        CHECK(cfs.write(fd, three.data() + 2 * kBlock + 256, kBlock) == static_cast<int>(kBlock - 256));  // clipped
        CHECK(cfs.write(fd, three.data(), 100) == -1);  // not a multiple of 128
        CHECK(cfs.last_error() == CardFs::kEInval);
        CHECK(cfs.last_was_async() && cfs.async_spec() == CardFs::kSpecError);
        CHECK(cfs.file_error(fd) == CardFs::kEInval);
        CHECK(cfs.close(fd) == fd);

        // Read back across boundaries, with lseek.
        fd = cfs.open("bu00:BISLPS-03101DCB2", CardFs::kFRead);
        CHECK(fd >= 0);
        std::vector<uint8_t> buf(3 * kBlock, 0);
        CHECK(cfs.read(fd, buf.data(), 3 * kBlock + 1024) == static_cast<int>(3 * kBlock));
        CHECK(buf == three);
        CHECK(cfs.read(fd, buf.data(), 128) == 0);  // at EOF
        CHECK(cfs.write(fd, buf.data(), 128) == -1);  // read-only
        CHECK(cfs.last_error() == CardFs::kEBadF);
        CHECK(cfs.lseek(fd, static_cast<int32_t>(kBlock - 128), CardFs::kSeekSet) == static_cast<int>(kBlock - 128));
        std::vector<uint8_t> small(384);
        CHECK(cfs.read(fd, small.data(), 384) == 384);
        CHECK(std::memcmp(small.data(), three.data() + kBlock - 128, 384) == 0);
        CHECK(cfs.lseek(fd, static_cast<int32_t>(kBlock), CardFs::kSeekCur) == static_cast<int>(2 * kBlock + 256));
        CHECK(cfs.read(fd, small.data(), 128) == 128);
        CHECK(std::memcmp(small.data(), three.data() + 2 * kBlock + 256, 128) == 0);
        CHECK(cfs.lseek(fd, -1, CardFs::kSeekSet) == -1);
        CHECK(cfs.lseek(fd, 0, 2) == -1);  // SEEK_END is not supported by the kernel
        CHECK(cfs.close(fd) == fd);

        // A third file for wildcard tests.
        fd = cfs.open("bu00:BESLUS-00000OTHER", CardFs::kFCreate);  // 0 blocks -> 1
        CHECK(fd >= 0);
        CHECK(cfs.close(fd) == fd);
        CHECK(cfs.free_blocks(0) == 10);

        // Listing.
        CHECK(cfs.firstfile("bu00:BISLPS-*", e));
        CHECK(std::string(e.name) == "BISLPS-03101DCB" && e.size == kBlock && e.head == 1 && e.attr == 0x51);
        CHECK(cfs.nextfile(e));
        CHECK(std::string(e.name) == "BISLPS-03101DCB2" && e.size == 3 * kBlock && e.head == 2);
        CHECK(!cfs.nextfile(e));
        CHECK(cfs.firstfile("bu00:B?SL*", e));
        CHECK(cfs.nextfile(e));
        CHECK(cfs.nextfile(e));
        CHECK(std::string(e.name) == "BESLUS-00000OTHER");
        CHECK(!cfs.nextfile(e));
        CHECK(cfs.firstfile("bu00:BISLPS-03101DCB?", e));
        CHECK(std::string(e.name) == "BISLPS-03101DCB2");
        CHECK(!cfs.nextfile(e));
        CHECK(cfs.firstfile("bu00:BISLPS-03101DCB", e));  // exact: no prefix match
        CHECK(e.head == 1);
        CHECK(!cfs.nextfile(e));
        CHECK(cfs.firstfile("bu00:", e));  // empty pattern lists everything
    }

    // Byte-level check of the directory frames, from the file on disk.
    {
        const auto img = image(path);
        CHECK(img.size() == MemoryCard::kSize);
        CHECK(img[0] == 'M' && img[1] == 'C' && checksum_ok(img.data()));
        for (uint32_t b = 1; b <= 15; ++b) CHECK(checksum_ok(dir(img, b)));
        const uint8_t* f1 = dir(img, 1);
        CHECK(f1[0] == 0x51 && u32(f1 + 4) == kBlock && u16(f1 + 8) == 0xFFFF);
        CHECK(std::memcmp(f1 + 0x0A, "BISLPS-03101DCB", 16) == 0);
        const uint8_t* f2 = dir(img, 2);
        const uint8_t* f3 = dir(img, 3);
        const uint8_t* f4 = dir(img, 4);
        CHECK(f2[0] == 0x51 && u32(f2 + 4) == 3 * kBlock && u16(f2 + 8) == 2);  // next = block 3 (minus 1)
        CHECK(f3[0] == 0x52 && u32(f3 + 4) == 0 && u16(f3 + 8) == 3 && f3[0x0A] == 0);
        CHECK(f4[0] == 0x53 && u32(f4 + 4) == 0 && u16(f4 + 8) == 0xFFFF);
        CHECK(dir(img, 5)[0] == 0x51);
        for (uint32_t b = 6; b <= 15; ++b) CHECK(dir(img, b)[0] == 0xA0 && u16(dir(img, b) + 8) == 0xFFFF);
        // Data lands in the blocks the chain names.
        const auto one = pattern(kBlock, 1);
        const auto three = pattern(3 * kBlock, 9);
        CHECK(std::memcmp(img.data() + 1 * kBlock, one.data(), kBlock) == 0);
        CHECK(std::memcmp(img.data() + 2 * kBlock, three.data(), 3 * kBlock) == 0);
    }

    // Reopen the image: saves persist. Rename, erase (frees blocks), reuse of deleted blocks.
    {
        MemoryCard card(path);
        MemoryCard card2(dir_path / "card2.mcd");
        CardFs cfs({&card, &card2});
        CardFs::DirEntry e{};
        CHECK(cfs.firstfile("bu10:*", e) == false && cfs.last_error() == CardFs::kENoEnt);  // present, empty

        CHECK(cfs.rename("bu00:BISLPS-03101DCB", "bu00:BISLPS-03101DCB9"));
        CHECK(!cfs.rename("bu00:BISLPS-03101DCB", "bu00:X"));
        CHECK(cfs.last_error() == CardFs::kENoEnt);
        CHECK(!cfs.rename("bu00:BISLPS-03101DCB9", "bu00:BISLPS-03101DCB2"));
        CHECK(cfs.last_error() == CardFs::kEExist);
        CHECK(cfs.firstfile("bu00:BISLPS-03101DCB9", e) && e.head == 1);

        const int held = cfs.open("bu00:BISLPS-03101DCB2", CardFs::kFRead);
        CHECK(held >= 0);
        CHECK(cfs.erase("bu00:BISLPS-03101DCB2"));
        CHECK(cfs.close(held) == -1);  // erase closed it
        CHECK(!cfs.erase("bu00:BISLPS-03101DCB2"));
        CHECK(cfs.last_error() == CardFs::kENoEnt);
        CHECK(cfs.free_blocks(0) == 13);
        auto img = image(path);
        CHECK(dir(img, 2)[0] == 0xA1 && dir(img, 3)[0] == 0xA2 && dir(img, 4)[0] == 0xA3);
        CHECK(checksum_ok(dir(img, 2)) && checksum_ok(dir(img, 3)) && checksum_ok(dir(img, 4)));
        const uint8_t* f1 = dir(img, 1);
        CHECK(std::memcmp(f1 + 0x0A, "BISLPS-03101DCB9", 17) == 0 && checksum_ok(f1));

        // A 4-block file takes the deleted blocks 2,3,4 plus the first fresh one (6).
        const int fd = cfs.open("bu00:BIG", CardFs::kFCreate | (4u << 16));
        CHECK(fd >= 0);
        const auto four = pattern(4 * kBlock, 3);
        CHECK(cfs.write(fd, four.data(), 4 * kBlock) == static_cast<int>(4 * kBlock));
        CHECK(cfs.lseek(fd, 0, CardFs::kSeekSet) == 0);
        std::vector<uint8_t> back(4 * kBlock);
        CHECK(cfs.read(fd, back.data(), 4 * kBlock) == static_cast<int>(4 * kBlock));
        CHECK(back == four);
        CHECK(cfs.close(fd) == fd);
        img = image(path);
        CHECK(dir(img, 2)[0] == 0x51 && u16(dir(img, 2) + 8) == 2);
        CHECK(dir(img, 4)[0] == 0x52 && u16(dir(img, 4) + 8) == 5);
        CHECK(dir(img, 6)[0] == 0x53 && u16(dir(img, 6) + 8) == 0xFFFF);
        CHECK(std::memcmp(img.data() + 6 * kBlock, four.data() + 3 * kBlock, kBlock) == 0);

        // Not enough space.
        CHECK(cfs.open("bu00:HUGE", CardFs::kFCreate | (15u << 16)) == -1);
        CHECK(cfs.last_error() == CardFs::kENoSpc);

        // Card removed at runtime.
        cfs.set_slot(1, nullptr);
        CHECK(cfs.open("bu10:X", CardFs::kFCreate) == -1 && cfs.last_error() == CardFs::kENoDev);

        // Format wipes it again.
        CHECK(cfs.format("bu00:"));
        CHECK(cfs.free_blocks(0) == 15);
        CHECK(!cfs.firstfile("bu00:*", e));
    }

    fs::remove_all(dir_path);
    std::puts("mcrd.card_fs: ok");
    return 0;
}
