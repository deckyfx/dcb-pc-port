#pragma once
// Save-state serialisation: a flat little-endian byte stream of tagged, versioned, sized chunks.
//
//     chunk := tag (4 bytes) | version (u32) | size (u64) | payload (size bytes)
//
// Each class that holds guest state writes its own chunk (save_state / load_state) with explicit
// fields, never a raw copy of an object that holds pointers. A reader checks the tag and the
// version of every chunk, never reads past a chunk's end, and requires each chunk to be consumed
// exactly: a layout change without a version bump is caught instead of misreading the rest.
// Every violation throws StateError. States are only valid in the process that made them
// (see docs/HOST_MAIN_LOOP.md, "Save states").

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace psx {

/// A state that cannot be read: wrong tag, unsupported version, truncated or oversized data.
class StateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Four-character chunk tag, e.g. tag("GPU ").
constexpr uint32_t state_tag(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}

/// Plain values that are copied byte for byte: scalars, enums, and arrays/structs of them.
template <class T>
concept StatePod = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

class StateWriter {
public:
    /// Open a chunk; every begin() needs its end().
    void begin(uint32_t tag, uint32_t version);
    /// Close the innermost chunk (writes its size).
    void end();

    void bytes(const void* data, size_t size);
    template <StatePod T>
    void pod(const T& value) {
        bytes(&value, sizeof value);
    }
    void u8(uint8_t v) { pod(v); }
    void u16(uint16_t v) { pod(v); }
    void u32(uint32_t v) { pod(v); }
    void u64(uint64_t v) { pod(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void size(size_t v) { u64(static_cast<uint64_t>(v)); }

    template <StatePod T>
    void vec(const std::vector<T>& v) {
        size(v.size());
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
    template <StatePod T>
    void deque(const std::deque<T>& d) {
        size(d.size());
        for (const T& x : d) pod(x);
    }
    void str(const std::string& s) {
        size(s.size());
        bytes(s.data(), s.size());
    }
    template <StatePod K, StatePod V>
    void map(const std::map<K, V>& m) {
        size(m.size());
        for (const auto& [k, v] : m) {
            pod(k);
            pod(v);
        }
    }

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
    std::vector<size_t> open_;  ///< offsets of the size fields of open chunks
};

class StateReader {
public:
    explicit StateReader(std::span<const uint8_t> data) : data_(data), limit_(data.size()) {}

    /// Enter the next chunk, which must be `tag` with a version in [1, max_version]. Returns
    /// the version found (older layouts can be read by the caller if it supports them).
    uint32_t begin(uint32_t tag, uint32_t max_version);
    /// Leave the innermost chunk; it must have been read to its last byte.
    void end();

    void bytes(void* out, size_t size);
    template <StatePod T>
    void pod(T& value) {
        bytes(&value, sizeof value);
    }
    template <StatePod T>
    T get() {
        T v{};
        pod(v);
        return v;
    }
    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    bool boolean();
    /// A count, rejected above `max` (and above what the chunk can still hold at `unit` bytes each).
    size_t size(size_t max, size_t unit = 1);

    /// Read a vector of at most `max` elements; `exact` (if not SIZE_MAX) requires that count.
    template <StatePod T>
    void vec(std::vector<T>& v, size_t max, size_t exact = SIZE_MAX) {
        const size_t n = size(max, sizeof(T));
        if (exact != SIZE_MAX && n != exact) fail("vector has " + std::to_string(n) + " elements, expected " + std::to_string(exact));
        v.resize(n);
        if (n) bytes(v.data(), n * sizeof(T));
    }
    template <StatePod T>
    void deque(std::deque<T>& d, size_t max) {
        const size_t n = size(max, sizeof(T));
        d.clear();
        for (size_t i = 0; i < n; ++i) d.push_back(get<T>());
    }
    void str(std::string& s, size_t max);
    template <StatePod K, StatePod V>
    void map(std::map<K, V>& m, size_t max) {
        const size_t n = size(max, sizeof(K) + sizeof(V));
        m.clear();
        for (size_t i = 0; i < n; ++i) {
            const K k = get<K>();
            m[k] = get<V>();
        }
    }

    /// Bytes left in the innermost chunk (or the whole stream at top level).
    size_t remaining() const { return limit_ - pos_; }
    bool at_end() const { return pos_ == data_.size(); }

    [[noreturn]] void fail(const std::string& why) const;

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
    size_t limit_;
    struct Open {
        uint32_t tag;
        size_t end, outer_limit;
    };
    std::vector<Open> open_;
};

}  // namespace psx
