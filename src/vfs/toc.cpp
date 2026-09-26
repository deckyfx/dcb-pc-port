// DRV table-of-contents reader. See toc.hpp for the verified format.

#include "vfs/toc.hpp"

#include <cctype>
#include <cstring>

namespace vfs {

namespace {

uint32_t load32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

bool looks_like_name(const uint8_t* p) {
    // 16 bytes: printable ASCII (or NUL padding), at least one alnum.
    bool alnum = false;
    for (int i = 0; i < 16; ++i) {
        const uint8_t c = p[i];
        if (c == 0) continue;
        if (c < 0x20 || c > 0x7E) return false;
        if (std::isalnum(c)) alnum = true;
    }
    return alnum;
}

}  // namespace

std::vector<TocEntry> parse_toc(const uint8_t* data, size_t size, size_t base) {
    std::vector<TocEntry> out;
    if (!data || base >= size) return out;
    // Bounded by the blob, not a fixed cap: real tables reach ~765 records.
    for (size_t off = base; off + 32 <= size; off += 32) {
        const uint8_t* e = data + off;
        if (std::memcmp(e, "\0\0\0\0", 4) == 0) break;  // zero entry ends the table
        const uint8_t kind = e[0];
        if (kind != 0x01 && kind != 0x80) break;  // unknown record kind
        if (!looks_like_name(e + 16)) break;
        TocEntry t;
        t.magic.assign(reinterpret_cast<const char*>(e), 4);
        t.sector = load32(e + 4);
        t.size = load32(e + 8);
        t.name.assign(reinterpret_cast<const char*>(e + 16), strnlen(reinterpret_cast<const char*>(e + 16), 16));
        t.is_group = (kind == 0x80);
        // Sanity: sectors must land inside any plausible DRV (< 2^21 sectors = 4 GiB).
        if (t.sector >= (1u << 21)) break;
        if (!t.is_group && t.size > (1u << 31)) break;
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace vfs
