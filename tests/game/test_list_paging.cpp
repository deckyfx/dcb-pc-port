// Card-list paging (src/game/overrides/list_paging.hpp): which list_cursor_update calls are card
// lists, and the Left/Right -> L2/R2 bits their pad word gets.

#include "list_paging.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

int main() {
    using namespace dcb::list_paging;

    // Page bits: Left -> L2, Right -> R2, other buttons kept, both or neither -> unchanged.
    CHECK(with_page_bits(0) == 0);
    CHECK(with_page_bits(kLeft) == (kLeft | kL2));
    CHECK(with_page_bits(kRight) == (kRight | kR2));
    CHECK(with_page_bits(kLeft | kRight) == (kLeft | kRight));
    CHECK(with_page_bits(0x1000 | kRight) == (0x1000 | kRight | kR2));  // Up kept
    CHECK(with_page_bits(0x0004 | 0x0008) == (0x0004 | 0x0008));          // L1/R1 untouched
    CHECK(with_page_bits(kL2) == kL2);

    // Call sites: the three card lists, with their jal and delay-slot words.
    CHECK(is_card_list_call(0x801EAD84u, kJalListCursorUpdate, 0x26045CC4u));  // Card Menu
    CHECK(is_card_list_call(0x801F2824u, kJalListCursorUpdate, 0x26045CC4u));  // Deck Edit
    CHECK(is_card_list_call(0x801EA030u, kJalListCursorUpdate, 0x24842AC8u));  // Fusion Shop
    CHECK(is_card_list_call(0x801E8190u, kJalListCursorUpdate, 0x24845C40u));  // Edit Partner Digi-Parts
    CHECK(!is_card_list_call(0x801E7F40u, kJalListCursorUpdate, 0x24845C6Cu));  // Edit Partner equipment
    // Another overlay at the same address (other words), or other lists: no.
    CHECK(!is_card_list_call(0x801EAD84u, 0x00000000u, 0x26045CC4u));
    CHECK(!is_card_list_call(0x801EAD84u, kJalListCursorUpdate, 0x24842AC8u));
    CHECK(!is_card_list_call(0x801EA6F8u, kJalListCursorUpdate, 0x26045DB8u));  // Card Menu sort
    CHECK(!is_card_list_call(0x801E9B54u, kJalListCursorUpdate, 0x26042AF4u));  // Fusion sort
    CHECK(!is_card_list_call(0x801E6A10u, kJalListCursorUpdate, 0x02402021u));  // OPENSEG list

    std::puts("list_paging: ok");
    return 0;
}
