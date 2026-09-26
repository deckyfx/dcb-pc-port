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

const char* toc_stop_name(TocStop stop) {
    switch (stop) {
        case TocStop::Terminator: return "zero-record terminator";
        case TocStop::EndOfBlob: return "end of blob";
        case TocStop::BadKind: return "unknown record kind";
        case TocStop::BadName: return "implausible name";
        case TocStop::BadSector: return "sector out of range";
        case TocStop::BadSize: return "size out of range";
    }
    return "?";
}

TocResult parse_toc_detailed(const uint8_t* data, size_t size, size_t base) {
    TocResult result;
    if (!data || base >= size) return result;
    // Bounded by the blob, not a fixed cap: real tables reach ~765 records.
    for (size_t off = base; off + 32 <= size; off += 32) {
        const uint8_t* e = data + off;
        auto stop = [&](TocStop s) {
            result.stop = s;
            result.stop_index = (off - base) / 32;
        };
        if (std::memcmp(e, "\0\0\0\0", 4) == 0) {
            stop(TocStop::Terminator);
            break;
        }
        const uint8_t kind = e[0];
        if (kind != 0x01 && kind != 0x80) {
            stop(TocStop::BadKind);
            break;
        }
        if (!looks_like_name(e + 16)) {
            stop(TocStop::BadName);
            break;
        }
        TocEntry t;
        t.magic.assign(reinterpret_cast<const char*>(e), 4);
        t.sector = load32(e + 4);
        t.size = load32(e + 8);
        t.name.assign(reinterpret_cast<const char*>(e + 16), strnlen(reinterpret_cast<const char*>(e + 16), 16));
        t.is_group = (kind == 0x80);
        // Sanity: sectors must land inside any plausible DRV (< 2^21 sectors = 4 GiB).
        if (t.sector >= (1u << 21)) {
            stop(TocStop::BadSector);
            break;
        }
        if (!t.is_group && t.size > (1u << 31)) {
            stop(TocStop::BadSize);
            break;
        }
        result.entries.push_back(std::move(t));
    }
    return result;
}

std::vector<TocEntry> parse_toc(const uint8_t* data, size_t size, size_t base) {
    return parse_toc_detailed(data, size, base).entries;
}

}  // namespace vfs
