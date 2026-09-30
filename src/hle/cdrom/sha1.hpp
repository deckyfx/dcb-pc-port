#pragma once
// Streaming SHA-1 (FIPS 180-4), for checking disc dumps against the redump.org database
// (hle::import::verify_dump). Not for anything security-related: redump publishes SHA-1, and
// here it only tells a good dump from a damaged or modified one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hle {

class Sha1 {
public:
    using Digest = std::array<uint8_t, 20>;

    Sha1() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    /// The digest of everything fed so far; reset() before feeding the object again.
    Digest finish();

    /// Lower-case hex, as redump.org lists it.
    static std::string to_hex(const Digest& digest);
    /// One-shot convenience: the hex digest of `len` bytes.
    static std::string hex_of(const void* data, size_t len);

private:
    uint32_t h_[5] = {};
    uint8_t block_[64] = {};
    size_t used_ = 0;      ///< bytes waiting in block_
    uint64_t length_ = 0;  ///< total bytes fed

    void compress(const uint8_t* block);
};

}  // namespace hle
