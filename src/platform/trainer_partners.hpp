#pragma once
// Partner editor: which Digimon is registered in the three partner slots (the trainer's General
// tab). Works on the guest's 2 MB main RAM while the game is suspended, like the Search tab's
// "write once"; nothing is kept in the cheat file (save in game to keep a change).
// No SDL, no guest runtime: unit-tested on a plain RAM buffer (tests/trainer).
//
// What the game keeps for a partner (docs/re/save-data.md, "Partners and Digimentals"):
//   - game_data + 0x80 + slot * 0x288: the slot record. +0x000 / +0x134: two working copies of
//     the card's database record (rebuilt from the card, level bonuses and Digi parts by
//     partner_rebuild, 8004AC98); +0x268 / +0x26C: the database records they come from (the card,
//     the armed Digimental's card); +0x270..+0x277 level-up bonuses; +0x278 card (0 = empty);
//     +0x279 level; +0x27A EXP; +0x27C Digi parts fitted (0xFF = none); +0x27F Digimentals
//     received; +0x282 the armed Digimental; +0x283/+0x284 bonuses from parts.
//   - the partner-owned city flags r294 .. r310;
//   - the collection byte of the partner's card (one copy, obtained, full);
//   - the saved decks (game_data + 0x2408, 3 x 0x10C): a deck entry of kind 0 whose card is a
//     partner's points at that partner's slot record (deck_fixup, 8004945C, on load).
//
// set_partner() does what partner_give (8004A4F0) does for a new partner (level 1, EXP 0, no parts,
// no Digimentals: the next city Menu hands out the ones whose flags are set), moves the deck
// entries of the partner it replaces to the new one, and swaps two slots when the new partner is
// already registered (so a partner is never in two slots: the game assumes one slot per partner).

#include <array>
#include <cstdint>
#include <string>

namespace trainer {

inline constexpr int kPartnerSlots = 3;
inline constexpr int kPartnerKinds = 6;

/// One of the six partners, in the game's partner index order (g_partner_cards, 800710F4).
struct PartnerInfo {
    const char* name;
    uint8_t card;                      ///< the partner's card number
    int flag;                          ///< city flag register "owned" (r294 ...)
    std::array<uint8_t, 3> digimentals;  ///< g_digimental_cards (800710FC), 0 = none
};
extern const std::array<PartnerInfo, kPartnerKinds> kPartners;

/// The partner index of `card`, or -1.
int partner_of_card(int card);

// Guest addresses (docs/re/save-data.md).
inline constexpr uint32_t kGameDataPtrAddr = 0x80070C2Cu;  ///< game_data*
inline constexpr uint32_t kCardDbPtrAddr = 0x801DB000u;    ///< card database*, 0x134 bytes per card
inline constexpr uint32_t kPartnerCardsAddr = 0x800710F4u; ///< g_partner_cards, u8[6]
inline constexpr uint32_t kDigimentalCardsAddr = 0x800710FCu;  ///< g_digimental_cards, u8[6][3]
inline constexpr uint32_t kCardRecordSize = 0x134;
inline constexpr uint32_t kSlotsOffset = 0x80;      ///< game_data + slot record 0
inline constexpr uint32_t kSlotSize = 0x288;
inline constexpr uint32_t kCollectionOffset = 0x1482;  ///< u8 per card
inline constexpr uint32_t kDecksOffset = 0x2408;       ///< 3 saved decks
inline constexpr uint32_t kDeckSize = 0x10C;
inline constexpr int kDeckEntries = 30;                ///< 8-byte entries from deck + 0x10

/// Offsets inside a slot record.
namespace slot_field {
inline constexpr uint32_t kCopyA = 0x000;
inline constexpr uint32_t kCopyB = 0x134;
inline constexpr uint32_t kSourceA = 0x268;  ///< u32: the card's database record
inline constexpr uint32_t kSourceB = 0x26C;  ///< u32: the armed Digimental's record (or the card's)
inline constexpr uint32_t kBonuses = 0x270;  ///< u16 x 4: level-up bonuses
inline constexpr uint32_t kCard = 0x278;
inline constexpr uint32_t kLevel = 0x279;
inline constexpr uint32_t kExp = 0x27A;      ///< u16
inline constexpr uint32_t kParts = 0x27C;    ///< u8 x 3, 0xFF = none
inline constexpr uint32_t kDigimentals = 0x27F;  ///< u8 x 3
inline constexpr uint32_t kArmed = 0x282;
inline constexpr uint32_t kPartBonus = 0x283;  ///< u8 x 2
}  // namespace slot_field

/// One slot as the game has it.
struct PartnerSlot {
    int partner = -1;  ///< kPartners index, -1 = empty
    int level = 0;
    int exp = 0;
};

/// The three slots, or why they cannot be read (no save loaded, unknown data).
struct PartnerState {
    bool ok = false;
    std::string error;
    std::array<PartnerSlot, kPartnerSlots> slots{};
};

/// Reads the slots from `ram` (kRamSize bytes).
PartnerState read_partners(const uint8_t* ram);

struct PartnerChange {
    bool ok = false;
    std::string message;  ///< what was done, or why nothing was
};

/// Registers partner `partner` (kPartners index, -1 = empty the slot) in slot `slot` (0-2).
/// Refused (nothing written) when no save is loaded, when it would leave a gap before a filled
/// slot or empty partner 1, when the partner to remove is still in a deck, or when the new
/// partner's card sits in a deck as a plain card. A partner already in another slot is swapped.
PartnerChange set_partner(uint8_t* ram, int slot, int partner);

}  // namespace trainer
