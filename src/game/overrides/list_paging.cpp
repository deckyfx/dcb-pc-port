// Card lists: Left/Right page like L2/R2.
//
// Every scrolling list of the game steps its cursor with list_cursor_update (EXE 800199F4, once a
// frame while the list has the focus): Up/Down move one row, L2/R2 a page, all read from the
// auto-repeat word (+14) of the list's pad. The card lists are single columns, so Left/Right do
// nothing there. This override runs the original; for the calls listed in list_paging.hpp only
// (the Card Menu list, the Deck Edit card selection, the Fusion Shop card list) it first adds
// L2/R2 to the pad's auto-repeat word when Left/Right are in it, and puts the word back after.
// Nothing else sees the added bits: other lists, the Card Menu's L1/R1 type panel, name entry,
// deck edit and battle keep their own buttons. docs/re/card-lists.md.

#include "list_paging.hpp"

#include <psx/recomp.h>

#include <cstdint>

namespace {

constexpr uint32_t kListCursorUpdate = 0x800199F4u;
constexpr uint32_t kPadTable = 0x8008C420u;  // pad state pointers, 4 bytes per pad
constexpr uint32_t kListPad = 0x28;          // u8: the list's pad index
constexpr uint32_t kRepeat = 14;             // u16: buttons pressed this frame with auto-repeat

bool in_ram(uint32_t addr) { return addr >= 0x80000000u && addr < 0x80200000u; }

}  // namespace

extern "C" {

// 800199F4: list_cursor_update(list).
void dcb_list_cursor_update(PsxContext* ctx) {
    using namespace dcb::list_paging;
    const uint32_t ra = ctx->r[31];
    const uint32_t list = ctx->r[4];
    uint32_t pad = 0;
    uint16_t saved = 0;
    if (in_ram(ra - 8) && in_ram(list) &&
        is_card_list_call(ra, psx_read32(ctx, ra - 8), psx_read32(ctx, ra - 4))) {
        const uint8_t index = psx_read8(ctx, list + kListPad);
        if (index < 2) pad = psx_read32(ctx, kPadTable + 4u * index);
        if (in_ram(pad)) {
            saved = psx_read16(ctx, pad + kRepeat);
            psx_write16(ctx, pad + kRepeat, with_page_bits(saved));
        } else {
            pad = 0;
        }
    }
    psx_call_original(ctx, kListCursorUpdate);
    if (pad != 0) psx_write16(ctx, pad + kRepeat, saved);
}

}  // extern "C"
