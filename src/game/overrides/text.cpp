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

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <cstdint>
#include <cstdio>
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

// Option-B VRAM area for the US font rows: 64 halfwords x 176 rows. Must stay clear of the
// framebuffers at (0,0)-(319,479) — the frame background is redrawn over that area every
// frame, which destroyed an earlier (0,320) placement — and of the JP glyph cache at
// (960,0). (512,320)-(575,495) was empty in the full-VRAM survey (docs/re/text-engine.md);
// the upload repeats on every ASCII draw so a later screen texture cannot break text.
constexpr int kFontVramX = 512;   // halfwords
constexpr int kFontVramY = 320;  // pixel rows

uint8_t rd8(PsxContext& ctx, uint32_t a) { return psx_read8(&ctx, a); }
uint16_t rd16(PsxContext& ctx, uint32_t a) { return psx_read16(&ctx, a); }
uint32_t rd32(PsxContext& ctx, uint32_t a) { return psx_read32(&ctx, a); }
void wr8(PsxContext& ctx, uint32_t a, uint8_t v) { psx_write8(&ctx, a, v); }
void wr16(PsxContext& ctx, uint32_t a, uint16_t v) { psx_write16(&ctx, a, v); }
void wr32(PsxContext& ctx, uint32_t a, uint32_t v) { psx_write32(&ctx, a, v); }

// en_font.bin payload (no game data in git; built on the player's machine).
std::vector<uint8_t> g_font_rows;  // 176 rows x 128 bytes (4 bpp, 64 halfwords/row)
uint8_t g_widths[96] = {0};        // width[c - 0x20]: hi nibble u offset, lo nibble advance
bool g_font_loaded = false;

/// Load en_font.bin once (rows + width table). Returns false when the file is missing
/// (caller falls back to JP rendering). Upload happens separately, every draw call.
bool load_font() {
    if (g_font_loaded) return !g_font_rows.empty();
    g_font_loaded = true;
    FILE* f = std::fopen("assets/SLPS-03101/en_font.bin", "rb");
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
    if (!ok) {
        g_font_rows.clear();
        return false;
    }
    return true;
}

/// Re-upload the US font rows into the option-B VRAM area (64x176 halfwords). Called on
/// every ASCII draw: the framebuffer and other screens may repaint the area between draws,
/// so a one-time upload is not enough.
void upload_font(PsxContext& ctx) {
    static std::vector<uint16_t> pixels;
    if (pixels.empty()) {
        pixels.resize(static_cast<size_t>(kFontRows) * 64);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint16_t>(g_font_rows[i * 2] | (g_font_rows[i * 2 + 1] << 8));
    }
    auto* mmio = static_cast<hle::Mmio*>(psx::Machine::from(&ctx).mmio());
    if (!mmio) return;
    mmio->gpu().upload_rect(kFontVramX, kFontVramY, 64, kFontRows, pixels.data());
}

/// True when the NUL-terminated guest string is plain 7-bit text (our grafted ASCII).
/// Any high byte means SJIS or half-width kana: fall back to the JP original.
bool is_ascii(PsxContext& ctx, uint32_t s) {
    for (int i = 0; i < 4096; ++i) {
        uint8_t c = rd8(ctx, s + i);
        if (c == 0) return true;
        if (c >= 0x80) return false;
    }
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

/// E1 texpage word for the option-B font area. Page bits come from our VRAM position
/// (x/64, y/256); the game sources semi/depth bits from the draw mode, which the packet's
/// semi flag (byte 15, bit 1) selects at draw time — the SPRT command carries them, so the
/// E1 here only needs the page. 4 bpp (depth 0), page (0, 1) for (0, 320).
uint16_t tpage_of(PsxContext& /*ctx*/) {
    return static_cast<uint16_t>((kFontVramX / 64) | ((kFontVramY / 256) << 4));
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
    uint16_t tpage = tpage_of(ctx);
    wr32(ctx, p + 0x04, 0xE1000000u | (tpage & 0x9FFu));
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

/// One parser step over `*`-escaped US codes (US jump table at 0x800102c0).
/// `s` advances past the consumed bytes.
Item next_item(PsxContext& ctx, uint32_t& s, Cursor& cur) {
    Item it;
    uint8_t c = rd8(ctx, s);
    if (c == 0) return it;  // Done
    if (c == 0x0A) {        // newline
        s += 1;
        it.step = Step::Newline;
        return it;
    }
    if (c == 0x5C && rd8(ctx, s + 1) == 'n') {  // backslash-n (MSD scripts)
        s += 2;
        it.step = Step::Newline;
        return it;
    }
    if (c != '*') {
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
    s += 1;  // consume '*'
    uint8_t code = rd8(ctx, s);
    if (code == 0) return it;  // trailing '*': Done
    if (code == 0x0A) {        // * newline: line break (US fallback re-examines)
        s += 1;
        it.step = Step::Newline;
        return it;
    }
    if (code < 'a' || code > 'w') {
        s += 1;  // not a code letter: skip it (US table has 23 entries, a..w)
        return it;
    }
    uint8_t arg = rd8(ctx, s + 1);
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
                cur.h_extra = -(rd8(ctx, s + 2) - '0');
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
                cur.spacing = -(rd8(ctx, s + 2) - '0');
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
int run_string(PsxContext& ctx, int x, int y, int clut, int prop, uint32_t rgb, int ot,
               uint32_t s, bool draw) {
    Cursor cur;
    cur.x = cur.x0 = x;
    cur.y = y;
    cur.prop = prop;
    cur.clut = clut;
    int last_y = y;
    for (int guard = 0; guard < 4096; ++guard) {
        Item it = next_item(ctx, s, cur);
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
            int v = kFontVramY + ((it.ch - 0x20) / kFontCols) * kCellH;
            int w = cur.prop ? (g_widths[it.ch - 0x20] & 0xF) : kCellW;
            emit_sprt(ctx, cur.x, cur.y, u, v, w, kCellH, rgb, cur.clut, ot);
        }
        cur.x += it.advance + cur.spacing;
        int w = cur.x - cur.x0;
        if (w > cur.max_w) cur.max_w = w;
    }
    int tail = cur.x - cur.x0;
    if (tail > cur.max_w) cur.max_w = tail;
    wr32(ctx, kTextW, static_cast<uint32_t>(cur.max_w));
    // Both functions write y_accumulator + 12: draw keeps the absolute pen (input y + line
    // advances), measure starts at 0. The dialog sizes itself from measure; the draw value
    // is the absolute bottom of the last line.
    wr32(ctx, kTextH, static_cast<uint32_t>((draw ? last_y : cur.y - y) + 12));
    return cur.max_w;
}

}  // namespace

extern "C" {

namespace {

/// DCB_TRACE_TEXT=1: log every text-engine call (ASCII takes the US port, SJIS the JP
/// original) so a headless run shows which screens use the engine and with which strings.
void trace_call(PsxContext& ctx, const char* fn, uint32_t str, int x, int y) {
    if (!std::getenv("DCB_TRACE_TEXT")) return;
    bool ascii = is_ascii(ctx, str);
    char buf[80];
    int n = 0;
    for (; n < 79; ++n) {
        uint8_t c = rd8(ctx, str + n);
        if (!c) break;
        buf[n] = static_cast<char>(c >= 0x20 && c < 0x7F ? c : '.');
    }
    buf[n] = 0;
    std::fprintf(stderr, "[text] %s %s \"%s\" at (%d,%d) str=%08X\n", fn, ascii ? "ascii" : "sjis",
                 buf, x, y, str);
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
    if (!is_ascii(*ctx, str) || !load_font()) return psx_call_original(ctx, kDrawAddr);
    upload_font(*ctx);
    ctx->r[kV0] = static_cast<uint32_t>(run_string(*ctx, x, y, clut, prop, rgb_p, ot, str, true));
}

// 8002b638: measure(prop, str). Same dispatch; sets g_text_w/h like the JP code.
void dcb_text_measure(PsxContext* ctx) {
    int prop = static_cast<int>(ctx->r[kA0]);
    uint32_t str = ctx->r[kA1];
    trace_call(*ctx, "measure", str, 0, 0);
    if (!is_ascii(*ctx, str) || !load_font()) return psx_call_original(ctx, kMeasureAddr);
    ctx->r[kV0] = static_cast<uint32_t>(run_string(*ctx, 0, 0, 0, prop, 0, 0, str, false));
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
    if (!is_ascii(*ctx, str) || !load_font()) return psx_call_original(ctx, kCondDrawAddr);
    upload_font(*ctx);
    ctx->r[kV0] = static_cast<uint32_t>(run_string(*ctx, x, y, clut, prop, rgb_p, ot, str, true));
}

// 8002c574: condensed measure (prop, str). Same dispatch.
void dcb_text_measure_condensed(PsxContext* ctx) {
    int prop = static_cast<int>(ctx->r[kA0]);
    uint32_t str = ctx->r[kA1];
    trace_call(*ctx, "measure-cond", str, 0, 0);
    if (!is_ascii(*ctx, str) || !load_font()) return psx_call_original(ctx, kCondMeasureAddr);
    ctx->r[kV0] = static_cast<uint32_t>(run_string(*ctx, 0, 0, 0, prop, 0, 0, str, false));
}

}  // extern "C"
