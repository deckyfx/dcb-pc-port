#pragma once
// FNV-1a 64-bit content hashing: asset identity for the HD-replacement index.
// Reference: http://www.isthe.com/chongo/tech/comp/fnv/ (public domain algorithm).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vfs {

inline constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

/// Hash `len` bytes. The incremental form lets callers hash a VRAM rect row by row.
inline uint64_t fnv1a64(const void* data, size_t len, uint64_t hash = kFnvOffsetBasis) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string to_hex16(uint64_t value);
bool from_hex16(std::string_view text, uint64_t& out);

}  // namespace vfs
