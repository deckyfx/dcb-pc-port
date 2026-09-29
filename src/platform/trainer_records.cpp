// Deck record editor (see trainer_records.hpp).

#include "trainer_records.hpp"

#include "trainer_cheats.hpp"  // kRamSize
#include "trainer_partners.hpp"  // kDecksOffset, kDeckSize, kGameDataPtrAddr

#include <algorithm>
#include <cstdio>

namespace trainer {

namespace record_field {
inline constexpr uint32_t kCounter = 0x104;  ///< u16: capped with the record by deck_store; not written by battle_result, meaning unknown
inline constexpr uint32_t kWins = 0x106;     ///< u16: +1 on the winner's deck after a battle
inline constexpr uint32_t kLosses = 0x108;   ///< u16: +1 on the loser's deck after a battle
}  // namespace record_field

namespace {

struct Ram {
    uint8_t* p;
    static uint32_t off(uint32_t addr) { return addr & (kRamSize - 1); }
    uint8_t u8(uint32_t a) const { return p[off(a)]; }
    uint16_t u16(uint32_t a) const { return static_cast<uint16_t>(p[off(a)] | p[off(a + 1)] << 8); }
    uint32_t u32(uint32_t a) const { return u16(a) | static_cast<uint32_t>(u16(a + 2)) << 16; }
    void w16(uint32_t a, uint16_t v) const {
        p[off(a)] = static_cast<uint8_t>(v);
        p[off(a + 1)] = static_cast<uint8_t>(v >> 8);
    }
};

bool in_ram(uint32_t addr, uint32_t size) { return addr >= kRamBase && addr - kRamBase <= kRamSize - size; }

uint32_t deck_addr(uint32_t game_data, int d) {
    return game_data + kDecksOffset + static_cast<uint32_t>(d) * kDeckSize;
}

}  // namespace

RecordState read_records(const uint8_t* ram) {
    RecordState st;
    const Ram r{const_cast<uint8_t*>(ram)};
    const uint32_t data = r.u32(kGameDataPtrAddr);
    if (!in_ram(data + kDecksOffset, kRecordDecks * kDeckSize)) {
        st.error = "no game data yet: load a save first";
        return st;
    }
    st.ok = true;
    for (int d = 0; d < kRecordDecks; ++d) {
        const uint32_t deck = deck_addr(data, d);
        DeckRecord& rec = st.decks[static_cast<size_t>(d)];
        rec.used = r.u8(deck) != 0;
        rec.wins = r.u16(deck + record_field::kWins);
        rec.losses = r.u16(deck + record_field::kLosses);
    }
    return st;
}

RecordChange set_record(uint8_t* ram, int deck, int wins, int losses) {
    RecordChange r;
    if (deck < 0 || deck >= kRecordDecks) return r;
    const Ram rram{ram};
    const uint32_t data = rram.u32(kGameDataPtrAddr);
    if (!in_ram(data + kDecksOffset, kRecordDecks * kDeckSize)) {
        r.message = "no game data yet: load a save first";
        return r;
    }
    const uint32_t addr = deck_addr(data, deck);
    if (rram.u8(addr) == 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "deck %d is not used yet", deck + 1);
        r.message = buf;
        return r;
    }
    wins = std::clamp(wins, 0, kRecordMax);
    losses = std::clamp(losses, 0, kRecordMax);
    rram.w16(addr + record_field::kWins, static_cast<uint16_t>(wins));
    rram.w16(addr + record_field::kLosses, static_cast<uint16_t>(losses));
    char buf[96];
    std::snprintf(buf, sizeof buf, "Deck %d: %dW-%dL (%d battles)", deck + 1, wins, losses, wins + losses);
    r.message = buf;
    r.ok = true;
    return r;
}

}  // namespace trainer
