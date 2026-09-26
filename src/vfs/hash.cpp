#include "vfs/hash.hpp"

namespace vfs {

std::string to_hex16(uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[value & 0xFu];
        value >>= 4;
    }
    return out;
}

bool from_hex16(std::string_view text, uint64_t& out) {
    if (text.size() != 16) return false;
    uint64_t value = 0;
    for (char ch : text) {
        value <<= 4;
        if (ch >= '0' && ch <= '9') value |= static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint64_t>(ch - 'A' + 10);
        else return false;
    }
    out = value;
    return true;
}

}  // namespace vfs
