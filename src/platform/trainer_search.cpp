// Trainer memory search (see trainer_search.hpp).

#include "trainer_search.hpp"

#include "trainer_cheats.hpp"  // kRamSize

#include <cctype>
#include <cstring>

namespace trainer {

namespace {

uint32_t mask_of(ValueSize size) {
    switch (size) {
    case ValueSize::U8: return 0xFFu;
    case ValueSize::U16: return 0xFFFFu;
    case ValueSize::U32: return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

int64_t sign_extend(uint32_t v, ValueSize size) {
    switch (size) {
    case ValueSize::U8: return static_cast<int8_t>(static_cast<uint8_t>(v));
    case ValueSize::U16: return static_cast<int16_t>(static_cast<uint16_t>(v));
    case ValueSize::U32: return static_cast<int32_t>(v);
    }
    return v;
}

}  // namespace

const char* filter_name(SearchFilter f) {
    switch (f) {
    case SearchFilter::Equal: return "= value";
    case SearchFilter::NotEqual: return "!= value";
    case SearchFilter::Greater: return "> value";
    case SearchFilter::Less: return "< value";
    case SearchFilter::Changed: return "changed";
    case SearchFilter::Unchanged: return "unchanged";
    case SearchFilter::Increased: return "increased";
    case SearchFilter::Decreased: return "decreased";
    }
    return "?";
}

bool filter_needs_value(SearchFilter f) {
    return f == SearchFilter::Equal || f == SearchFilter::NotEqual || f == SearchFilter::Greater ||
           f == SearchFilter::Less;
}

uint32_t read_value(const uint8_t* ram, uint32_t offset, ValueSize size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(size); ++i)
        v |= static_cast<uint32_t>(ram[(offset + i) & (kRamSize - 1)]) << (8 * i);
    return v;
}

void write_value(uint8_t* ram, uint32_t offset, ValueSize size, uint32_t value) {
    for (unsigned i = 0; i < static_cast<unsigned>(size); ++i)
        ram[(offset + i) & (kRamSize - 1)] = static_cast<uint8_t>(value >> (8 * i));
}

std::optional<uint32_t> parse_value(std::string_view text, ValueSize size) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    bool hex = false, negative = false;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        hex = true;
        text.remove_prefix(2);
    } else if (text.size() > 1 && text[0] == '$') {
        hex = true;
        text.remove_prefix(1);
    } else if (text.size() > 1 && text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (text.size() > 10) return std::nullopt;
    uint64_t v = 0;
    for (const char c : text) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return std::nullopt;
        v = v * (hex ? 16u : 10u) + static_cast<uint64_t>(d);
    }
    const uint64_t mask = mask_of(size);
    if (negative) {
        // Down to the most negative value of the size.
        if (v > (mask >> 1) + 1) return std::nullopt;
        return static_cast<uint32_t>((0 - v) & mask);
    }
    if (v > mask) return std::nullopt;
    return static_cast<uint32_t>(v);
}

std::string format_value(uint32_t value, ValueSize size, bool is_signed) {
    value &= mask_of(size);
    return is_signed ? std::to_string(sign_extend(value, size)) : std::to_string(value);
}

void MemorySearch::start(const uint8_t* ram, ValueSize size) {
    size_ = size;
    snapshot_.assign(ram, ram + kRamSize);
    const uint32_t step = static_cast<uint32_t>(size);
    candidates_.clear();
    candidates_.reserve(kRamSize / step);
    for (uint32_t off = 0; off < kRamSize; off += step) candidates_.push_back(off);
}

void MemorySearch::reset() {
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    candidates_.clear();
    candidates_.shrink_to_fit();
}

int64_t MemorySearch::as_number(uint32_t v) const {
    return signed_ ? sign_extend(v, size_) : static_cast<int64_t>(v & mask_of(size_));
}

bool MemorySearch::passes(uint32_t now, uint32_t before, SearchFilter f, uint32_t value) const {
    const int64_t n = as_number(now), b = as_number(before), v = as_number(value);
    switch (f) {
    case SearchFilter::Equal: return n == v;
    case SearchFilter::NotEqual: return n != v;
    case SearchFilter::Greater: return n > v;
    case SearchFilter::Less: return n < v;
    case SearchFilter::Changed: return n != b;
    case SearchFilter::Unchanged: return n == b;
    case SearchFilter::Increased: return n > b;
    case SearchFilter::Decreased: return n < b;
    }
    return false;
}

size_t MemorySearch::filter(const uint8_t* ram, SearchFilter f, uint32_t value) {
    if (!active()) start(ram, size_);
    size_t kept = 0;
    for (const uint32_t off : candidates_) {
        const uint32_t now = read_value(ram, off, size_);
        const uint32_t before = read_value(snapshot_.data(), off, size_);
        if (passes(now, before, f, value)) candidates_[kept++] = off;
    }
    candidates_.resize(kept);
    std::memcpy(snapshot_.data(), ram, kRamSize);
    return kept;
}

}  // namespace trainer
