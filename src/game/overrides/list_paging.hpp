#pragma once
// Left/Right page the card lists (src/game/overrides/list_paging.cpp). Pure logic shared with its
// unit test: which calls of list_cursor_update are card lists, and the pad bits the list sees.
// docs/re/card-lists.md.

#include <array>
#include <cstdint>

namespace dcb::list_paging {

// Game pad bits (the words at *(8008C420 + 4 * pad), active high).
constexpr uint16_t kL2 = 0x0001;     // list: page up
constexpr uint16_t kR2 = 0x0002;     // list: page down
constexpr uint16_t kRight = 0x2000;
constexpr uint16_t kLeft = 0x8000;

/// `jal 0x800199F4` (list_cursor_update): the word before a card-list call's delay slot.
constexpr uint32_t kJalListCursorUpdate = 0x0C00667Du;

/// A card list's call of list_cursor_update: its return address and the delay-slot word (the
/// list's address into a0), which together name the overlay that is resident.
struct CallSite {
    uint32_t ra;
    uint32_t delay_word;
    const char* what;
};

/// The card lists. Each is a single column (Up/Down, L2/R2 page) and nothing on its screen
/// reads Left/Right while the list has the focus (checked in the game, docs/re/card-lists.md).
inline constexpr std::array<CallSite, 3> kCardLists{{
    {0x801EAD84u, 0x26045CC4u, "SUBSEG Card Menu card list (801EA7DC, list 801F5CC4)"},
    {0x801F2824u, 0x26045CC4u, "SUBSEG Deck Edit card selection (801F22DC, list 801F5CC4)"},
    {0x801EA030u, 0x24842AC8u, "EVOSEG Fusion Shop card list (801E9C08, list 801F2AC8)"},
}};

/// True when a call returning to `ra`, whose jal and delay-slot words are `jal_word` /
/// `delay_word`, is one of kCardLists (the words guard against another overlay at the address).
constexpr bool is_card_list_call(uint32_t ra, uint32_t jal_word, uint32_t delay_word) {
    if (jal_word != kJalListCursorUpdate) return false;
    for (const CallSite& s : kCardLists)
        if (s.ra == ra && s.delay_word == delay_word) return true;
    return false;
}

/// The auto-repeat word the list sees: Left adds L2 (page up), Right adds R2 (page down); both
/// at once add nothing.
constexpr uint16_t with_page_bits(uint16_t repeat) {
    const bool left = (repeat & kLeft) != 0, right = (repeat & kRight) != 0;
    if (left == right) return repeat;
    return static_cast<uint16_t>(repeat | (left ? kL2 : kR2));
}

}  // namespace dcb::list_paging
