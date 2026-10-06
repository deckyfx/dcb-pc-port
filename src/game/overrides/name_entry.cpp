// Name entry: the ABC/123 page first, typing half-width letters (docs/re/name-entry.md).
//
// The three name-entry screens (player name, OPENSEG; deck name, SUBSEG; WORD INPUT keyword,
// SAISEG) share one design: a scrolling grid of rows, 10 characters each, from a table of
// {left 5, right 5} Shift-JIS string pointers in the overlay's .data, 9 rows to a page; and a tab
// list on the right whose slot n (drawn from a code-immediate label address) jumps to page n
// (row = n * 9). The page in view picks the highlighted label (scroll / page height), and the
// grid opens on row 0: hiragana.
//
// Letters first. An override around each screen's per-frame draw/input callback makes page 0 the
// letters page without touching the game's logic:
//   - the first 27 rows of the table are rotated (in RAM, once per overlay load) to
//     ABC/123, hiragana, katakana, so the cursor opens on "Ａ" and L1/R1 paging, the tab jumps and
//     the highlighted label all follow the new order;
//   - the three kana/letters labels are drawn in the same order through text aliases (their
//     addresses are code immediates and the slots are 12/12/8 bytes, so the strings cannot be
//     moved in place); the catalog translates them by content as before;
//   - slot 2 of the tab cursor's width jump table takes slot 0's case (48 px, the width of a
//     four-kana label) now that "Katakana" sits there.
//
// Half-width letters (player and deck name). The game keeps the name as 2-byte characters: the
// state's "length" byte (+22) is the cursor's character index 0-5, every edit moves bytes in
// pairs (circle writes the character under the grid cursor at index*2, triangle inserts it,
// cross deletes the one before the cursor; L1/R1 in the name box move the cursor), the sixth
// character sends the focus to OK, and the box draws the underline at index*12. That caps a
// name at 6 characters in its 12 bytes (+ NUL: the 13-byte buffer at +9, the player name at
// game_data + 0, a deck name at its DEK record + 1). Here the ABC page types ASCII (Ａ -> 'A',
// ａ -> 'a', ０ -> '0', its blank cells a space), so a name holds up to 12 letters in the same
// 12 bytes; kana and kanji stay full-width. The edits are done natively on bytes:
//   - name_entry_input (overridden) runs the original (cursor, paging, tab list), then, with the
//     focus it leaves, performs circle / triangle / cross itself and hides those buttons from the
//     rest of the frame callback (restored when the callback returns);
//   - +22 holds the cursor as a byte offset (only these overrides read it);
//   - the name box callback (overridden) draws the name one character at a time (ASCII 6 px,
//     Shift-JIS 12 px, fixed cells as before), moves the cursor with L1/R1 by whole characters,
//     and puts the underline under the character at the cursor.
// The WORD INPUT screen (Wizardmon's spell) types half-width too, so the US keywords fit (up to
// 12 letters: MTLGARURUMON); a long-vowel mark or dash from the kana pages types '-'. The game
// compares the result with its full-width keywords, so keyword.cpp matches it afterwards.

#include "text.hpp"

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
void f_8002ADC8(PsxContext* ctx);  // text_draw_grey(x, y, clut, prop, ot@sp16, str@sp20)
void f_8002E398(PsxContext* ctx);  // menu sound (a0: 1 confirm, 2 move)
void f_8002DCD8(PsxContext* ctx);  // the same sound call, as SAISEG makes it
void f_800193CC(PsxContext* ctx);  // cursor_set_target(cursor, rect*): the box slides there
void f_80019448(PsxContext* ctx);  // cursor_draw(cursor, ot)
}

namespace {

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kSp = 29;

constexpr int kKanaPages = 3;   // hiragana, katakana, ABC/123
constexpr int kPageRows = 9;    // rows per page (tab n -> row n * 9)
constexpr int kRotRows = kKanaPages * kPageRows;

// Name entry state (g_name_entry; OPENSEG and SUBSEG share the layout).
constexpr uint32_t kCol = 0;      // s16 grid column 0-9
constexpr uint32_t kRow = 4;      // s16 grid row
// The name, cursor, focus and tab follow at per-screen offsets (Screen::at): OPENSEG and SUBSEG
// {9, 22, 23, 24}; SAISEG's keyword {10, 24, 25, 26} (its +23 holds the grid's row count).
constexpr size_t kNameMax = 12;   // bytes, without the NUL

constexpr uint32_t kPadPtr = 0x8008C420u;  // pad state pointers (one per pad)
constexpr uint32_t kPadNew = 10;           // u16 pressed this frame
constexpr uint32_t kPadRepeat = 14;        // u16 pressed, with auto-repeat
constexpr uint16_t kCircle = 0x20, kTriangle = 0x10, kCross = 0x40, kL1 = 0x4, kR1 = 0x8;
constexpr uint32_t kGrey = 0x80070A64u;    // text_draw_grey's colour

/// One screen's addresses (overlays load at 0x801E0B30).
struct Screen {
    uint32_t frame;      // per-frame draw/input callback (the override)
    uint32_t grid;       // row table: {left, right} string pointers per row
    uint32_t hira_row0;  // original row 0 left string ("あいうえお")
    uint32_t abc_row0;   // original row 18 left string ("ＡＢＣＤＥ")
    uint32_t label[kKanaPages];  // ひらがな, カタカナ, 英数字 label strings (slots 0-2)
    uint32_t cursor_jt;  // tab cursor jump table (9 code addresses; 0-1 = 48 px, 2-6 = 36 px)
    // Half-width typing (0 / false: the screen keeps the game's own editing).
    bool half_width;
    uint32_t state;       // g_name_entry
    uint32_t input;       // name_entry_input (overridden)
    uint32_t box;         // name box window callback (overridden)
    uint32_t box_cursor;  // the name box's underline cursor object
    uint32_t deck_label;  // "デック" drawn after the name (SUBSEG), 0 = none
    int pad_index;        // state offset of the pad number, -1 = pad 0
    uint32_t edit_pad;    // pad field the grid edits (and cross in the tab list) read
    uint32_t fresh;       // state offset: 1 until the first edit (circle at 0 then clears the rest); 0 = none
    struct {
        uint32_t name;    // 13 bytes
        uint32_t cursor;  // u8: JP character index; here a byte offset
        uint32_t focus;   // u8: 0 grid, 1 tab list
        uint32_t tab;     // s8 tab cursor (7 = OK)
    } at;
    void (*sound)(PsxContext*);  // the menu sound call the screen makes (a0: 1 confirm, 2 move)
    bool keyword;         // WORD INPUT: a long-vowel / dash from the kana pages types '-'
};

constexpr Screen kOpenseg{0x801EB5BCu, 0x801F5118u, 0x801E1BA0u, 0x801E19F0u,
                          {0x801E1BD8u, 0x801E1BE4u, 0x801E1BF0u}, 0x801E1BACu,
                          true, 0x801F9E18u, 0x801EA910u, 0x801EBE18u, 0x801F9D78u, 0, -1, kPadNew, 27,
                          {9, 22, 23, 24}, f_8002E398, false};
constexpr Screen kSubseg{0x801E37B4u, 0x801F541Cu, 0x801E10E0u, 0x801E0F30u,
                         {0x801E1118u, 0x801E1124u, 0x801E1130u}, 0x801E10ECu,
                         true, 0x801F6168u, 0x801E2960u, 0x801E417Cu, 0x801F60C8u, 0x801E116Cu, 27, kPadRepeat, 28,
                         {9, 22, 23, 24}, f_8002E398, false};
constexpr Screen kSaiseg{0x801EDC40u, 0x801F6C4Cu, 0x801E155Cu, 0x801E13ACu,
                         {0x801E1638u, 0x801E1644u, 0x801E1650u}, 0x801E160Cu,
                         true, 0x801F7BB0u, 0x801ECF7Cu, 0x801EE488u, 0x801F7B10u, 0, -1, kPadRepeat, 0,
                         {10, 24, 25, 26}, f_8002DCD8, true};

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

bool rotated(PsxContext& ctx, const Screen& s) { return psx_read32(&ctx, s.grid) == s.abc_row0; }

// --- the name as bytes -------------------------------------------------------------------------

bool sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }

/// Bytes of the character at `i` (0 at the end).
size_t char_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    return sjis_lead(static_cast<uint8_t>(s[i])) && i + 1 < s.size() ? 2 : 1;
}

/// The start of the character before offset `c` (0 at the start).
size_t prev_char(const std::string& s, size_t c) {
    size_t i = 0, prev = 0;
    while (i < c && i < s.size()) {
        prev = i;
        i += char_len(s, i);
    }
    return prev;
}

/// `c` moved back to the start of the character it falls in, and into [0, size].
size_t snap(const std::string& s, size_t c) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = char_len(s, i);
        if (i + n > c) break;
        i += n;
    }
    return std::min(i, s.size());
}

/// The name without the characters that no longer fit in kNameMax bytes.
void fit(std::string& s) {
    size_t i = 0;
    while (i < s.size() && i + char_len(s, i) <= kNameMax) i += char_len(s, i);
    s.resize(i);
}

std::string read_name(PsxContext& ctx, const Screen& s_) {
    std::string s;
    for (uint32_t i = 0; i < kNameMax; ++i) {
        const uint8_t c = psx_read8(&ctx, s_.state + s_.at.name + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

void write_name(PsxContext& ctx, const Screen& s_, const std::string& s) {
    for (uint32_t i = 0; i <= kNameMax; ++i)
        psx_write8(&ctx, s_.state + s_.at.name + i, i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
}

/// A full-width letter, digit or space of the ABC page as ASCII (0: keep it full-width).
char half_width(uint16_t c) {
    if (c >= 0x8260 && c <= 0x8279) return static_cast<char>('A' + (c - 0x8260));
    if (c >= 0x8281 && c <= 0x829A) return static_cast<char>('a' + (c - 0x8281));
    if (c >= 0x824F && c <= 0x8258) return static_cast<char>('0' + (c - 0x824F));
    if (c == 0x8140) return ' ';
    return 0;
}

/// True when the grid cursor is on the letters page (rows 0-8 once the table is rotated).
bool on_letters(PsxContext& ctx, const Screen& s) {
    const auto row = static_cast<int16_t>(psx_read16(&ctx, s.state + kRow));
    return rotated(ctx, s) && row >= 0 && row < kPageRows;
}

/// The character under the grid cursor, as the name will hold it.
std::string grid_char(PsxContext& ctx, const Screen& s) {
    const auto row = static_cast<int16_t>(psx_read16(&ctx, s.state + kRow));
    const auto col = static_cast<int16_t>(psx_read16(&ctx, s.state + kCol));
    const uint32_t str = psx_read32(&ctx, s.grid + 8u * static_cast<uint32_t>(row) + 4u * static_cast<uint32_t>(col / 5));
    const uint32_t at = str + 2u * static_cast<uint32_t>(col % 5);
    const uint8_t hi = psx_read8(&ctx, at), lo = psx_read8(&ctx, at + 1);
    const auto c = static_cast<uint16_t>(hi << 8 | lo);
    if (on_letters(ctx, s))
        if (const char a = half_width(c)) return std::string(1, a);
    if (s.keyword && (c == 0x815B || c == 0x815C || c == 0x815D || c == 0x817C)) return "-";  // ー ― ‐ －
    return std::string{static_cast<char>(hi), static_cast<char>(lo)};
}

void sound(PsxContext& ctx, const Screen& s, uint32_t id) {
    ctx.r[kA0] = id;
    s.sound(&ctx);
}

uint32_t pad(PsxContext& ctx, const Screen& s) {
    const uint32_t index = s.pad_index < 0 ? 0 : psx_read8(&ctx, s.state + static_cast<uint32_t>(s.pad_index));
    return psx_read32(&ctx, kPadPtr + 4u * index);
}

void to_ok(PsxContext& ctx, const Screen& s) {
    psx_write8(&ctx, s.state + s.at.focus, 1);
    psx_write8(&ctx, s.state + s.at.tab, 7);
}

/// Circle (overwrite) or triangle (insert) on the grid: the character under the grid cursor goes
/// in at the name cursor, which moves past it; a full name sends the focus to OK (as the game
/// does after the sixth character).
void type_char(PsxContext& ctx, const Screen& s, bool insert) {
    std::string name = read_name(ctx, s);
    size_t c = snap(name, psx_read8(&ctx, s.state + s.at.cursor));
    const std::string ch = grid_char(ctx, s);
    sound(ctx, s, 1);
    if (insert) {
        name.insert(c, ch);
        fit(name);
    } else {
        std::string out = name.substr(0, c) + ch;
        const bool fresh = s.fresh && c == 0 && psx_read8(&ctx, s.state + s.fresh) == 1;  // the first key replaces the name
        if (!fresh) out += name.substr(c + char_len(name, c));
        fit(out);
        name = out;
    }
    if (name.size() < c + ch.size() || name.compare(c, ch.size(), ch) != 0) {
        to_ok(ctx, s);  // no room for it
    } else {
        write_name(ctx, s, name);
        if (c + ch.size() < kNameMax) c += ch.size();
        else to_ok(ctx, s);
    }
    psx_write8(&ctx, s.state + s.at.cursor, static_cast<uint8_t>(c));
    if (s.fresh) psx_write8(&ctx, s.state + s.fresh, 0);
}

/// Cross: deletes the character before the cursor (at the start: the first one).
void delete_char(PsxContext& ctx, const Screen& s) {
    std::string name = read_name(ctx, s);
    size_t c = snap(name, psx_read8(&ctx, s.state + s.at.cursor));
    if (!name.empty()) sound(ctx, s, 1);
    const size_t at = c == 0 ? 0 : prev_char(name, c);
    name.erase(at, char_len(name, at));
    write_name(ctx, s, name);
    psx_write8(&ctx, s.state + s.at.cursor, static_cast<uint8_t>(at));
    if (s.fresh) psx_write8(&ctx, s.state + s.fresh, 0);
}

// Pad words hidden from the frame callback's own editing, put back when it returns.
struct HiddenPad {
    uint32_t addr = 0;
    uint16_t fresh = 0, repeat = 0;
};
HiddenPad g_hidden;

void input(PsxContext* ctx, const Screen& s) {
    psx_call_original(ctx, s.input);
    if (!rotated(*ctx, s)) return;  // not this screen's data: the game edits as always
    const uint32_t p = pad(*ctx, s);
    g_hidden = {p, psx_read16(ctx, p + kPadNew), psx_read16(ctx, p + kPadRepeat)};
    const uint16_t keys = psx_read16(ctx, p + s.edit_pad);
    uint16_t hide = 0;
    if (psx_read8(ctx, s.state + s.at.focus) == 0) {
        if (keys & kCircle) type_char(*ctx, s, false);
        else if (keys & kTriangle) type_char(*ctx, s, true);
        else if (keys & kCross) delete_char(*ctx, s);
        hide = kCircle | kTriangle | kCross;
    } else if (!(psx_read16(ctx, p + kPadNew) & kCircle) && (keys & kCross)) {
        delete_char(*ctx, s);  // the tab list's circle stays the game's
        hide = kCross;
    }
    // Both fields: an edit that sends the focus to OK must not reach the tab list's circle
    // (SUBSEG reads it from the other field) in the same frame.
    psx_write16(ctx, p + kPadNew, static_cast<uint16_t>(g_hidden.fresh & ~hide));
    psx_write16(ctx, p + kPadRepeat, static_cast<uint16_t>(g_hidden.repeat & ~hide));
}

void restore_pad(PsxContext& ctx) {
    if (!g_hidden.addr) return;
    psx_write16(&ctx, g_hidden.addr + kPadNew, g_hidden.fresh);
    psx_write16(&ctx, g_hidden.addr + kPadRepeat, g_hidden.repeat);
    g_hidden = {};
}

/// text_draw_grey(x, y, clut, prop, ot, str) for a guest string.
void draw_grey(PsxContext& ctx, int x, int y, int clut, int prop, int ot, uint32_t str) {
    const uint32_t sp = ctx.r[kSp], frame = sp - 32;
    psx_write32(&ctx, frame + 16, static_cast<uint32_t>(ot));
    psx_write32(&ctx, frame + 20, str);
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y);
    ctx.r[kA2] = static_cast<uint32_t>(clut);
    ctx.r[kA3] = static_cast<uint32_t>(prop);
    ctx.r[kSp] = frame;
    f_8002ADC8(&ctx);
    ctx.r[kSp] = sp;
}

/// The name box (window callback, a0 = window): the name, L1/R1, the underline cursor.
void name_box(PsxContext* ctx, const Screen& s) {
    if (!rotated(*ctx, s)) return psx_call_original(ctx, s.box);
    const uint32_t win = ctx->r[kA0];
    const int x = static_cast<int16_t>(psx_read16(ctx, win)) + 1;
    const int y = static_cast<int16_t>(psx_read16(ctx, win + 2));
    const int ot = static_cast<int16_t>(psx_read16(ctx, win + 58));
    const std::string name = read_name(*ctx, s);
    std::vector<int> xs;
    dcb::text_draw_verbatim(*ctx, x, y, 7, 0, kGrey, ot, name, &xs);
    if (s.deck_label) draw_grey(*ctx, x + 76, y, 6, 1, ot, s.deck_label);

    size_t c = snap(name, psx_read8(ctx, s.state + s.at.cursor));
    const uint16_t keys = psx_read16(ctx, pad(*ctx, s) + kPadRepeat);
    if (keys & kL1) {
        if (c != 0) {
            sound(*ctx, s, 2);
            c = prev_char(name, c);
        }
    } else if ((keys & kR1) && c < name.size() && c + char_len(name, c) < kNameMax) {
        c += char_len(name, c);
        sound(*ctx, s, 2);
    }
    psx_write8(ctx, s.state + s.at.cursor, static_cast<uint8_t>(c));

    // The underline: under the character at the cursor, or where the next one goes.
    size_t index = 0;
    for (size_t i = 0; i < c; i += char_len(name, i)) ++index;
    const int cx = index < xs.size() ? xs[index] : x;
    const int w = c < name.size() ? xs[index + 1] - cx : (on_letters(*ctx, s) ? 6 : 12);
    const uint32_t sp = ctx->r[kSp], frame = sp - 32, rect = frame + 24;
    psx_write16(ctx, rect + 0, static_cast<uint16_t>(cx));
    psx_write16(ctx, rect + 2, static_cast<uint16_t>(y + 13));
    psx_write16(ctx, rect + 4, static_cast<uint16_t>(w));
    psx_write16(ctx, rect + 6, 0);
    ctx->r[kSp] = frame;
    ctx->r[kA0] = s.box_cursor;
    ctx->r[kA1] = rect;
    f_800193CC(ctx);
    ctx->r[kA0] = s.box_cursor;
    ctx->r[kA1] = static_cast<uint32_t>(ot);
    f_80019448(ctx);
    ctx->r[kSp] = sp;
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
    if (s.half_width) restore_pad(*ctx);
}

}  // namespace

extern "C" {

// OPENSEG 801EB5BC: player name entry, per frame (grid, tab labels, input via 801EA910).
void dcb_name_entry_frame_open(PsxContext* ctx) { run(ctx, kOpenseg); }

// SUBSEG 801E37B4: deck name entry, per frame (input via 801E2960).
void dcb_name_entry_frame_sub(PsxContext* ctx) { run(ctx, kSubseg); }

// SAISEG 801EDC40: WORD INPUT (keyword) entry, per frame (input via 801ECF7C).
void dcb_name_entry_frame_sai(PsxContext* ctx) { run(ctx, kSaiseg); }

// OPENSEG 801EA910 / SUBSEG 801E2960: pad input, then the name edits on bytes.
void dcb_name_entry_input_open(PsxContext* ctx) { input(ctx, kOpenseg); }
void dcb_name_entry_input_sub(PsxContext* ctx) { input(ctx, kSubseg); }
void dcb_name_entry_input_sai(PsxContext* ctx) { input(ctx, kSaiseg); }  // SAISEG 801ECF7C

// OPENSEG 801EBE18 / SUBSEG 801E417C: the name box window callback.
void dcb_name_box_open(PsxContext* ctx) { name_box(ctx, kOpenseg); }
void dcb_name_box_sub(PsxContext* ctx) { name_box(ctx, kSubseg); }
void dcb_name_box_sai(PsxContext* ctx) { name_box(ctx, kSaiseg); }  // SAISEG 801EE488 (the keyword)

}  // extern "C"
