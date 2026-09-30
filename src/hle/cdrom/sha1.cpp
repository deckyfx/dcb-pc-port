#include "cdrom/sha1.hpp"

#include <cstring>

namespace hle {

namespace {

uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

}  // namespace

void Sha1::reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    used_ = 0;
    length_ = 0;
}

void Sha1::compress(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
               static_cast<uint32_t>(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    length_ += len;
    if (used_ != 0) {
        const size_t take = len < 64 - used_ ? len : 64 - used_;
        std::memcpy(block_ + used_, p, take);
        used_ += take;
        p += take;
        len -= take;
        if (used_ < 64) return;
        compress(block_);
        used_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64) compress(p);  // whole blocks straight from the input
    if (len != 0) std::memcpy(block_, p, len);
    used_ = len;
}

Sha1::Digest Sha1::finish() {
    // Padding: 0x80, zeros up to 56 mod 64, then the message length in bits, big-endian.
    const uint64_t bits = length_ * 8;
    uint8_t tail[72] = {0x80};
    const size_t zeros = (used_ < 56 ? 56 : 120) - used_ - 1;
    for (int i = 0; i < 8; ++i) tail[1 + zeros + static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(tail, 1 + zeros + 8);
    Digest out{};
    for (size_t i = 0; i < 5; ++i)
        for (size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h_[i] >> (24 - 8 * j));
    return out;
}

std::string Sha1::to_hex(const Digest& digest) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(40);
    for (uint8_t b : digest) {
        s += kHex[b >> 4];
        s += kHex[b & 15];
    }
    return s;
}

std::string Sha1::hex_of(const void* data, size_t len) {
    Sha1 sha;
    sha.update(data, len);
    return to_hex(sha.finish());
}

}  // namespace hle
