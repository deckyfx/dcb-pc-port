// Memory search: value parsing / formatting, every filter, alignment per size, signed
// comparisons, and the classic narrowing workflow on a simulated RAM.

#include "trainer_cheats.hpp"
#include "trainer_search.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using namespace trainer;

std::vector<uint8_t> ram() { return std::vector<uint8_t>(kRamSize, 0); }

bool has(const MemorySearch& s, uint32_t off) {
    for (size_t i = 0; i < s.count(); ++i)
        if (s.offset(i) == off) return true;
    return false;
}

void test_values() {
    CHECK(parse_value("1234", ValueSize::U16) == 1234u);
    CHECK(parse_value(" 0x04d2 ", ValueSize::U16) == 0x04D2u);
    CHECK(parse_value("$FF", ValueSize::U8) == 0xFFu);
    CHECK(parse_value("-1", ValueSize::U8) == 0xFFu);
    CHECK(parse_value("-128", ValueSize::U8) == 0x80u);
    CHECK(!parse_value("-129", ValueSize::U8));
    CHECK(!parse_value("256", ValueSize::U8));
    CHECK(parse_value("65535", ValueSize::U16) == 0xFFFFu);
    CHECK(!parse_value("65536", ValueSize::U16));
    CHECK(parse_value("4294967295", ValueSize::U32) == 0xFFFFFFFFu);
    CHECK(!parse_value("4294967296", ValueSize::U32));
    CHECK(parse_value("-2147483648", ValueSize::U32) == 0x80000000u);
    CHECK(!parse_value("", ValueSize::U16));
    CHECK(!parse_value("-", ValueSize::U16));
    CHECK(!parse_value("0x", ValueSize::U16));
    CHECK(!parse_value("12a", ValueSize::U16));
    CHECK(!parse_value("0xG", ValueSize::U16));
    CHECK(format_value(0xFFFF, ValueSize::U16, false) == "65535");
    CHECK(format_value(0xFFFF, ValueSize::U16, true) == "-1");
    CHECK(format_value(0x80, ValueSize::U8, true) == "-128");
    CHECK(format_value(0xFFFFFFFF, ValueSize::U32, true) == "-1");

    auto r = ram();
    write_value(r.data(), 0x10, ValueSize::U32, 0x11223344);
    CHECK(r[0x10] == 0x44 && r[0x13] == 0x11);
    CHECK(read_value(r.data(), 0x10, ValueSize::U32) == 0x11223344);
    CHECK(read_value(r.data(), 0x11, ValueSize::U16) == 0x2233);
    CHECK(read_value(r.data(), 0x13, ValueSize::U8) == 0x11);
    write_value(r.data(), 0x20, ValueSize::U8, 0x1FF);  // truncated
    CHECK(r[0x20] == 0xFF && r[0x21] == 0);
}

void test_start_and_alignment() {
    auto r = ram();
    MemorySearch s;
    CHECK(!s.active() && s.count() == 0);
    s.start(r.data(), ValueSize::U8);
    CHECK(s.active() && s.count() == kRamSize);
    s.start(r.data(), ValueSize::U16);
    CHECK(s.count() == kRamSize / 2 && s.offset(1) == 2);
    s.start(r.data(), ValueSize::U32);
    CHECK(s.count() == kRamSize / 4 && s.offset(1) == 4);
    s.reset();
    CHECK(!s.active() && s.count() == 0);
}

void test_value_filters() {
    auto r = ram();
    write_value(r.data(), 0x100, ValueSize::U16, 500);
    write_value(r.data(), 0x200, ValueSize::U16, 700);
    write_value(r.data(), 0x301, ValueSize::U8, 1);  // odd byte: inside the 16-bit value at 0x300 = 256
    MemorySearch s;
    s.start(r.data(), ValueSize::U16);
    CHECK(s.filter(r.data(), SearchFilter::Greater, 255) == 3);
    CHECK(has(s, 0x100) && has(s, 0x200) && has(s, 0x300));
    CHECK(s.filter(r.data(), SearchFilter::Less, 700) == 2);
    CHECK(has(s, 0x100) && has(s, 0x300));
    CHECK(s.filter(r.data(), SearchFilter::NotEqual, 256) == 1);
    CHECK(s.offset(0) == 0x100);
    CHECK(s.filter(r.data(), SearchFilter::Equal, 500) == 1);
    CHECK(s.filter(r.data(), SearchFilter::Equal, 501) == 0);

    // Filtering without a started search starts one.
    MemorySearch fresh;
    CHECK(fresh.filter(r.data(), SearchFilter::Equal, 700) == 1 && fresh.offset(0) == 0x200);
}

void test_change_filters() {
    auto r = ram();
    MemorySearch s;
    s.start(r.data(), ValueSize::U8);
    r[0x10] = 5;  // increased
    r[0x20] = 9;  // increased, later decreased
    CHECK(s.filter(r.data(), SearchFilter::Changed) == 2);
    CHECK(s.previous(0) == 5 && s.previous(1) == 9);  // snapshot follows the filter
    r[0x20] = 3;
    CHECK(s.filter(r.data(), SearchFilter::Unchanged) == 1 && s.offset(0) == 0x10);

    s.start(r.data(), ValueSize::U8);
    r[0x10] = 6;
    r[0x20] = 2;
    r[0x30] = 1;
    CHECK(s.filter(r.data(), SearchFilter::Increased) == 2 && has(s, 0x10) && has(s, 0x30));
    s.start(r.data(), ValueSize::U8);
    r[0x10] = 4;
    r[0x30] = 7;
    CHECK(s.filter(r.data(), SearchFilter::Decreased) == 1 && s.offset(0) == 0x10);
}

void test_signed() {
    auto r = ram();
    write_value(r.data(), 0x40, ValueSize::U16, 0xFFFE);  // -2
    write_value(r.data(), 0x50, ValueSize::U16, 3);
    MemorySearch s;
    s.set_signed(true);
    s.start(r.data(), ValueSize::U16);
    CHECK(s.filter(r.data(), SearchFilter::Less, 0) == 1 && s.offset(0) == 0x40);
    MemorySearch u;
    u.start(r.data(), ValueSize::U16);
    CHECK(u.filter(r.data(), SearchFilter::Greater, 3) == 1 && u.offset(0) == 0x40);
    // Signed decrease: 1 -> -1 decreased.
    s.start(r.data(), ValueSize::U16);
    write_value(r.data(), 0x50, ValueSize::U16, 0xFFFF);
    CHECK(s.filter(r.data(), SearchFilter::Decreased) == 1 && s.offset(0) == 0x50);
}

/// The workflow the trainer is for: money 1200 -> 1150 -> 1300 among noise.
void test_workflow() {
    auto r = ram();
    for (uint32_t i = 0; i < kRamSize; i += 2) write_value(r.data(), i, ValueSize::U16, (i * 7919u) & 0xFFFF);
    const uint32_t money = 0x0B1234;
    write_value(r.data(), money, ValueSize::U16, 1200);
    MemorySearch s;
    s.start(r.data(), ValueSize::U16);
    s.filter(r.data(), SearchFilter::Equal, 1200);
    CHECK(has(s, money));
    write_value(r.data(), money, ValueSize::U16, 1150);
    s.filter(r.data(), SearchFilter::Decreased);
    write_value(r.data(), money, ValueSize::U16, 1300);
    s.filter(r.data(), SearchFilter::Equal, 1300);
    CHECK(s.count() == 1 && s.offset(0) == money);
}

}  // namespace

int main() {
    test_values();
    test_start_and_alignment();
    test_value_filters();
    test_change_filters();
    test_signed();
    test_workflow();
    std::puts("trainer search: all tests passed");
    return 0;
}
