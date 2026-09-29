// Partner editor (see trainer_partners.hpp).

#include "trainer_partners.hpp"

#include "trainer_cheats.hpp"  // kRamSize
#include "trainer_toggles.hpp"  // city flags

#include <cstring>

namespace trainer {

const std::array<PartnerInfo, kPartnerKinds> kPartners = {{
    {"Veemon", 175, 294, {172, 185, 173}},
    {"Hawkmon", 182, 298, {179, 188, 0}},
    {"Armadillomon", 190, 301, {189, 176, 0}},
    {"Gatomon", 184, 307, {181, 178, 0}},
    {"Patamon", 183, 304, {180, 174, 0}},
    {"Wormmon", 187, 310, {186, 177, 0}},
}};

int partner_of_card(int card) {
    for (int i = 0; i < kPartnerKinds; ++i)
        if (kPartners[static_cast<size_t>(i)].card == card) return i;
    return -1;
}

namespace {

// Main RAM view with guest addresses (little-endian, like the PS1).
struct Ram {
    uint8_t* p;
    static uint32_t off(uint32_t addr) { return addr & (kRamSize - 1); }
    uint8_t u8(uint32_t a) const { return p[off(a)]; }
    uint16_t u16(uint32_t a) const { return static_cast<uint16_t>(p[off(a)] | p[off(a + 1)] << 8); }
    uint32_t u32(uint32_t a) const { return u16(a) | static_cast<uint32_t>(u16(a + 2)) << 16; }
    void w8(uint32_t a, uint8_t v) const { p[off(a)] = v; }
    void w16(uint32_t a, uint16_t v) const {
        w8(a, static_cast<uint8_t>(v));
        w8(a + 1, static_cast<uint8_t>(v >> 8));
    }
    void w32(uint32_t a, uint32_t v) const {
        w16(a, static_cast<uint16_t>(v));
        w16(a + 2, static_cast<uint16_t>(v >> 16));
    }
};

bool in_ram(uint32_t addr, uint32_t size) {
    return addr >= kRamBase && addr - kRamBase <= kRamSize - size;
}

/// game_data and the card database, checked against the game's own partner tables.
struct Game {
    uint32_t data = 0;
    uint32_t db = 0;
    std::string error;
    uint32_t slot(int s) const { return data + kSlotsOffset + static_cast<uint32_t>(s) * kSlotSize; }
    uint32_t deck(int d) const { return data + kDecksOffset + static_cast<uint32_t>(d) * kDeckSize; }
    uint32_t card_record(int card) const { return db + static_cast<uint32_t>(card) * kCardRecordSize; }
};

Game find_game(const Ram& ram) {
    Game g;
    for (int i = 0; i < kPartnerKinds; ++i) {
        const PartnerInfo& p = kPartners[static_cast<size_t>(i)];
        bool same = ram.u8(kPartnerCardsAddr + static_cast<uint32_t>(i)) == p.card;
        for (int n = 0; n < 3; ++n)
            same = same && ram.u8(kDigimentalCardsAddr + static_cast<uint32_t>(i * 3 + n)) == p.digimentals[static_cast<size_t>(n)];
        if (!same) {
            g.error = "partner tables not found (game not running?)";
            return g;
        }
    }
    g.data = ram.u32(kGameDataPtrAddr);
    g.db = ram.u32(kCardDbPtrAddr);
    if (!in_ram(g.data, 0x2738) || !in_ram(g.db, 301 * kCardRecordSize)) {
        g.error = "no game data yet: load a save first";
        g.data = g.db = 0;
    }
    return g;
}

void set_flag(const Ram& ram, const Game& g, int reg, bool on) {
    const FlagBit b = city_flag_bit(reg);
    const uint32_t a = g.data + kCityFlagsOffset + b.byte;
    const uint8_t v = ram.u8(a);
    ram.w8(a, static_cast<uint8_t>(on ? v | b.mask : v & ~b.mask));
}

/// The slot's partner index, -1 when empty, -2 when it holds a card that is not a partner's.
int slot_partner(const Ram& ram, const Game& g, int s) {
    const int card = ram.u8(g.slot(s) + slot_field::kCard);
    if (card == 0) return -1;
    const int p = partner_of_card(card);
    return p >= 0 ? p : -2;
}

/// The first used deck holding `card` in a Digimon entry (kind 0), or -1.
int deck_with_card(const Ram& ram, const Game& g, int card) {
    for (int d = 0; d < 3; ++d) {
        const uint32_t deck = g.deck(d);
        if (ram.u8(deck) == 0) continue;
        for (int e = 0; e < kDeckEntries; ++e) {
            const uint32_t entry = deck + 0x10 + static_cast<uint32_t>(e) * 8;
            if (ram.u8(entry) == 0 && ram.u16(entry + 2) == card) return d;
        }
    }
    return -1;
}

/// deck_fixup (8004945C) for the Digimon entries of every used deck: the card's database record,
/// or the slot record of the partner holding that card.
void fix_decks(const Ram& ram, const Game& g) {
    for (int d = 0; d < 3; ++d) {
        const uint32_t deck = g.deck(d);
        if (ram.u8(deck) == 0) continue;
        for (int e = 0; e < kDeckEntries; ++e) {
            const uint32_t entry = deck + 0x10 + static_cast<uint32_t>(e) * 8;
            if (ram.u8(entry) != 0) continue;
            uint32_t target = g.card_record(ram.u8(entry + 1));
            for (int s = 0; s < kPartnerSlots; ++s) {
                const uint8_t card = ram.u8(g.slot(s) + slot_field::kCard);
                if (card != 0 && card == ram.u16(entry + 2)) {
                    target = g.slot(s);
                    break;
                }
            }
            ram.w32(entry + 4, target);
        }
    }
}

/// A new partner in slot `s`, as partner_add (8004A0F8) leaves it, then partner_rebuild
/// (8004AC98) with nothing fitted: both working copies are the card's database record.
void write_new_partner(const Ram& ram, const Game& g, int s, int card) {
    namespace f = slot_field;
    const uint32_t rec = g.slot(s);
    const uint32_t src = g.card_record(card);
    ram.w32(rec + f::kSourceA, src);
    ram.w32(rec + f::kSourceB, src);
    ram.w8(rec + f::kCard, static_cast<uint8_t>(card));
    ram.w8(rec + f::kLevel, 1);
    ram.w16(rec + f::kExp, 0);
    for (uint32_t i = 0; i < 3; ++i) {
        ram.w8(rec + f::kParts + i, 0xFF);
        ram.w8(rec + f::kDigimentals + i, 0);
    }
    ram.w8(rec + f::kArmed, 0);
    for (uint32_t i = 0; i < 4; ++i) ram.w16(rec + f::kBonuses + 2 * i, 0);
    // partner_rebuild with no bonuses and no parts.
    ram.w8(rec + f::kPartBonus, 0);
    ram.w8(rec + f::kPartBonus + 1, 0);
    for (uint32_t i = 0; i < kCardRecordSize; ++i) {
        const uint8_t v = ram.u8(src + i);
        ram.w8(rec + f::kCopyA + i, v);
        ram.w8(rec + f::kCopyB + i, v);
    }
    for (const uint32_t copy : {f::kCopyA, f::kCopyB}) {
        if (ram.u16(rec + copy + 0x58) == 0) ram.w16(rec + copy + 0x58, 100);
        for (uint32_t i = 0; i < 3; ++i)  // attack values are never negative
            if (static_cast<int16_t>(ram.u16(rec + copy + 0x20 + 28 * i)) < 0) ram.w16(rec + copy + 0x20 + 28 * i, 0);
        const uint8_t kind = ram.u8(rec + copy + 0xE4);
        if (kind >= 5 && kind <= 8) ram.w16(rec + copy + 0x58, 0);
    }
}

std::string slot_name(int s) { return "Partner " + std::to_string(s + 1); }

PartnerChange refuse(std::string why) { return {false, std::move(why)}; }

}  // namespace

PartnerState read_partners(const uint8_t* ram_bytes) {
    PartnerState st;
    const Ram ram{const_cast<uint8_t*>(ram_bytes)};  // read only
    const Game g = find_game(ram);
    if (!g.error.empty()) {
        st.error = g.error;
        return st;
    }
    for (int s = 0; s < kPartnerSlots; ++s) {
        const int p = slot_partner(ram, g, s);
        if (p == -2) {
            st.error = slot_name(s) + " holds card " + std::to_string(ram.u8(g.slot(s) + slot_field::kCard)) +
                       ", not a partner";
            return st;
        }
        PartnerSlot& slot = st.slots[static_cast<size_t>(s)];
        slot.partner = p;
        if (p >= 0) {
            slot.level = ram.u8(g.slot(s) + slot_field::kLevel);
            slot.exp = ram.u16(g.slot(s) + slot_field::kExp);
        }
    }
    if (st.slots[0].partner < 0) {
        st.error = "no partner yet: load a save first";
        return st;
    }
    st.ok = true;
    return st;
}

PartnerChange set_partner(uint8_t* ram_bytes, int slot, int partner) {
    if (slot < 0 || slot >= kPartnerSlots || partner < -1 || partner >= kPartnerKinds) return refuse("no such slot or partner");
    const PartnerState st = read_partners(ram_bytes);
    if (!st.ok) return refuse(st.error);
    const Ram ram{ram_bytes};
    const Game g = find_game(ram);
    const std::string who = slot_name(slot);
    const int old = st.slots[static_cast<size_t>(slot)].partner;
    if (old == partner) return refuse(who + " is " + (old < 0 ? std::string("empty") : kPartners[static_cast<size_t>(old)].name) + " already");

    // The slots after the change must be filled from partner 1 on: the game fills the first empty
    // slot and some screens stop at the first empty one.
    std::array<int, kPartnerSlots> after{};
    for (int s = 0; s < kPartnerSlots; ++s) after[static_cast<size_t>(s)] = st.slots[static_cast<size_t>(s)].partner;
    int other = -1;  // the slot that already holds `partner`
    for (int s = 0; s < kPartnerSlots; ++s)
        if (partner >= 0 && s != slot && after[static_cast<size_t>(s)] == partner) other = s;
    if (other >= 0) std::swap(after[static_cast<size_t>(slot)], after[static_cast<size_t>(other)]);
    else after[static_cast<size_t>(slot)] = partner;
    if (after[0] < 0) return refuse("Partner 1 cannot be empty");
    for (int s = 1; s < kPartnerSlots; ++s)
        if (after[static_cast<size_t>(s)] >= 0 && after[static_cast<size_t>(s - 1)] < 0)
            return refuse(s - 1 == slot ? "empty the last partner first" : "fill " + slot_name(s - 1) + " first");

    if (other >= 0) {  // already registered: swap the two records (each is self-contained)
        const uint32_t a = g.slot(slot), b = g.slot(other);
        for (uint32_t i = 0; i < kSlotSize; ++i) {
            const uint8_t va = ram.u8(a + i);
            ram.w8(a + i, ram.u8(b + i));
            ram.w8(b + i, va);
        }
        fix_decks(ram, g);
        return {true, who + " <-> " + slot_name(other) + ": swapped (save in game to keep it)"};
    }

    if (partner < 0) {  // empty the last slot
        const PartnerInfo& o = kPartners[static_cast<size_t>(old)];
        if (const int d = deck_with_card(ram, g, o.card); d >= 0)
            return refuse(std::string(o.name) + " is in deck " + std::to_string(d + 1) + ": take it out first");
        ram.w8(g.slot(slot) + slot_field::kCard, 0);
        set_flag(ram, g, o.flag, false);
        ram.w8(g.data + kCollectionOffset + o.card, 0x40);  // seen, none owned
        return {true, who + ": " + o.name + " removed (save in game to keep it)"};
    }

    const PartnerInfo& n = kPartners[static_cast<size_t>(partner)];
    if (const int d = deck_with_card(ram, g, n.card); d >= 0)
        return refuse(std::string("a plain ") + n.name + " card is in deck " + std::to_string(d + 1) + ": take it out first");
    write_new_partner(ram, g, slot, n.card);
    set_flag(ram, g, n.flag, true);
    ram.w8(g.data + kCollectionOffset + n.card, 0xF1);  // partner_give: one copy, obtained, seen, new, full
    std::string what = who + ": " + n.name + " Lv 1";
    if (old >= 0) {
        const PartnerInfo& o = kPartners[static_cast<size_t>(old)];
        set_flag(ram, g, o.flag, false);
        ram.w8(g.data + kCollectionOffset + o.card, 0x40);
        // The decks keep their partner: entries of the old one become the new one.
        for (int d = 0; d < 3; ++d) {
            const uint32_t deck = g.deck(d);
            if (ram.u8(deck) == 0) continue;
            for (int e = 0; e < kDeckEntries; ++e) {
                const uint32_t entry = deck + 0x10 + static_cast<uint32_t>(e) * 8;
                if (ram.u8(entry) == 0 && ram.u16(entry + 2) == o.card) {
                    ram.w8(entry + 1, n.card);
                    ram.w16(entry + 2, n.card);
                }
            }
        }
        what += " (was " + std::string(o.name) + ")";
    }
    fix_decks(ram, g);
    return {true, what + " (save in game to keep it)"};
}

}  // namespace trainer
