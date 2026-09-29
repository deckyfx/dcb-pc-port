// Deck record editor: the model on a fake RAM (game_data, used/unused decks), every refusal,
// clamping, and the General tab rows that drive it.

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

struct Fake {
    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize, 0);
    static uint32_t off(uint32_t a) { return a & (kRamSize - 1); }
    uint8_t u8(uint32_t a) const { return ram[off(a)]; }
    uint16_t u16(uint32_t a) const { return static_cast<uint16_t>(u8(a) | u8(a + 1) << 8); }
    uint32_t u32(uint32_t a) const { return u16(a) | static_cast<uint32_t>(u16(a + 2)) << 16; }
    void w8(uint32_t a, uint8_t v) { ram[off(a)] = v; }
    void w16(uint32_t a, uint16_t v) { w8(a, static_cast<uint8_t>(v)), w8(a + 1, static_cast<uint8_t>(v >> 8)); }
    void w32(uint32_t a, uint32_t v) { w16(a, static_cast<uint16_t>(v)), w16(a + 2, static_cast<uint16_t>(v >> 16)); }
    static uint32_t deck(int d) { return kGameData + kDecksOffset + static_cast<uint32_t>(d) * kDeckSize; }

    Fake() {
        w32(kGameDataPtrAddr, kGameData);
        w8(deck(0), 1);
        w16(deck(0) + 0x106, 12);
        w16(deck(0) + 0x108, 3);
        // Decks 2-3 unused.
    }
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void test_model() {
    {  // no save loaded
        std::vector<uint8_t> empty(kRamSize, 0);
        CHECK(!read_records(empty.data()).ok && contains(read_records(empty.data()).error, "load a save"));
        CHECK(!set_record(empty.data(), 0, 5, 5).ok);
        Fake f;
        f.w32(kGameDataPtrAddr, 0);
        CHECK(!read_records(f.ram.data()).ok);
    }

    Fake f;
    RecordState st = read_records(f.ram.data());
    CHECK(st.ok && st.decks[0].used && st.decks[0].wins == 12 && st.decks[0].losses == 3);
    CHECK(!st.decks[1].used && st.decks[1].wins == 0 && st.decks[1].losses == 0);
    CHECK(!st.decks[2].used);

    // Refusals leave RAM alone.
    const std::vector<uint8_t> before = f.ram;
    CHECK(!set_record(f.ram.data(), 1, 5, 5).ok && contains(set_record(f.ram.data(), 1, 5, 5).message, "not used"));
    CHECK(!set_record(f.ram.data(), 3, 5, 5).ok && !set_record(f.ram.data(), -1, 5, 5).ok);
    CHECK(f.ram == before);

    // A write, clamped at the game's cap (999 kept).
    RecordChange r = set_record(f.ram.data(), 0, 2000, 7);
    CHECK(r.ok && !contains(r.message, "2000"));  // clamped: the message shows the kept value
    CHECK(f.u16(Fake::deck(0) + 0x106) == 999 && f.u16(Fake::deck(0) + 0x108) == 7);
    CHECK(contains(r.message, "999W-7L") && contains(r.message, "1006 battles"));
    r = set_record(f.ram.data(), 0, 0, 0);
    CHECK(r.ok && f.u16(Fake::deck(0) + 0x106) == 0 && f.u16(Fake::deck(0) + 0x108) == 0);
}

void test_panel_rows() {
    Fake f;
    const fs::path dir = fs::temp_directory_path() / "dcb_test_trainer_records";
    fs::remove_all(dir);
    Trainer t(f.ram.data(), dir / "cheats.txt");
    t.load();
    t.set_open(true);
    std::string screen;
    for (const Line& l : t.render(kPanelCols, kPanelRows)) screen += l.text + "\n";
    CHECK(contains(screen, "Deck 1 wins"));
    CHECK(contains(screen, "Deck 1 losses"));
    CHECK(contains(screen, "now W12-L3 (15 battles)"));
    // Walk down to Deck 1 wins: partners (3 rows) are above the 6 record rows.
    t.key(Key::End);  // last row: Deck 3 losses
    for (int i = 0; i < 5; ++i) t.key(Key::Up);  // Deck 1 wins
    t.key(Key::Right);
    t.key(Key::Right);
    t.key(Key::Enter);  // 12 -> 14, written once
    CHECK(contains(t.status(), "Deck 1: 14W-3L") && f.u16(Fake::deck(0) + 0x106) == 14);
    t.key(Key::Down);  // Deck 1 losses
    t.key(Key::PageDown);
    t.key(Key::Enter);  // 3 -> 0 (clamped, not negative)
    CHECK(f.u16(Fake::deck(0) + 0x108) == 0);
    CHECK(!t.dirty());  // nothing for the cheat file
    // Without a save the rows say why and Enter only reports it.
    std::vector<uint8_t> empty(kRamSize, 0);
    Trainer none(empty.data(), dir / "cheats.txt");
    none.set_open(true);
    screen.clear();
    for (const Line& l : none.render(kPanelCols, kPanelRows)) screen += l.text + "\n";
    CHECK(contains(screen, "Deck records: no game data yet"));
    none.key(Key::End);
    none.key(Key::Enter);
    CHECK(contains(none.status(), "load a save"));
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_model();
    test_panel_rows();
    std::puts("trainer records: all tests passed");
    return 0;
}
