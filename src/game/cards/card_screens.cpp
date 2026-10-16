// The new cards in the Card Menu and Deck Edit (SUBSEG; docs/re/cards.md, "Screens").
//
// The screens list cards through two tables of record pointers by card number (801F7178, a copy at
// 801F7638), static u32 x 301 with three unused words after each, so cards 301-303 fit in place.
// The loops that only touch a card's own entry or byte run to the new limit through runtime
// immediates (config/SLPS-03101/immediates.json); the functions that sort cards into kinds by
// number ranges are wrapped here:
//   801EA3DC tables_build   the new cards' table entries (the original fills 0-300 by kind)
//   801ED004 card_menu_task / 801F0300 deck_edit_init
//                           the list's row count (static data at 801F5CE4, 301) raised before the
//                           list is set up
//   801EC6E8 pages_build    each new card appended to its specialty's page (pages hold 40)
//   801ECC2C totals         the new cards' copies / seen added (the original's range split would
//                           count them as Digivolve)
//   801EC0B0 list_icon      a new card's 24x24 icon: the DECK.TIS sheets have no slot above
//                           300, so the port puts one at (548, 480) in VRAM (left of card 300's
//                           own slot), quantized to the card's specialty palette (CLUT rows 504 +
//                           specialty at x 576: each group has its own 256 colours), from its base
//                           card's icon until it has art
//   801EE13C card_detail / 801F191C deck_card_info
//                           per-Digimon stats are read by card number from arrays of 191: for a
//                           new card they would show other cards' counts, so they read 0
// The auto deck (801F4C10) keeps to cards 0-300: it finds records by number ranges inline.

#include "cards.hpp"

#include "gpu/gpu.hpp"
#include "hw/mmio.hpp"
#include "vfs/tim.hpp"

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kA0 = 4;
constexpr int kOldCards = 301, kDigimon = 191;

constexpr uint32_t kDigimonBase = 0x801DB000u;  // -> Digimon records (0x134 each)
constexpr uint32_t kGameData = 0x80070C2Cu;     // -> game_data, 0x2738 bytes per player
constexpr uint32_t kGameDataSize = 0x2738u;
constexpr uint32_t kCollection = 0x1482;        // game_data: u8 per card, low 3 bits = copies, 0x40 seen
constexpr uint32_t kBestDamage = 0xD0C, kKosScored = 0x1186, kKosTaken = 0x1304;  // per Digimon index

// SUBSEG data.
constexpr uint32_t kTables[] = {0x801F7178u, 0x801F7638u};  // u32 record* per card
constexpr uint32_t kListRows = 0x801F5CE4u;    // u16: rows of the card list (list struct 801F5CC4 +0x20)
constexpr uint32_t kPlayer = 0x801F7E62u;      // s16: whose collection
constexpr uint32_t kPages = 0x801F7FD0u;       // u32 x 8 -> {u16 card, u16 copies} lists, -1 ends
constexpr int kPageCapacity = 40;
constexpr uint32_t kSeenTotal = 0x801F7FF8u, kCopiesTotal = 0x801F7FFAu;  // s16
constexpr uint32_t kDetailCard = 0x801F7FF4u;  // u32: the Card Menu's card shown in detail
constexpr uint32_t kDeckEntries = 0x801F7FC8u; // -> the edited deck's entries (8 bytes from +0x10)
constexpr uint32_t kDeckSlot = 0x801F7CFCu;    // s16: the deck entry under the cursor

constexpr uint32_t kTablesBuild = 0x801EA3DCu, kCardMenuTask = 0x801ED004u, kDeckEditInit = 0x801F0300u;
constexpr uint32_t kPagesBuild = 0x801EC6E8u, kTotals = 0x801ECC2Cu, kListIcon = 0x801EC0B0u;
constexpr uint32_t kCardDetail = 0x801EE13Cu, kDeckCardInfo = 0x801F191Cu;

/// DCB_TRACE_CARDS=1: what the wrappers do.
bool trace() {
    static const bool on = std::getenv("DCB_TRACE_CARDS") != nullptr;
    return on;
}

// The Deck Edit icons (DECK.TIS, 8-bit 24x24) and the routine that draws one.
constexpr int kIconSheetX = 576, kIconSheetY = 256, kIconW = 24, kIconH = 24;
constexpr int kIconClutX = 576, kIconClutRow = 504;  // + specialty; 509 Options, 510 Digivolve and 200
constexpr int kNewIconX = 548, kNewIconY = 480;      // VRAM halfwords: free, by card 300's (560, 480)
constexpr uint32_t kIconDraw = 0x801EC250u;  // (x, y, clut, vram x, vram y, w, h, 1, a3, -1, stack arg)

int new_cards() { return dcb::cards::card_limit() - kOldCards; }

uint32_t player_data(PsxContext& ctx) {
    const uint32_t p = static_cast<uint32_t>(static_cast<int16_t>(psx_read16(&ctx, kPlayer))) & 1;
    return psx_read32(&ctx, kGameData) + p * kGameDataSize;
}

uint32_t record(PsxContext& ctx, int card) {
    return psx_read32(&ctx, kDigimonBase) + static_cast<uint32_t>(kDigimon + card - kOldCards) * 0x134u;
}

void set_rows(PsxContext& ctx) { psx_write16(&ctx, kListRows, static_cast<uint16_t>(dcb::cards::card_limit())); }

/// Runs the original at `addr` with the per-Digimon stats a new card's number would read (best
/// damage, KOs scored / taken: arrays of 191 indexed by card number) reading 0, then puts them back.
void with_stats_hidden(PsxContext* ctx, int card, uint32_t addr) {
    if (card < kOldCards || card >= dcb::cards::card_limit()) return psx_call_original(ctx, addr);
    const uint32_t data = player_data(*ctx), n = static_cast<uint32_t>(card);
    struct Span {
        uint32_t at, size;
    };
    const std::array<Span, 3> spans = {{{data + kBestDamage + n * 6, 6}, {data + kKosScored + n * 2, 2}, {data + kKosTaken + n * 2, 2}}};
    std::array<std::array<uint8_t, 6>, 3> saved{};
    for (size_t i = 0; i < spans.size(); ++i)
        for (uint32_t b = 0; b < spans[i].size; ++b) {
            saved[i][b] = psx_read8(ctx, spans[i].at + b);
            psx_write8(ctx, spans[i].at + b, 0);
        }
    psx_call_original(ctx, addr);
    for (size_t i = 0; i < spans.size(); ++i)
        for (uint32_t b = 0; b < spans[i].size; ++b) psx_write8(ctx, spans[i].at + b, saved[i][b]);
}

/// RGBA of a 15-bit VRAM colour (0x0000 is transparent).
std::array<uint8_t, 4> rgba(uint16_t c) {
    const auto x = [](int v) { return static_cast<uint8_t>(v << 3 | v >> 2); };
    return {x(c & 31), x(c >> 5 & 31), x(c >> 10 & 31), static_cast<uint8_t>(c == 0 ? 0 : 255)};
}

/// The 8-bit indices of icon slot (x, y) in the sheets, row by row.
std::array<uint8_t, kIconW * kIconH> icon_at(const uint16_t* vram, int x0, int y0) {
    std::array<uint8_t, kIconW * kIconH> px{};
    for (int y = 0; y < kIconH; ++y)
        for (int x = 0; x < kIconW; ++x) {
            const uint16_t word = vram[static_cast<size_t>(y0 + y) * 1024 + static_cast<size_t>(x0 + x / 2)];
            px[static_cast<size_t>(y * kIconW + x)] = static_cast<uint8_t>((x & 1) ? word >> 8 : word & 0xFF);
        }
    return px;
}

int icon_x(int n) { return kIconSheetX + (n / 50) * 64 + (n % 5) * 12; }
int icon_y(int n) { return kIconSheetY + ((n % 50) / 5) * 24; }

/// Puts new card `card`'s icon at (kNewIconX, kNewIconY) in its specialty's palette. The frame and
/// the level letter are what every icon of the same specialty and level has in common (taken from
/// one of them); the rest is the picture, from its base's icon through the base's palette,
/// quantized. False when the GPU is busy receiving an upload or there is nothing to take from.
bool new_icon(PsxContext& ctx, int card, int base) {
    auto* mmio = static_cast<hle::Mmio*>(psx::Machine::from(&ctx).mmio());
    if (!mmio || mmio->gpu().receiving_vram() || base >= kDigimon) return false;
    hle::Gpu& gpu = mmio->gpu();
    const uint16_t* vram = gpu.vram();
    const uint32_t db = psx_read32(&ctx, kDigimonBase);
    const uint8_t want = psx_read8(&ctx, record(ctx, card) + 0x1A);  // specialty << 4 | level
    const int from = kIconClutRow + (psx_read8(&ctx, db + static_cast<uint32_t>(base) * 0x134u + 0x1A) >> 4);
    const int to = kIconClutRow + (want >> 4);
    // The frame and letter: the pixels most icons of that specialty and level have alike (the
    // sheets hold the US icons, whose letter is blended into each picture, so not all agree on
    // every pixel of it); 172-190, the armor Digimon and partners, are left out.
    std::array<std::array<uint8_t, 256>, kIconW * kIconH> votes{};
    int donors = 0;
    for (int n = 0; n < 172; ++n) {
        if (psx_read8(&ctx, db + static_cast<uint32_t>(n) * 0x134u + 0x1A) != want) continue;
        const auto px = icon_at(vram, icon_x(n), icon_y(n));
        for (size_t i = 0; i < px.size(); ++i) ++votes[i][px[i]];
        ++donors;
    }
    std::array<uint8_t, kIconW * kIconH> donor{};
    std::array<bool, kIconW * kIconH> shared{};
    for (size_t i = 0; i < votes.size(); ++i) {
        const auto top = std::max_element(votes[i].begin(), votes[i].end());
        donor[i] = static_cast<uint8_t>(top - votes[i].begin());
        shared[i] = *top * 10 >= donors * 6;  // 60% or more
    }
    if (donors < 2) return false;
    std::array<uint16_t, 256> pal{};
    for (int i = 0; i < 256; ++i) pal[static_cast<size_t>(i)] = vram[static_cast<size_t>(to) * 1024 + static_cast<size_t>(kIconClutX + i)];
    // The picture: the card's art, else its base's icon through the base's palette.
    const auto art = dcb::cards::picture_rgba(card, kIconW);
    const auto picture = icon_at(vram, icon_x(base), icon_y(base));
    std::array<uint16_t, kIconW / 2 * kIconH> out{};
    for (size_t i = 0; i < picture.size(); ++i) {
        unsigned idx = donor[i];
        if (!shared[i]) {
            const auto c = art ? std::array<uint8_t, 4>{(*art)[i * 4], (*art)[i * 4 + 1], (*art)[i * 4 + 2], 255}
                               : rgba(vram[static_cast<size_t>(from) * 1024 + static_cast<size_t>(kIconClutX + picture[i])]);
            idx = vfs::palette_index(pal.data(), 256, c[0], c[1], c[2], c[3]);
        }
        out[i / 2] |= static_cast<uint16_t>(idx << ((i & 1) * 8));
    }
    // Unchanged since the last upload: nothing to do (the list draws every icon every frame).
    bool same = true;
    for (int y = 0; y < kIconH && same; ++y)
        for (int x = 0; x < kIconW / 2 && same; ++x)
            same = vram[static_cast<size_t>(kNewIconY + y) * 1024 + static_cast<size_t>(kNewIconX + x)] == out[static_cast<size_t>(y * kIconW / 2 + x)];
    if (same) return true;
    gpu.gp0(0x01000000u);  // clear the texture cache
    gpu.gp0(0xA0000000u);  // CPU -> VRAM
    gpu.gp0(static_cast<uint32_t>(kNewIconY) << 16 | static_cast<uint32_t>(kNewIconX));
    gpu.gp0(static_cast<uint32_t>(kIconH) << 16 | static_cast<uint32_t>(kIconW / 2));
    std::array<uint32_t, out.size() / 2> words{};
    for (size_t i = 0; i < words.size(); ++i) words[i] = out[2 * i] | static_cast<uint32_t>(out[2 * i + 1]) << 16;
    gpu.gp0_block(words.data(), words.size());
    if (trace())
        std::printf("[cards] icon of card %d at (%d, %d): picture from %s (palette row %d), frame from %d icons\n", card,
                    kNewIconX, kNewIconY, art ? "its art" : ("card " + std::to_string(base)).c_str(), to, donors);
    return true;
}

}  // namespace

extern "C" {

// SUBSEG 801EA3DC: tables_build(): record pointers by card number into 801F7178 and 801F7638.
void dcb_card_tables_build(PsxContext* ctx) {
    psx_call_original(ctx, kTablesBuild);
    for (int card = kOldCards; card < dcb::cards::card_limit(); ++card)
        for (const uint32_t table : kTables) psx_write32(ctx, table + static_cast<uint32_t>(card) * 4, record(*ctx, card));
}

// SUBSEG 801ED004: the Card Menu task.
void dcb_card_menu_task(PsxContext* ctx) {
    set_rows(*ctx);
    psx_call_original(ctx, kCardMenuTask);
}

// SUBSEG 801F0300: Deck Edit init.
void dcb_deck_edit_init(PsxContext* ctx) {
    set_rows(*ctx);
    psx_call_original(ctx, kDeckEditInit);
}

// SUBSEG 801EC6E8: pages_build(): the 8 type pages (5 specialties, 3 Option / Digivolve).
void dcb_card_pages_build(PsxContext* ctx) {
    psx_call_original(ctx, kPagesBuild);
    const uint32_t data = player_data(*ctx);
    for (int card = kOldCards; card < dcb::cards::card_limit(); ++card) {
        const uint32_t page = psx_read32(ctx, kPages + 4u * (psx_read8(ctx, record(*ctx, card) + 0x1A) >> 4));
        int used = 0;
        while (used < kPageCapacity && psx_read16(ctx, page + 4u * static_cast<uint32_t>(used)) != 0xFFFF) ++used;
        if (trace()) std::printf("[cards] pages: card %d onto page %08X after %d entries\n", card, page, used);
        if (used >= kPageCapacity) continue;  // full: the page panel draws 40 cells
        const uint32_t at = page + 4u * static_cast<uint32_t>(used);
        psx_write16(ctx, at, static_cast<uint16_t>(card));
        psx_write16(ctx, at + 2, psx_read8(ctx, data + kCollection + static_cast<uint32_t>(card)) & 7);
        psx_write16(ctx, at + 4, 0xFFFF);
    }
}

// SUBSEG 801ECC2C: totals(): copies, seen, per kind and per page.
void dcb_card_totals(PsxContext* ctx) {
    psx_call_original(ctx, kTotals);  // its pages_build call includes the new cards (above)
    const uint32_t data = player_data(*ctx);
    for (int card = kOldCards; card < dcb::cards::card_limit(); ++card) {
        const uint8_t byte = psx_read8(ctx, data + kCollection + static_cast<uint32_t>(card));
        psx_write16(ctx, kCopiesTotal, static_cast<uint16_t>(psx_read16(ctx, kCopiesTotal) + (byte & 7)));
        if (byte & 0x40) psx_write16(ctx, kSeenTotal, static_cast<uint16_t>(psx_read16(ctx, kSeenTotal) + 1));
        if (trace())
            std::printf("[cards] totals: card %d byte %02X -> seen %d, copies %d\n", card, byte,
                        static_cast<int16_t>(psx_read16(ctx, kSeenTotal)), static_cast<int16_t>(psx_read16(ctx, kCopiesTotal)));
    }
}

// SUBSEG 801EC0B0: list_icon(card, x, y, a3, stack arg): the card's 24x24 icon from the DECK.TIS
// sheets: x = 576 + (n / 50) * 64 + (n % 5) * 12, y = 256 + ((n % 50) / 5) * 24 (card 300 at (560,
// 480)), CLUT row 504 + specialty (509 Options, 510 Digivolve and card 200).
void dcb_card_list_icon(PsxContext* ctx) {
    const int card = static_cast<int16_t>(ctx->r[kA0]);
    const int base = dcb::cards::base_of(card);
    if (base < 0 || !new_icon(*ctx, card, base)) {
        if (base >= 0) ctx->r[kA0] = static_cast<uint32_t>(base);  // no VRAM: the base's icon
        psx_call_original(ctx, kListIcon);
        return;
    }
    const uint32_t sp = (ctx->r[29] - 0x100u) & ~7u;  // the frame call_guest gives the callee
    const uint32_t args[] = {static_cast<uint32_t>(kNewIconY), kIconW, kIconH, 1, ctx->r[7] & 0xFF, 0xFFFFFFFFu,
                             psx_read32(ctx, ctx->r[29] + 16)};
    for (uint32_t i = 0; i < 7; ++i) psx_write32(ctx, sp + 16 + 4 * i, args[i]);
    const int spec = psx_read8(ctx, record(*ctx, card) + 0x1A) >> 4;
    const uint32_t clut = static_cast<uint32_t>((kIconClutRow + spec) << 6 | (kIconClutX >> 4));
    psx::call_guest(*ctx, kIconDraw, static_cast<uint32_t>(static_cast<int16_t>(ctx->r[5])),
                    static_cast<uint32_t>(static_cast<int16_t>(ctx->r[6])), clut, kNewIconX);
}

// SUBSEG 801EE13C: card_detail(window): the Card Menu's card page.
void dcb_card_detail(PsxContext* ctx) {
    with_stats_hidden(ctx, static_cast<int32_t>(psx_read32(ctx, kDetailCard)), kCardDetail);
}

// SUBSEG 801F191C: deck_card_info(window): Deck Edit's panel for the deck entry under the cursor.
void dcb_deck_card_info(PsxContext* ctx) {
    const uint32_t entry = psx_read32(ctx, kDeckEntries) +
                           static_cast<uint32_t>(static_cast<int16_t>(psx_read16(ctx, kDeckSlot))) * 8u + 0x10;
    int card = -1;
    if (psx_read8(ctx, entry) == 0) {  // a Digimon: index 191 + k is new card 301 + k
        const int index = psx_read8(ctx, entry + 1);
        if (index >= kDigimon && index < kDigimon + new_cards()) card = kOldCards + index - kDigimon;
    }
    with_stats_hidden(ctx, card, kDeckCardInfo);
}

}  // extern "C"
