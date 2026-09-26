// Save-state stream: round trips of every field kind, nested chunks, and rejection of states
// that do not match the reader (wrong tag, newer version, size mismatch, truncation, bad counts).

#include <psx/state.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

using psx::StateError;
using psx::StateReader;
using psx::StateWriter;
using psx::state_tag;

/// True if `fn` throws StateError whose message contains `needle`.
bool rejects(const std::function<void()>& fn, const char* needle) {
    try {
        fn();
    } catch (const StateError& e) {
        if (std::strstr(e.what(), needle) == nullptr) {
            std::fprintf(stderr, "unexpected message: %s (wanted '%s')\n", e.what(), needle);
            return false;
        }
        return true;
    }
    return false;
}

struct Pod {
    uint16_t a;
    uint16_t b;
    int32_t c;
    bool operator==(const Pod&) const = default;
};

std::vector<uint8_t> sample() {
    StateWriter w;
    w.begin(state_tag("OUTR"), 3);
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.boolean(true);
    w.pod(Pod{1, 2, -3});
    w.pod(std::array<int16_t, 3>{-1, 0, 1});
    w.begin(state_tag("INNR"), 1);
    w.vec(std::vector<uint32_t>{1, 2, 3});
    w.deque(std::deque<uint8_t>{9, 8});
    w.str("hello");
    w.map(std::map<uint32_t, uint32_t>{{10, 100}, {20, 200}});
    w.end();
    w.vec(std::vector<uint8_t>{});
    w.end();
    return w.take();
}

void test_round_trip() {
    const std::vector<uint8_t> data = sample();
    StateReader r(data);
    CHECK(r.begin(state_tag("OUTR"), 3) == 3);
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0xBEEF);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFull);
    CHECK(r.boolean());
    CHECK((r.get<Pod>() == Pod{1, 2, -3}));
    CHECK((r.get<std::array<int16_t, 3>>() == std::array<int16_t, 3>{-1, 0, 1}));
    CHECK(r.begin(state_tag("INNR"), 1) == 1);
    std::vector<uint32_t> v;
    r.vec(v, 16);
    CHECK((v == std::vector<uint32_t>{1, 2, 3}));
    std::deque<uint8_t> d;
    r.deque(d, 16);
    CHECK((d == std::deque<uint8_t>{9, 8}));
    std::string s;
    r.str(s, 16);
    CHECK(s == "hello");
    std::map<uint32_t, uint32_t> m;
    r.map(m, 16);
    CHECK((m == std::map<uint32_t, uint32_t>{{10, 100}, {20, 200}}));
    r.end();
    std::vector<uint8_t> empty{1};
    r.vec(empty, 16);
    CHECK(empty.empty());
    r.end();
    CHECK(r.at_end());

    // An older version is accepted (the caller decides how to read it).
    StateReader older(data);
    CHECK(older.begin(state_tag("OUTR"), 5) == 3);
}

void test_rejections() {
    const std::vector<uint8_t> data = sample();

    // Another chunk than expected.
    CHECK(rejects([&] { StateReader(data).begin(state_tag("GPU "), 3); }, "expected chunk 'GPU ', found 'OUTR'"));
    // A version newer than the reader knows.
    CHECK(rejects([&] { StateReader(data).begin(state_tag("OUTR"), 2); }, "version 3 is not supported"));
    // Size mismatch: a reader that stops early (a field removed without a version bump).
    CHECK(rejects(
        [&] {
            StateReader r(data);
            r.begin(state_tag("OUTR"), 3);
            r.u8();
            r.end();
        },
        "unread bytes"));
    // ...and one that reads too far (a field added without a version bump) hits the chunk end.
    CHECK(rejects(
        [&] {
            StateReader r(data);
            r.begin(state_tag("OUTR"), 3);
            r.begin(state_tag("INNR"), 1);  // skips nothing: OUTR starts with plain fields
        },
        "expected chunk 'INNR'"));
    {
        StateWriter w;
        w.begin(state_tag("TINY"), 1);
        w.u16(7);
        w.end();
        const std::vector<uint8_t> tiny = w.take();
        CHECK(rejects(
            [&] {
                StateReader r(tiny);
                r.begin(state_tag("TINY"), 1);
                r.u32();
            },
            "truncated data"));
    }
    // Truncated stream: the chunk claims more bytes than there are.
    {
        std::vector<uint8_t> cut(data.begin(), data.end() - 5);
        CHECK(rejects([&] { StateReader(cut).begin(state_tag("OUTR"), 3); }, "is truncated"));
        std::vector<uint8_t> header_only(data.begin(), data.begin() + 10);
        CHECK(rejects([&] { StateReader(header_only).begin(state_tag("OUTR"), 3); }, "missing chunk"));
    }
    // Version 0 is never valid.
    {
        StateWriter w;
        w.begin(state_tag("ZERO"), 0);
        w.end();
        const std::vector<uint8_t> zero = w.take();
        CHECK(rejects([&] { StateReader(zero).begin(state_tag("ZERO"), 1); }, "version 0"));
    }
    // Counts above the caller's limit, or larger than the remaining data, are rejected.
    {
        StateWriter w;
        w.vec(std::vector<uint32_t>{1, 2, 3, 4});
        const std::vector<uint8_t> four = w.take();
        CHECK(rejects(
            [&] {
                StateReader r(four);
                std::vector<uint32_t> v;
                r.vec(v, 3);
            },
            "out of range"));
        CHECK(rejects(
            [&] {
                StateReader r(four);
                std::vector<uint32_t> v;
                r.vec(v, 16, 5);
            },
            "expected 5"));
        std::vector<uint8_t> lying = four;
        lying[0] = 200;  // count 200, but only 16 bytes follow
        CHECK(rejects(
            [&] {
                StateReader r(lying);
                std::vector<uint32_t> v;
                r.vec(v, 1000);
            },
            "out of range"));
    }
    // Booleans are 0 or 1.
    {
        const std::vector<uint8_t> two{2};
        CHECK(rejects([&] { StateReader(two).boolean(); }, "bad boolean"));
    }
    // end() without begin().
    CHECK(rejects([&] { StateReader(data).end(); }, "without begin"));
}

void test_sizes_are_patched() {
    StateWriter w;
    w.begin(state_tag("SIZE"), 1);
    w.u32(1);
    w.u32(2);
    w.end();
    const std::vector<uint8_t>& d = w.data();
    CHECK(d.size() == 16 + 8);
    uint64_t size = 0;
    std::memcpy(&size, d.data() + 8, sizeof size);
    CHECK(size == 8);
}

}  // namespace

int main() {
    test_round_trip();
    test_rejections();
    test_sizes_are_patched();
    std::puts("state.stream: ok");
    return 0;
}
