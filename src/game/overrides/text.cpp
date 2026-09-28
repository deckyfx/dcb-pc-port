// English text renderer for the JP build (config/SLPS-03101/overrides.json).
//
// The JP engine draws Shift-JIS through a kanji glyph cache; ASCII bytes are skipped, so
// English text renders as nothing. These overrides dispatch per string (docs/re/text-engine.md §7.1):
// a string with no SJIS lead byte (0x81-0x98) takes the ASCII path, a port of the US
// renderer's draw (US 80028d48) / measure (US 800293fc) with `*`-escaped control codes;
// anything else falls back to the recompiled JP original, so untranslated JP strings keep
// rendering. Measure and draw share one parser, so a string always takes the same path.
//
// The ASCII path emits the same packets the JP function does: one semi-transparent textured
// SPRT per glyph (7 words, 0x1C bytes: DR_TPAGE + SPRT) from the pool at g_text_prim
// (DAT_801d9714), pool-full checked like FUN_80029ef4, linked into the current frame's OT
// entry `ot`, and the same result globals (g_text_w/h 801d9708/970c). Inline icons are
// delegated to the JP icon function (same index mapping as the US codes); the JP sheet
// stays loaded (option B), so the icon row base question does not arise.
// The glyph pixels are the US font rows, uploaded once into a free VRAM area (option B);
// the width table comes from en_font.bin, built on the player's machine by
// tools/text/en_text.py (never in git).
//
// JP argument order is kept: draw(x, y, clut, prop, rgb*, ot, str).

#include "gpu/gpu.hpp"
#include "hw/mmio.hpp"

#include <psx/backtrace.hpp>
#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
void f_80029F70(PsxContext* ctx);  // JP icon (not overridden; callable directly)
}

namespace {

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kSp = 29;

// JP text globals (docs/re/text-engine.md §3.3).
constexpr uint32_t kTextW = 0x801D9708u;      // result width
constexpr uint32_t kTextH = 0x801D970Cu;      // result height
constexpr uint32_t kTextPrim = 0x801D9714u;   // next free text primitive
constexpr uint32_t kSysTimX = 0x801D9704u;    // SYSTEM.TIM VRAM x (u16)
constexpr uint32_t kSysTimY = 0x801D9706u;    // SYSTEM.TIM VRAM y (u16)
constexpr uint32_t kClutX = 0x801D9710u;      // text CLUT block x (u16)
constexpr uint32_t kClutY = 0x801D9712u;      // text CLUT block y (u16)
constexpr uint32_t kOtBase = 0x8007BF80u;     // frame struct; OT entries at +0x70
constexpr uint32_t kPoolSize = 0x801D96F0u;   // primitive pool size (u16, 1000)
constexpr uint32_t kFrameOff = 0x40B8u;       // frame struct -> pool base
constexpr uint32_t kDrawAddr = 0x8002AE00u;
constexpr uint32_t kMeasureAddr = 0x8002B638u;
constexpr uint32_t kCondDrawAddr = 0x8002BD3Cu;
constexpr uint32_t kCondMeasureAddr = 0x8002C574u;

constexpr int kPrimSize = 0x1C;  // DR_TPAGE (2 words) + SPRT (5 words)

// US font geometry (docs/re/text-engine.md §4.2): glyph i = c - 0x20 in a 16-per-row grid,
// u = col*6 + (width >> 4), v = row*12 + 0x30, advance = width & 0xF (prop) or 6.
constexpr int kFontCols = 16, kCellW = 6, kCellH = 12;
constexpr int kFontFirst = 48, kFontRows = 176;  // en_font.bin rows 48..223 (first cell row)

// The US font rows live in the GPU's private sheet (hle::Gpu::set_private_sheet), not in VRAM:
// every VRAM area is some screen's texture somewhere (the city maps load into the area this
// port first tried), so a font there either overwrote the game's art or was overwritten. Each
// glyph's packet puts the sheet marker where the texpage word went; UVs are sheet coordinates
// (row 0 = font row 48). 64 units x 176 rows, 4 bpp.
constexpr int kSheetUnits = 64;

uint8_t rd8(PsxContext& ctx, uint32_t a) { return psx_read8(&ctx, a); }
uint16_t rd16(PsxContext& ctx, uint32_t a) { return psx_read16(&ctx, a); }
uint32_t rd32(PsxContext& ctx, uint32_t a) { return psx_read32(&ctx, a); }
void wr8(PsxContext& ctx, uint32_t a, uint8_t v) { psx_write8(&ctx, a, v); }
void wr16(PsxContext& ctx, uint32_t a, uint16_t v) { psx_write16(&ctx, a, v); }
void wr32(PsxContext& ctx, uint32_t a, uint32_t v) { psx_write32(&ctx, a, v); }

void load_names();

// en_font.bin payload (no game data in git; built on the player's machine).
std::vector<uint8_t> g_font_rows;  // 176 rows x 128 bytes (4 bpp, 64 halfwords/row)
uint8_t g_widths[96] = {0};        // width[c - 0x20]: hi nibble u offset, lo nibble advance
bool g_font_loaded = false;

/// Load en_font.bin once (rows + width table) and hand the rows to the GPU's private sheet.
/// Returns false when the file is missing (the caller falls back to JP rendering).
bool load_font(PsxContext& ctx) {
    if (g_font_loaded) return !g_font_rows.empty();
    g_font_loaded = true;
    const std::string path = std::string("assets/") + DCB_GAME_ID + "/en_font.bin";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[4];
    if (std::fread(hdr, 1, 4, f) != 4) {
        std::fclose(f);
        return false;
    }
    const uint16_t first = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
    const uint16_t last = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
    if (first != kFontFirst || last != kFontFirst + kFontRows - 1) {
        std::fclose(f);
        std::fprintf(stderr, "[text] en_font.bin missing or bad header (ASCII text falls back to JP)\n");
        return false;
    }
    g_font_rows.resize(static_cast<size_t>(kFontRows) * 128);
    bool ok = std::fread(g_font_rows.data(), 1, g_font_rows.size(), f) == g_font_rows.size() &&
              std::fread(g_widths, 1, sizeof(g_widths), f) == sizeof(g_widths);
    std::fclose(f);
    auto* mmio = static_cast<hle::Mmio*>(psx::Machine::from(&ctx).mmio());
    if (!ok || !mmio) {
        g_font_rows.clear();
        return false;
    }
    std::vector<uint16_t> units(static_cast<size_t>(kFontRows) * kSheetUnits);
    for (size_t i = 0; i < units.size(); ++i)
        units[i] = static_cast<uint16_t>(g_font_rows[i * 2] | (g_font_rows[i * 2 + 1] << 8));
    mmio->gpu().set_private_sheet(std::move(units), kSheetUnits, kFontRows);
    load_names();
    return true;
}

// en_names.txt (built with en_font.bin): English names longer than their JP slot. The slot
// holds the first 11 letters and a tag byte (0x01, 0x02... for names sharing those letters);
// the renderer draws the full name wherever that key shows up. Other renderers show the 11
// letters (the tag byte is skipped).
struct LongName {
    std::string key;   // 11 letters + tag byte
    std::string full;  // the US name
};
std::vector<LongName> g_names;

/// Loads en_names.txt ("<11 letters>\t<tag>\t<full name>" per line). Missing file: no names.
void load_names() {
    const std::string path = std::string("assets/") + DCB_GAME_ID + "/en_names.txt";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const int tag = std::atoi(l.substr(a + 1, b - a - 1).c_str());
        if (tag < 1 || tag > 9) continue;
        g_names.push_back({l.substr(0, a) + static_cast<char>(tag), l.substr(b + 1)});
    }
    std::fclose(f);
}

/// A guest string copied to the host (NUL included) with the English expansions applied: long
/// names (en_names.txt) and "デック" after an English name, which becomes " Deck" like the US
/// "%s Deck" (the JP "%sデック" at 800114E0).
struct Text {
    std::vector<uint8_t> b;
    uint8_t at(size_t i) const { return i < b.size() ? b[i] : 0; }
    size_t size() const { return b.empty() ? 0 : b.size() - 1; }  // without the NUL
};

Text load_text(PsxContext& ctx, uint32_t str) {
    std::string s;
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint8_t c = rd8(ctx, str + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    for (const LongName& n : g_names)
        for (size_t p = s.find(n.key); p != std::string::npos; p = s.find(n.key, p + n.full.size()))
            s.replace(p, n.key.size(), n.full);
    static const std::string kDeck = "\x83\x66\x83\x62\x83\x4e";  // デック
    for (size_t p = s.find(kDeck); p != std::string::npos; p = s.find(kDeck, p + 1)) {
        const auto prev = p ? static_cast<uint8_t>(s[p - 1]) : 0;
        if (prev > 0x20 && prev < 0x7F) s.replace(p, kDeck.size(), " Deck");
    }
    Text t;
    t.b.assign(s.begin(), s.end());
    t.b.push_back(0);
    return t;
}

/// True when the string is plain 7-bit text (our grafted ASCII). Any high byte means SJIS or
/// half-width kana: JP, or English inside JP.
bool is_ascii(const Text& t) {
    for (size_t i = 0; i < t.size(); ++i)
        if (t.b[i] >= 0x80) return false;
    return true;
}

/// Pool-full check (FUN_80029ef4): true when g_text_prim reached the pool end.
bool prim_full(PsxContext& ctx) {
    uint32_t frame = rd32(ctx, kOtBase);
    uint32_t base = rd32(ctx, frame + kFrameOff);
    uint16_t size = rd16(ctx, kPoolSize);
    return rd32(ctx, kTextPrim) == base + static_cast<uint32_t>(size) * kPrimSize;
}

uint32_t prim_alloc(PsxContext& ctx) {
    uint32_t p = rd32(ctx, kTextPrim);
    wr32(ctx, kTextPrim, p + kPrimSize);
    return p;
}

/// CLUT word for text colour `idx`: CLUT n lives at (base_x + (n%2)*16, base_y + n/2).
uint16_t clut_of(PsxContext& ctx, int idx) {
    uint16_t bx = rd16(ctx, kClutX), by = rd16(ctx, kClutY);
    return static_cast<uint16_t>(((by + idx / 2) << 6) | ((bx + (idx % 2) * 16) >> 4));
}

/// Emit one DR_TPAGE+SPRT packet with the JP field layout (see the f_8002AE00 block).
/// (u, v) is the texture position, (w, h) the sprite size, clut the text colour index.
void emit_sprt(PsxContext& ctx, int x, int y, int u, int v, int w, int h, uint32_t rgb, int clut,
               int ot) {
    uint32_t p = prim_alloc(ctx);
    wr16(ctx, p + 0x10, static_cast<uint16_t>(x));
    wr16(ctx, p + 0x12, static_cast<uint16_t>(y));
    wr8(ctx, p + 0x14, static_cast<uint8_t>(u));
    wr8(ctx, p + 0x15, static_cast<uint8_t>(v));
    wr16(ctx, p + 0x16, clut_of(ctx, clut));
    wr16(ctx, p + 0x18, static_cast<uint16_t>(w));
    wr16(ctx, p + 0x1A, static_cast<uint16_t>(h));
    wr8(ctx, p + 0x0C, rd8(ctx, rgb));
    wr8(ctx, p + 0x0D, rd8(ctx, rgb + 1));
    wr8(ctx, p + 0x0E, rd8(ctx, rgb + 2));
    wr8(ctx, p + 0x03, 1);
    wr8(ctx, p + 0x0F, rd8(ctx, p + 0x0F) | 2);  // semi-transparent bit
    // The packet's one-word slot (the JP code's texpage word) carries the private-sheet marker:
    // the SPRT that follows samples the font from the sheet, semi-transparency mode 0 like the
    // JP glyph pages (GetTPage(0, 0, ...)).
    wr32(ctx, p + 0x04, hle::Gpu::kSheetMarker | 0u);
    // Link into the OT entry: packet+0 points at packet+8 (the SPRT after the DR_TPAGE
    // E1 word), packet+8 chains the old head, OT points at the packet. Words keep tags.
    uint32_t frame = rd32(ctx, kOtBase);
    uint32_t ot_addr = frame + 0x70u + static_cast<uint32_t>(ot) * 4u;
    uint32_t head = rd32(ctx, ot_addr) & 0x00FFFFFFu;
    wr32(ctx, p + 0x08, (rd32(ctx, p + 0x08) & 0xFF000000u) | head);
    wr32(ctx, p + 0x00, (rd32(ctx, p + 0x00) & 0xFF000000u) | ((p + 8u) & 0x00FFFFFFu));
    wr32(ctx, ot_addr, (rd32(ctx, ot_addr) & 0xFF000000u) | (p & 0x00FFFFFFu));
}

/// Inline icon through the JP icon function (same index mapping as the US * codes).
/// The JP sheet stays loaded under option B, so icons render as the JP art.
void emit_icon(PsxContext& ctx, int x, int y, int mode, int idx, uint32_t rgb, int ot) {
    uint32_t save[4] = {ctx.r[kA0], ctx.r[kA1], ctx.r[kA2], ctx.r[kA3]};
    uint32_t sp = ctx.r[kSp];
    wr32(ctx, sp + 16, rgb);
    wr32(ctx, sp + 20, static_cast<uint32_t>(ot));
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y + 1);
    ctx.r[kA2] = static_cast<uint32_t>(mode);
    ctx.r[kA3] = static_cast<uint32_t>(idx);
    f_80029F70(&ctx);  // direct call: 80029F70 is not overridden (no original-table entry)
    ctx.r[kA0] = save[0];
    ctx.r[kA1] = save[1];
    ctx.r[kA2] = save[2];
    ctx.r[kA3] = save[3];
}

struct Cursor {
    int x = 0, y = 0;      // pen
    int x0 = 0;            // line start
    int prop = 1;          // proportional mode (from arg or *s)
    int spacing = 0;       // *w letter spacing
    int clut = 0;          // *c text colour (starts at the clut argument)
    int h_extra = 0;       // *h extra line spacing
    int max_w = 0;         // max line width so far (relative)
};

/// Advance for an ASCII glyph (US width table): prop ? lo nibble : 6; space is 4/6.
int advance_of(uint8_t c, int prop) {
    if (c == 0x20) return prop ? 4 : 6;
    if (c < 0x20 || c > 0x7F) return 0;
    uint8_t w = g_widths[c - 0x20];
    return prop ? (w & 0xF) : 6;
}

enum class Step {
    Done,     // end of string
    Glyph,    // draw/measure an ASCII glyph
    Icon,     // inline icon via the JP icon function
    Newline,  // line break
    Skip,     // consumed bytes, nothing emitted
};

struct Item {
    Step step = Step::Skip;
    uint8_t ch = 0;    // Glyph: the character
    int mode = 0;      // Icon: icon mode
    int index = 0;     // Icon: icon index
    int advance = 0;   // Glyph/Icon: pen advance afterwards
};

/// A JP control code written bare (no '*'): one of a b c d e g h s w, then a digit, or h/w then
/// '-' and a digit. JP messages use these; an English name dropped into one keeps them.
bool jp_code_at(const Text& t, size_t s) {
    const uint8_t c = t.at(s), n = t.at(s + 1);
    const bool letter = c == 'a' || c == 'b' || c == 'c' || c == 'd' || c == 'e' || c == 'g' || c == 'h' ||
                        c == 's' || c == 'w';
    if (!letter) return false;
    if (n >= '0' && n <= '9') return true;
    return (c == 'h' || c == 'w') && n == '-' && t.at(s + 2) >= '0' && t.at(s + 2) <= '9';
}

/// One parser step over `*`-escaped US codes (US jump table at 0x800102c0). With `jp_codes`
/// (English inside a JP message), bare JP codes count too. `s` advances past the consumed bytes.
Item next_item(const Text& t, size_t& s, Cursor& cur, bool jp_codes = false) {
    Item it;
    uint8_t c = t.at(s);
    if (c == 0) {
        it.step = Step::Done;  // (Item defaults to Skip: without this the caller loops to its guard)
        return it;
    }
    if (c == 0x0A) {        // newline
        s += 1;
        it.step = Step::Newline;
        return it;
    }
    if (c == 0x5C && t.at(s + 1) == 'n') {  // backslash-n (MSD scripts)
        s += 2;
        it.step = Step::Newline;
        return it;
    }
    const bool bare_code = c != '*' && jp_codes && jp_code_at(t, s);
    if (c != '*' && !bare_code) {
        if (c < 0x20 || c > 0x7F) {  // unrecognized byte: skip (JP default branch)
            s += 1;
            return it;
        }
        s += 1;
        it.step = Step::Glyph;
        it.ch = c;
        it.advance = advance_of(c, cur.prop);
        return it;
    }
    if (!bare_code) s += 1;  // consume '*'
    uint8_t code = t.at(s);
    if (code == 0) {  // trailing '*'
        it.step = Step::Done;
        return it;
    }
    if (code == 0x0A) {        // * newline: line break (US fallback re-examines)
        s += 1;
        it.step = Step::Newline;
        return it;
    }
    if (code < 'a' || code > 'w') {
        s += 1;  // not a code letter: skip it (US table has 23 entries, a..w)
        return it;
    }
    uint8_t arg = t.at(s + 1);
    // A code cut short by the end of the string (a truncated "*e" in a fixed-size slot) ends it:
    // stepping over the NUL would draw whatever follows (the next effect line).
    const bool signed_arg = (code == 'h' || code == 'w') && arg == '-';
    if (arg == 0 || (signed_arg && t.at(s + 2) == 0)) {
        s += 1;
        it.step = Step::Done;
        return it;
    }
    switch (code) {
        case 'a': s += 2; it.step = Step::Icon; it.mode = 0; it.index = arg - '0'; it.advance = 12; return it;
        case 'b': s += 2; it.step = Step::Icon; it.mode = 0; it.index = arg - 0x29; it.advance = 12; return it;
        case 'c': s += 2; cur.clut = arg - '0'; return it;
        case 'd': s += 2; it.step = Step::Icon; it.mode = 0; it.index = arg - 0x1C; it.advance = 12; return it;
        case 'e': {
            s += 2;
            it.step = Step::Icon;
            it.mode = 0;
            it.index = (arg <= '3') ? arg - 0x23 : (arg == 'a' ? 0x11 : arg - 0x22);
            it.advance = 12;
            return it;
        }
        case 'g': s += 2; it.step = Step::Icon; it.mode = 2; it.index = arg - '0'; it.advance = 25; return it;
        case 'h': {  // *hN / *h-N: extra line spacing
            if (arg == '-') {
                cur.h_extra = -(t.at(s + 2) - '0');
                s += 3;
            } else {
                cur.h_extra = arg - '0';
                s += 2;
            }
            return it;
        }
        case 's': s += 2; cur.prop = arg - '0'; return it;
        case 'w': {  // *wN / *w-N: letter spacing
            if (arg == '-') {
                cur.spacing = -(t.at(s + 2) - '0');
                s += 3;
            } else {
                cur.spacing = arg - '0';
                s += 2;
            }
            return it;
        }
        default:  // unhandled letter: draw it (US fallback)
            s += 1;
            it.step = Step::Glyph;
            it.ch = code;
            it.advance = advance_of(code, cur.prop);
            return it;
    }
}

/// Shared parser: walks the string like US 80028d48/800293fc. When `draw` is true,
/// emits glyph SPRTs and icons; otherwise only measures. Sets g_text_w to the widest
/// line and g_text_h to the y accumulator + 12 (absolute for draw, relative for measure,
/// matching 8002B5FC and 8002B980) and returns the width.
/// `s`/`end` (non-zero): the piece of a mixed string to run; stop at `end` (a piece of a mixed string). `jp_codes`: bare JP codes count.
/// `final_clut` (optional): the colour in effect at the end.
int run_string(PsxContext& ctx, int x, int y, int clut, int prop, uint32_t rgb, int ot, const Text& t,
               bool draw, size_t s = 0, size_t end = 0, bool jp_codes = false, int* final_clut = nullptr) {
    Cursor cur;
    cur.x = cur.x0 = x;
    cur.y = y;
    cur.prop = prop;
    cur.clut = clut;
    int last_y = y;
    for (int guard = 0; guard < 4096; ++guard) {
        if (end != 0 && s >= end) break;
        Item it = next_item(t, s, cur, jp_codes);
        if (it.step == Step::Done) break;
        if (it.step == Step::Newline) {
            int w = cur.x - cur.x0;
            if (w > cur.max_w) cur.max_w = w;
            cur.x = cur.x0;
            cur.y += 13 + cur.h_extra;
            last_y = cur.y;
            continue;
        }
        if (it.step == Step::Skip) continue;
        if (it.step == Step::Icon) {
            if (draw) {
                if (prim_full(ctx)) break;
                emit_icon(ctx, cur.x, cur.y, it.mode, it.index, rgb, ot);
            }
            cur.x += it.advance + cur.spacing;
            int w = cur.x - cur.x0;
            if (w > cur.max_w) cur.max_w = w;
            continue;
        }
        // Glyph.
        if (draw) {
            if (prim_full(ctx)) break;
            int u = ((it.ch - 0x20) % kFontCols) * kCellW + (g_widths[it.ch - 0x20] >> 4);
            int v = ((it.ch - 0x20) / kFontCols) * kCellH;  // sheet row 0 = font row 48
            int w = cur.prop ? (g_widths[it.ch - 0x20] & 0xF) : kCellW;
            emit_sprt(ctx, cur.x, cur.y, u, v, w, kCellH, rgb, cur.clut, ot);
        }
        cur.x += it.advance + cur.spacing;
        int w = cur.x - cur.x0;
        if (w > cur.max_w) cur.max_w = w;
    }
    int tail = cur.x - cur.x0;
    if (tail > cur.max_w) cur.max_w = tail;
    if (final_clut) *final_clut = cur.clut;
    wr32(ctx, kTextW, static_cast<uint32_t>(cur.max_w));
    // Both functions write y_accumulator + 12: draw keeps the absolute pen (input y + line
    // advances), measure starts at 0. The dialog sizes itself from measure; the draw value
    // is the absolute bottom of the last line.
    wr32(ctx, kTextH, static_cast<uint32_t>((draw ? last_y : cur.y - y) + 12));
    return cur.max_w;
}

// ---------------------------------------------------------------------------------------------
// Mixed strings: English inside a JP message. The game builds them all the time (a deck name
// + "デック", a player name in a banner, colour codes around a name); the JP renderer cannot
// draw the English letters. They are cut into pieces: English runs go to the US port, the rest
// (SJIS, JP codes, digits) to the JP original, which gets a NUL-terminated copy of its piece
// on the guest stack; newlines are handled here, so each piece sits on one line.
// ---------------------------------------------------------------------------------------------

enum class PieceKind : uint8_t { English, Japanese, Newline };
struct Piece {
    size_t start, end;  // [start, end) in the Text
    PieceKind kind;
};

bool sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }
bool ascii_letter(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/// Cuts a mixed string into pieces. An ASCII run counts as English when it has two letters in a
/// row outside the JP codes; otherwise (codes, digits, spaces) it stays with the JP text.
std::vector<Piece> split_mixed(const Text& t) {
    std::vector<Piece> out;
    const auto add = [&](size_t a, size_t b, PieceKind k) {
        if (a >= b) return;
        if (k != PieceKind::Newline && !out.empty() && out.back().kind == k && out.back().end == a) {
            out.back().end = b;
            return;
        }
        out.push_back({a, b, k});
    };
    size_t s = 0;
    while (s < t.size()) {
        const uint8_t c = t.at(s);
        if (c == 0x0A) {
            add(s, s + 1, PieceKind::Newline);
            s += 1;
        } else if (c == 0x5C && t.at(s + 1) == 'n') {
            add(s, s + 2, PieceKind::Newline);
            s += 2;
        } else if (c >= 0x80) {
            const size_t n = sjis_lead(c) && t.at(s + 1) != 0 ? 2 : 1;
            add(s, s + n, PieceKind::Japanese);
            s += n;
        } else {
            size_t e = s;  // the ASCII run
            while (true) {
                const uint8_t d = t.at(e);
                if (d == 0 || d >= 0x80 || d == 0x0A || (d == 0x5C && t.at(e + 1) == 'n')) break;
                ++e;
            }
            bool english = false;
            for (size_t p = s; p + 1 < e && !english;) {
                if (jp_code_at(t, p)) {
                    p += t.at(p + 1) == '-' ? 3 : 2;
                    continue;
                }
                english = ascii_letter(t.at(p)) && ascii_letter(t.at(p + 1)) && !jp_code_at(t, p + 1);
                ++p;
            }
            add(s, e, english ? PieceKind::English : PieceKind::Japanese);
            s = e;
        }
    }
    return out;
}

/// The last colour code (c0-c9) in a JP piece, or `clut` if there is none.
int last_jp_clut(const Text& t, const Piece& p, int clut) {
    for (size_t s = p.start; s + 1 < p.end; ++s) {
        const uint8_t c = t.at(s);
        if (c >= 0x80) {
            s += sjis_lead(c) ? 1 : 0;
            continue;
        }
        if (c == 'c' && t.at(s + 1) >= '0' && t.at(s + 1) <= '9') clut = t.at(s + 1) - '0';
    }
    return clut;
}

/// One JP piece through the recompiled original (draw: x, y, clut, prop, rgb*, ot, str;
/// measure: prop, str). The piece is copied, NUL-terminated, into a frame pushed on the guest
/// stack, with the stack arguments below it. Returns its width.
int jp_piece(PsxContext& ctx, uint32_t addr, bool draw, int x, int y, int clut, int prop, uint32_t rgb, int ot,
             const Text& t, const Piece& p) {
    const uint32_t len = static_cast<uint32_t>(p.end - p.start);
    const uint32_t sp = ctx.r[kSp];
    const uint32_t frame = sp - ((32 + len + 1 + 7) & ~7u);
    const uint32_t str = frame + 32;
    for (uint32_t i = 0; i < len; ++i) wr8(ctx, str + i, t.at(p.start + i));
    wr8(ctx, str + len, 0);
    const uint32_t regs[4] = {ctx.r[kA0], ctx.r[kA1], ctx.r[kA2], ctx.r[kA3]};
    if (draw) {
        ctx.r[kA0] = static_cast<uint32_t>(x);
        ctx.r[kA1] = static_cast<uint32_t>(y);
        ctx.r[kA2] = static_cast<uint32_t>(clut);
        ctx.r[kA3] = static_cast<uint32_t>(prop);
        wr32(ctx, frame + 16, rgb);
        wr32(ctx, frame + 20, static_cast<uint32_t>(ot));
        wr32(ctx, frame + 24, str);
    } else {
        ctx.r[kA0] = static_cast<uint32_t>(prop);
        ctx.r[kA1] = str;
    }
    ctx.r[kSp] = frame;
    psx_call_original(&ctx, addr);
    ctx.r[kSp] = sp;
    const int w = static_cast<int>(ctx.r[kV0]);
    for (int i = 0; i < 4; ++i) ctx.r[kA0 + i] = regs[i];
    return w;
}

/// Draws or measures a mixed string piece by piece; sets g_text_w/h like the JP code. Returns
/// the width, or -1 when the string has no English piece (the caller uses the original).
int run_mixed(PsxContext& ctx, uint32_t jp_addr, bool draw, int x, int y, int clut, int prop, uint32_t rgb,
              int ot, const Text& t) {
    const std::vector<Piece> pieces = split_mixed(t);
    bool english = false;
    for (const Piece& p : pieces) english = english || p.kind == PieceKind::English;
    if (!english) return -1;
    int pen = x, line_y = draw ? y : 0, max_w = 0;
    for (const Piece& p : pieces) {
        if (p.kind == PieceKind::Newline) {
            max_w = std::max(max_w, pen - x);
            pen = x;
            line_y += 13;
            continue;
        }
        int w = 0;
        if (p.kind == PieceKind::English) {
            w = run_string(ctx, pen, line_y, clut, prop, rgb, ot, t, draw, p.start, p.end, true, &clut);
        } else {
            w = jp_piece(ctx, jp_addr, draw, pen, line_y, clut, prop, rgb, ot, t, p);
            clut = last_jp_clut(t, p, clut);
        }
        pen += w;
    }
    max_w = std::max(max_w, pen - x);
    wr32(ctx, kTextW, static_cast<uint32_t>(max_w));
    wr32(ctx, kTextH, static_cast<uint32_t>(line_y + 12));  // absolute for draw, relative for measure
    return max_w;
}

/// One text call: English (all ASCII after expansion) takes the US port, English inside JP the
/// piece path, anything else (or a missing en_font.bin) the JP original at `jp_addr`.
void dispatch(PsxContext* ctx, uint32_t jp_addr, bool draw, int x, int y, int clut, int prop, uint32_t rgb,
              int ot, uint32_t str) {
    if (!load_font(*ctx)) return psx_call_original(ctx, jp_addr);
    const Text t = load_text(*ctx, str);
    int w = 0;
    if (is_ascii(t)) {
        w = run_string(*ctx, x, y, clut, prop, rgb, ot, t, draw);
    } else {
        w = run_mixed(*ctx, jp_addr, draw, x, y, clut, prop, rgb, ot, t);
        if (w < 0) return psx_call_original(ctx, jp_addr);
    }
    ctx->r[kV0] = static_cast<uint32_t>(w);
}

}  // namespace

extern "C" {

namespace {

/// DCB_TRACE_TEXT=1: log every text-engine call (ASCII takes the US port, SJIS the JP
/// original) so a headless run shows which screens use the engine and with which strings.
void trace_call(PsxContext& ctx, const char* fn, uint32_t str, int x, int y) {
    if (!std::getenv("DCB_TRACE_TEXT")) return;
    bool ascii = true;
    for (uint32_t i = 0; i < 4096 && rd8(ctx, str + i); ++i) ascii = ascii && rd8(ctx, str + i) < 0x80;
    char buf[80];
    int n = 0;
    for (; n < 79; ++n) {
        uint8_t c = rd8(ctx, str + n);
        if (!c) break;
        buf[n] = static_cast<char>(c >= 0x20 && c < 0x7F ? c : '.');
    }
    buf[n] = 0;
    std::fprintf(stderr, "[text] %s %s \"%s\" at (%d,%d) str=%08X%s\n", fn, ascii ? "ascii" : "sjis",
                 buf, x, y, str, psx::backtrace_string(&ctx).c_str());
}

}  // namespace

// 8002ae00: draw(x, y, clut, prop, rgb*, ot, str). ASCII strings take the US port;
// SJIS strings (or a missing en_font.bin) go to the JP original.
void dcb_text_draw(PsxContext* ctx) {
    int x = static_cast<int>(ctx->r[kA0]);
    int y = static_cast<int>(ctx->r[kA1]);
    int clut = static_cast<int>(ctx->r[kA2]);
    int prop = static_cast<int>(ctx->r[kA3]);
    uint32_t sp = ctx->r[kSp];
    uint32_t rgb_p = psx_read32(ctx, sp + 16);
    int ot = static_cast<int>(psx_read32(ctx, sp + 20));
    uint32_t str = psx_read32(ctx, sp + 24);
    trace_call(*ctx, "draw", str, x, y);
    dispatch(ctx, kDrawAddr, true, x, y, clut, prop, rgb_p, ot, str);
}

// 8002b638: measure(prop, str). Same dispatch; sets g_text_w/h like the JP code.
void dcb_text_measure(PsxContext* ctx) {
    int prop = static_cast<int>(ctx->r[kA0]);
    uint32_t str = ctx->r[kA1];
    trace_call(*ctx, "measure", str, 0, 0);
    dispatch(ctx, kMeasureAddr, false, 0, 0, 0, prop, 0, 0, str);
}

// 8002bd3c: condensed draw. ASCII takes the normal US port (the US has no condensed
// ASCII); SJIS goes to the JP condensed original.
void dcb_text_draw_condensed(PsxContext* ctx) {
    int x = static_cast<int>(ctx->r[kA0]);
    int y = static_cast<int>(ctx->r[kA1]);
    int clut = static_cast<int>(ctx->r[kA2]);
    int prop = static_cast<int>(ctx->r[kA3]);
    uint32_t sp = ctx->r[kSp];
    uint32_t rgb_p = psx_read32(ctx, sp + 16);
    int ot = static_cast<int>(psx_read32(ctx, sp + 20));
    uint32_t str = psx_read32(ctx, sp + 24);
    trace_call(*ctx, "draw-cond", str, x, y);
    dispatch(ctx, kCondDrawAddr, true, x, y, clut, prop, rgb_p, ot, str);
}

// 8002c574: condensed measure (prop, str). Same dispatch.
void dcb_text_measure_condensed(PsxContext* ctx) {
    int prop = static_cast<int>(ctx->r[kA0]);
    uint32_t str = ctx->r[kA1];
    trace_call(*ctx, "measure-cond", str, 0, 0);
    dispatch(ctx, kCondMeasureAddr, false, 0, 0, 0, prop, 0, 0, str);
}

}  // extern "C"
