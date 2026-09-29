// Name entry: the ABC/123 page first (docs/re/name-entry.md).
//
// The three name-entry screens (player name, OPENSEG; deck name, SUBSEG; WORD INPUT keyword,
// SAISEG) share one design: a scrolling grid of rows, 10 characters each, from a table of
// {left 5, right 5} Shift-JIS string pointers in the overlay's .data, 9 rows to a page; and a tab
// list on the right whose slot n (drawn from a code-immediate label address) jumps to page n
// (row = n * 9). The page in view picks the highlighted label (scroll / page height), and the
// grid opens on row 0: hiragana.
//
// This override runs around each screen's per-frame draw/input callback and makes page 0 the
// letters page without touching the game's logic:
//   - the first 27 rows of the table are rotated (in RAM, once per overlay load) to
//     ABC/123, hiragana, katakana, so the cursor opens on "Ａ" and L1/R1 paging, the tab jumps and
//     the highlighted label all follow the new order;
//   - the three kana/letters labels are drawn in the same order through text aliases (their
//     addresses are code immediates and the slots are 12/12/8 bytes, so the strings cannot be
//     moved in place); the catalog translates them by content as before;
//   - slot 2 of the tab cursor's width jump table takes slot 0's case (48 px, the width of a
//     four-kana label) now that "Katakana" sits there.
// Typed characters are the grid's own strings, so what the player types is unchanged.

#include "text.hpp"

#include <psx/recomp.h>

#include <cstdint>

namespace {

constexpr int kKanaPages = 3;   // hiragana, katakana, ABC/123
constexpr int kPageRows = 9;    // rows per page (tab n -> row n * 9)
constexpr int kRotRows = kKanaPages * kPageRows;

/// One screen's addresses (overlays load at 0x801E0B30).
struct Screen {
    uint32_t frame;      // per-frame draw/input callback (the override)
    uint32_t grid;       // row table: {left, right} string pointers per row
    uint32_t hira_row0;  // original row 0 left string ("あいうえお")
    uint32_t abc_row0;   // original row 18 left string ("ＡＢＣＤＥ")
    uint32_t label[kKanaPages];  // ひらがな, カタカナ, 英数字 label strings (slots 0-2)
    uint32_t cursor_jt;  // tab cursor jump table (9 code addresses; 0-1 = 48 px, 2-6 = 36 px)
};

constexpr Screen kOpenseg{0x801EB5BCu, 0x801F5118u, 0x801E1BA0u, 0x801E19F0u,
                          {0x801E1BD8u, 0x801E1BE4u, 0x801E1BF0u}, 0x801E1BACu};
constexpr Screen kSubseg{0x801E37B4u, 0x801F541Cu, 0x801E10E0u, 0x801E0F30u,
                         {0x801E1118u, 0x801E1124u, 0x801E1130u}, 0x801E10ECu};
constexpr Screen kSaiseg{0x801EDC40u, 0x801F6C4Cu, 0x801E155Cu, 0x801E13ACu,
                         {0x801E1638u, 0x801E1644u, 0x801E1650u}, 0x801E160Cu};

/// Rotate the row table (idempotent: row 0 tells whether it is done). False when the table
/// holds neither layout (not this screen's data), so the caller leaves everything alone.
bool rotate_grid(PsxContext& ctx, const Screen& s) {
    const uint32_t row0 = psx_read32(&ctx, s.grid);
    if (row0 == s.abc_row0) return true;
    if (row0 != s.hira_row0 || psx_read32(&ctx, s.grid + 8u * 2 * kPageRows) != s.abc_row0) return false;
    uint32_t rows[kRotRows][2];
    for (int r = 0; r < kRotRows; ++r)
        for (int h = 0; h < 2; ++h) rows[r][h] = psx_read32(&ctx, s.grid + 8u * r + 4u * h);
    // New page p shows old page (p + 2) % 3: ABC/123, hiragana, katakana.
    for (int r = 0; r < kRotRows; ++r) {
        const int from = (r + 2 * kPageRows) % kRotRows;
        for (int h = 0; h < 2; ++h) psx_write32(&ctx, s.grid + 8u * r + 4u * h, rows[from][h]);
    }
    // Tab cursor width: slot 2 now holds a four-kana label.
    const uint32_t wide = psx_read32(&ctx, s.cursor_jt);
    if (psx_read32(&ctx, s.cursor_jt + 4) == wide) psx_write32(&ctx, s.cursor_jt + 8, wide);
    return true;
}

void run(PsxContext* ctx, const Screen& s) {
    if (!rotate_grid(*ctx, s)) return psx_call_original(ctx, s.frame);
    const dcb::TextAlias aliases[kKanaPages] = {
        {s.label[0], s.label[2]},  // slot 0: 英数字
        {s.label[1], s.label[0]},  // slot 1: ひらがな
        {s.label[2], s.label[1]},  // slot 2: カタカナ
    };
    dcb::text_set_aliases(aliases, kKanaPages);
    psx_call_original(ctx, s.frame);
    dcb::text_set_aliases(nullptr, 0);
}

}  // namespace

extern "C" {

// OPENSEG 801EB5BC: player name entry, per frame (grid, tab labels, input via 801EA910).
void dcb_name_entry_frame_open(PsxContext* ctx) { run(ctx, kOpenseg); }

// SUBSEG 801E37B4: deck name entry, per frame (input via 801E2960).
void dcb_name_entry_frame_sub(PsxContext* ctx) { run(ctx, kSubseg); }

// SAISEG 801EDC40: WORD INPUT (keyword) entry, per frame (input via 801ECF7C).
void dcb_name_entry_frame_sai(PsxContext* ctx) { run(ctx, kSaiseg); }

}  // extern "C"
