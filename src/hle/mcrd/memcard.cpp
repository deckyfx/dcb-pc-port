#include "mcrd/memcard.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace hle {

namespace {

void checksum(uint8_t* frame) {
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; ++i) x ^= frame[i];
    frame[0x7F] = x;
}

}  // namespace

MemoryCard::MemoryCard(std::filesystem::path path) : path_(std::move(path)), data_(kSize, 0) {
    std::ifstream in(path_, std::ios::binary);
    if (in) {
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        if (bytes.size() == kSize) {
            data_ = std::move(bytes);
            return;
        }
        std::fprintf(stderr, "[mcrd] %s is not a 128 KB card image; using a blank card\n", path_.string().c_str());
    }
    format();
}

void MemoryCard::format() {
    std::fill(data_.begin(), data_.end(), 0);
    uint8_t* f = data_.data();
    // Frame 0: header.
    f[0] = 'M';
    f[1] = 'C';
    checksum(f);
    // Frames 1-15: directory, all blocks free.
    for (uint32_t i = 1; i <= 15; ++i) {
        uint8_t* d = f + i * kFrameSize;
        d[0] = 0xA0;  // free, freshly formatted
        d[8] = d[9] = 0xFF;  // no next block
        checksum(d);
    }
    // Frames 16-35: broken-sector list, all unused.
    for (uint32_t i = 16; i <= 35; ++i) {
        uint8_t* b = f + i * kFrameSize;
        std::memset(b, 0xFF, 4);
        b[8] = b[9] = 0xFF;
        checksum(b);
    }
    // Frame 63: write-test frame mirrors the header.
    std::memcpy(f + 63 * kFrameSize, f, kFrameSize);
}

bool MemoryCard::read_frame(uint32_t frame, uint8_t* out) const {
    if (frame >= kFrames) return false;
    std::memcpy(out, data_.data() + frame * kFrameSize, kFrameSize);
    return true;
}

bool MemoryCard::write_frame(uint32_t frame, const uint8_t* in) {
    if (frame >= kFrames) return false;
    std::memcpy(data_.data() + frame * kFrameSize, in, kFrameSize);
    save();
    return true;
}

void MemoryCard::save() const {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    if (!out) std::fprintf(stderr, "[mcrd] cannot write %s\n", path_.string().c_str());
}

}  // namespace hle
