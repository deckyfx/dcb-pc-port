// Partner editor: the slot model on a fake RAM (partner tables, game_data, card database, a deck),
// every refusal, a new partner, a swap, a replacement that moves the deck's partner, emptying the
// last slot, and the General tab rows that drive it.

#include "trainer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace trainer;

constexpr uint32_t kGameData = 0x800DF1C4u;
constexpr uint32_t kDb = 0x800CB1DCu;
constexpr int kVeemon = 0, kHawkmon = 1, kArmadillomon = 2, kGatomon = 3, kPatamon = 4, kWormmon = 5;

struct Fake {
    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize, 0);
    uint32_t off(uint32_t a) const { return a & (kRamSize - 1); }
    uint8_t u8(uint32_t a) const { return ram[off(a)]; }
    uint16_t u16(uint32_t a) const { return static_cast<uint16_t>(u8(a) | u8(a + 1) << 8); }
    uint32_t u32(uint32_t a) const { return u16(a) | static_cast<uint32_t>(u16(a + 2)) << 16; }
    void w8(uint32_t a, uint8_t v) { ram[off(a)] = v; }
    void w16(uint32_t a, uint16_t v) { w8(a, static_cast<uint8_t>(v)), w8(a + 1, static_cast<uint8_t>(v >> 8)); }
    void w32(uint32_t a, uint32_t v) { w16(a, static_cast<uint16_t>(v)), w16(a + 2, static_cast<uint16_t>(v >> 16)); }
    static uint32_t slot(int s) { return kGameData + kSlotsOffset + static_cast<uint32_t>(s) * kSlotSize; }
    static uint32_t entry(int d, int e) { return kGameData + kDecksOffset + static_cast<uint32_t>(d) * kDeckSize + 0x10 + static_cast<uint32_t>(e) * 8; }
    static uint32_t record(int card) { return kDb + static_cast<uint32_t>(card) * kCardRecordSize; }
    bool flag(int reg) const {
        const FlagBit b = city_flag_bit(reg);
        return (u8(kGameData + kCityFlagsOffset + b.byte) & b.mask) != 0;
    }
    void set_flag(int reg) {
        const FlagBit b = city_flag_bit(reg);
        w8(kGameData + kCityFlagsOffset + b.byte, static_cast<uint8_t>(u8(kGameData + kCityFlagsOffset + b.byte) | b.mask));
    }
    void deck_entry(int d, int e, uint8_t kind, uint8_t index, uint16_t card, uint32_t ptr) {
        w8(entry(d, e), kind);
        w8(entry(d, e) + 1, index);
        w16(entry(d, e) + 2, card);
        w32(entry(d, e) + 4, ptr);
    }

    /// A save in the city: Veemon Lv 7 in slot 0, a deck with Veemon, a plain Hawkmon card, an
    /// option card and card 5; slots 1 and 2 hold leftovers, as in real saves.
    Fake() {
        for (int i = 0; i < kPartnerKinds; ++i) {
            w8(kPartnerCardsAddr + static_cast<uint32_t>(i), kPartners[static_cast<size_t>(i)].card);
            for (int n = 0; n < 3; ++n)
                w8(kDigimentalCardsAddr + static_cast<uint32_t>(i * 3 + n), kPartners[static_cast<size_t>(i)].digimentals[static_cast<size_t>(n)]);
        }
        w32(kGameDataPtrAddr, kGameData);
        w32(kCardDbPtrAddr, kDb);
        for (int c = 0; c < 301; ++c)
            for (uint32_t i = 0; i < kCardRecordSize; ++i) w8(record(c) + i, static_cast<uint8_t>(c * 7 + i));
        // Patamon (183): no +0x58 value (the rebuild's default 100), a negative first attack.
        w16(record(183) + 0x58, 0);
        w16(record(183) + 0x20, 0xFFF6);
        w8(record(183) + 0xE4, 1);
        // Wormmon (187): type 6, so no +0x58 value at all.
        w8(record(187) + 0xE4, 6);
        for (uint32_t i = 0; i < 3 * kSlotSize; ++i) w8(slot(0) + i, static_cast<uint8_t>(i * 13 + 5));
        w8(slot(1) + 0x278, 0);
        w8(slot(2) + 0x278, 0);
        const uint32_t s0 = slot(0);
        w32(s0 + 0x268, record(175));
        w32(s0 + 0x26C, record(175));
        w8(s0 + 0x278, 175);
        w8(s0 + 0x279, 7);
        w16(s0 + 0x27A, 68);
        w8(s0 + 0x27C, 19), w8(s0 + 0x27D, 23), w8(s0 + 0x27E, 30);
        set_flag(294);
        w8(kGameData + kCollectionOffset + 175, 0x51);
        w8(kGameData + kDecksOffset, 1);  // deck 1 used, decks 2-3 not
        deck_entry(0, 0, 0, 175, 175, slot(0));
        deck_entry(0, 1, 0, 182, 182, record(182));
        deck_entry(0, 2, 1, 99, 290, 0x800DEBF6u);
        deck_entry(0, 3, 0, 5, 5, record(5));
    }
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void test_model() {
    CHECK(partner_of_card(175) == kVeemon && partner_of_card(187) == kWormmon && partner_of_card(176) == -1);
    CHECK(kPartners[kGatomon].flag == 307 && kPartners[kPatamon].flag == 304);

    {  // no game running / no save loaded
        std::vector<uint8_t> empty(kRamSize, 0);
        CHECK(!read_partners(empty.data()).ok && contains(read_partners(empty.data()).error, "not found"));
        Fake f;
        f.w32(kGameDataPtrAddr, 0);
        CHECK(contains(read_partners(f.ram.data()).error, "load a save"));
        CHECK(!set_partner(f.ram.data(), 1, kPatamon).ok);
        Fake g;
        g.w8(Fake::slot(0) + 0x278, 0);
        CHECK(contains(read_partners(g.ram.data()).error, "no partner yet"));
        Fake h;
        h.w8(Fake::slot(1) + 0x278, 12);
        CHECK(contains(read_partners(h.ram.data()).error, "holds card 12"));
    }

    Fake f;
    const PartnerState st = read_partners(f.ram.data());
    CHECK(st.ok && st.slots[0].partner == kVeemon && st.slots[0].level == 7 && st.slots[0].exp == 68);
    CHECK(st.slots[1].partner == -1 && st.slots[2].partner == -1);

    // Refusals leave RAM alone.
    const std::vector<uint8_t> before = f.ram;
    CHECK(contains(set_partner(f.ram.data(), 0, kVeemon).message, "already"));
    CHECK(contains(set_partner(f.ram.data(), 0, -1).message, "cannot be empty"));
    CHECK(contains(set_partner(f.ram.data(), 2, kGatomon).message, "fill Partner 2 first"));
    CHECK(contains(set_partner(f.ram.data(), 1, kVeemon).message, "cannot be empty"));  // a swap would empty slot 1
    CHECK(contains(set_partner(f.ram.data(), 1, kHawkmon).message, "plain Hawkmon card is in deck 1"));
    CHECK(!set_partner(f.ram.data(), 3, kGatomon).ok && !set_partner(f.ram.data(), 1, 6).ok);
    CHECK(f.ram == before);

    // A new partner in slot 2: partner_add's record, rebuilt from the database.
    PartnerChange r = set_partner(f.ram.data(), 1, kPatamon);
    CHECK(r.ok && contains(r.message, "Partner 2: Patamon Lv 1"));
    const uint32_t s1 = Fake::slot(1);
    CHECK(f.u8(s1 + 0x278) == 183 && f.u8(s1 + 0x279) == 1 && f.u16(s1 + 0x27A) == 0);
    CHECK(f.u32(s1 + 0x268) == Fake::record(183) && f.u32(s1 + 0x26C) == Fake::record(183));
    for (uint32_t i = 0; i < 3; ++i) CHECK(f.u8(s1 + 0x27C + i) == 0xFF && f.u8(s1 + 0x27F + i) == 0);
    CHECK(f.u8(s1 + 0x282) == 0 && f.u8(s1 + 0x283) == 0 && f.u8(s1 + 0x284) == 0);
    for (uint32_t i = 0; i < 8; i += 2) CHECK(f.u16(s1 + 0x270 + i) == 0);
    for (const uint32_t copy : {0u, 0x134u}) {
        CHECK(f.u16(s1 + copy + 0x58) == 100 && f.u16(s1 + copy + 0x20) == 0);
        for (uint32_t i = 0; i < kCardRecordSize; ++i)
            if (i != 0x58 && i != 0x59 && i != 0x20 && i != 0x21) CHECK(f.u8(s1 + copy + i) == f.u8(Fake::record(183) + i));
    }
    CHECK(f.flag(304) && f.flag(294) && f.u8(kGameData + kCollectionOffset + 183) == 0xF1);
    CHECK(f.u32(Fake::entry(0, 0) + 4) == Fake::slot(0) && f.u32(Fake::entry(0, 1) + 4) == Fake::record(182));
    CHECK(f.u32(Fake::entry(0, 2) + 4) == 0x800DEBF6u && f.u32(Fake::entry(0, 3) + 4) == Fake::record(5));

    // Patamon to partner 1: the records are swapped, the deck follows Veemon to slot 2.
    std::vector<uint8_t> veemon(f.ram.begin() + (Fake::slot(0) & (kRamSize - 1)), f.ram.begin() + (Fake::slot(0) & (kRamSize - 1)) + kSlotSize);
    r = set_partner(f.ram.data(), 0, kPatamon);
    CHECK(r.ok && contains(r.message, "swapped"));
    CHECK(f.u8(Fake::slot(0) + 0x278) == 183 && f.u8(s1 + 0x278) == 175 && f.u8(s1 + 0x279) == 7 && f.u8(s1 + 0x27D) == 23);
    CHECK(std::equal(veemon.begin(), veemon.end(), f.ram.begin() + (s1 & (kRamSize - 1))));
    CHECK(f.u32(Fake::entry(0, 0) + 4) == s1 && f.flag(294) && f.flag(304));

    // Wormmon replaces Veemon (in the deck): the deck's partner becomes Wormmon, the flags follow.
    r = set_partner(f.ram.data(), 1, kWormmon);
    CHECK(r.ok && contains(r.message, "Wormmon Lv 1 (was Veemon)"));
    CHECK(f.u8(s1 + 0x278) == 187 && f.u8(s1 + 0x279) == 1 && f.u16(s1 + 0x58) == 0);  // type 6: no value
    CHECK(f.u8(Fake::entry(0, 0)) == 0 && f.u8(Fake::entry(0, 0) + 1) == 187 && f.u16(Fake::entry(0, 0) + 2) == 187);
    CHECK(f.u32(Fake::entry(0, 0) + 4) == s1);
    CHECK(!f.flag(294) && f.flag(310) && f.flag(304));
    CHECK(f.u8(kGameData + kCollectionOffset + 175) == 0x40 && f.u8(kGameData + kCollectionOffset + 187) == 0xF1);

    // A third partner, then empty the last slot again; a partner in a deck cannot be removed.
    CHECK(set_partner(f.ram.data(), 2, kArmadillomon).ok && f.flag(301));
    CHECK(contains(set_partner(f.ram.data(), 1, -1).message, "empty the last partner first"));
    r = set_partner(f.ram.data(), 2, -1);
    CHECK(r.ok && contains(r.message, "Armadillomon removed"));
    CHECK(f.u8(Fake::slot(2) + 0x278) == 0 && !f.flag(301) && f.u8(kGameData + kCollectionOffset + 190) == 0x40);
    CHECK(contains(set_partner(f.ram.data(), 1, -1).message, "Wormmon is in deck 1"));
    const PartnerState end = read_partners(f.ram.data());
    CHECK(end.ok && end.slots[0].partner == kPatamon && end.slots[1].partner == kWormmon && end.slots[2].partner == -1);
}

void test_panel_rows() {
    Fake f;
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_partners";
    fs::remove_all(dir);
    Trainer t(f.ram.data(), dir / "cheats.txt");
    t.load();
    t.set_open(true);  // the selectors start at the slots' partners
    CHECK(t.partner_choices()[0] == kVeemon && t.partner_choices()[1] == -1 && t.partner_choices()[2] == -1);
    std::string screen;
    for (const Line& l : t.render(kPanelCols, kPanelRows)) screen += l.text + "\n";
    CHECK(contains(screen, "[General]") && contains(screen, "Partner 1  < Veemon       >  now Veemon Lv 7"));
    CHECK(contains(screen, "Partner 3  < (empty)      >  now (empty)"));
    for (int i = 0; i < 4; ++i) t.key(Key::Down);  // 3 toggles, then partner 2
    t.key(Key::Right);  // Veemon: a swap that would empty partner 1
    CHECK(t.partner_choices()[1] == kVeemon);
    t.key(Key::Enter);
    CHECK(contains(t.status(), "cannot be empty") && f.u8(Fake::slot(1) + 0x278) == 0);
    t.key(Key::Left);
    t.key(Key::Left);  // wraps to Wormmon
    CHECK(t.partner_choices()[1] == kWormmon);
    t.key(Key::Left);
    t.key(Key::Left);
    t.key(Key::Left);  // Armadillomon
    t.text(" ");
    CHECK(contains(t.status(), "Partner 2: Armadillomon Lv 1") && f.u8(Fake::slot(1) + 0x278) == 190);
    screen.clear();
    for (const Line& l : t.render(kPanelCols, kPanelRows)) screen += l.text + "\n";
    CHECK(contains(screen, "now Armadillomon Lv 1"));
    CHECK(!t.dirty());  // nothing for the cheat file
    for (int cols : {20, 38, 64})
        for (int rows : {12, 22, 30}) {
            const std::vector<Line> lines = t.render(cols, rows);
            CHECK(static_cast<int>(lines.size()) <= rows);
            for (const Line& l : lines) CHECK(static_cast<int>(l.text.size()) <= cols);
        }
    // Without a save the rows say why and Enter only reports it.
    std::vector<uint8_t> empty(kRamSize, 0);
    Trainer none(empty.data(), dir / "cheats.txt");
    none.set_open(true);
    screen.clear();
    for (const Line& l : none.render(kPanelCols, kPanelRows)) screen += l.text + "\n";
    CHECK(contains(screen, "Partners: partner tables not found"));
    for (int i = 0; i < 3; ++i) none.key(Key::Down);  // 3 toggles, then partner 1
    none.key(Key::Enter);
    CHECK(contains(none.status(), "not found"));
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_model();
    test_panel_rows();
    std::puts("trainer partners: all tests passed");
    return 0;
}
