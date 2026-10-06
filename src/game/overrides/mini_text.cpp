// English in the JP mini font (8x7 cells, SYSTEM.TIM rows 0-104; docs/re/text-engine.md §2.3).
//
// Two functions, registered in config/SLPS-03101/overrides.json:
//
//   sjis_to_mini (8002A37C, a0 src, a1 dst): converts a Shift-JIS string to the mini encoding
//     (kana -> half-width, full-width A-Z/0-9 -> ASCII, bare a-e codes -> 01 N) and drops every
//     other ASCII byte. Its callers pass a card name (card+3: the battle card panel 8003C200 into
//     a 40-byte stack buffer, SUBSEG 801E6AC4 / 801E78A4 into sp+32 buffers of 64 bytes) and draw
//     the result right after. The names are English now (US CARD2.CDD grafted), so they came out
//     blank. An all-ASCII source is copied through (bounded to the 21-byte name slot; bytes below
//     0x20, such as a long-name tag, are dropped); anything else takes the original.
//     Name slots (kSlots): the name is folded to capitals. The battle card panel draws it in the
//     4x5 micro font at the US position, like the US build (below); the Edit Partner panels keep
//     the mini font (the US drew those in its 6x6 font, not micro), in tight spacing if a name
//     would be wider than its slot. The font, the slot's area (the name moves left when it would
//     overrun), the spacing and the offsets are handed to the draw that follows (g_pending), not
//     written into the string.
//
//   text_draw_mini (800288C8, a0 x, a1 y, a2 str, a3 clut, sp+16 rgb*, sp+20 ot). Its grey
//     wrapper 80028898 calls it with a jal the recompiler routes here, so both are covered. The
//     string is copied to the host and translated whole when the text catalog knows it (the
//     label table 0x80071058 and the SUBSEG button hints are in catalog-exe.txt /
//     catalog-subseg.txt, two labels shortened in en-mini.tsv; a label in brackets, "(%s)" on
//     the partner screen, is translated inside them), then:
//       - still Japanese (a byte >= 0x80 or the kana toggle '~'): the original, untouched;
//       - English words (any lowercase letter): drawn proportionally, one glyph per call of the
//         original on a 2-byte guest-stack string, each glyph trimmed to its ink columns (read
//         from the SYSTEM.TIM mini cells in VRAM) with a 1-pixel gap, space 3 px, no space
//         right after an icon (tight spacing for a card name that sjis_to_mini found too wide).
//         The mini font already has full ASCII incl. lowercase, so no new art is needed. Fixed
//         8-px cells made English 40-60% wider than the kana it replaces (":Cursor" 56 px in a
//         ~52 px help panel, card names up to 20 letters = 160 px); proportional ink widths
//         bring most of them back to the JP widths;
//       - a card name fitted by sjis_to_mini (capitals): proportionally as above, at its place;
//       - other capitals only (a translated "FULL SET!", the JP disc's "RANK UP!", "L1      "):
//         the original on a guest-stack copy, keeping the fixed cells (padding by spaces
//         depends on them).
//
//   Micro font (4x5 capitals), as the US battle card panel: the US build draws the card name
//   (US 800398A0, JP 8003C230) and the support label under the attack rows (US 80039994 /
//   8003AA88, JP 8003C32C / 8003D43C) with its micro renderer (US 80027DB8 / 80027DE8, JP
//   80027EC4 / 80027EF4) instead of the mini font. Those draws (known by the return address)
//   are laid out by mini::micro_layout (folded capitals, 5 px cells, 5x5 icons) at the US
//   position and drawn glyph by glyph with the JP micro renderer; an icon (01 N) is the JP
//   text_icon's packet pointed at the US icon cell in the private sheet (US-only art).
//
// DCB_TRACE_TEXT=1 / hex logs each mini draw like the main renderer's trace ("[text] mini ...",
// "micro" for the micro draws), each fitted card name ("[text] mini-name ...") and the measured
// ink table ("[text] mini ink").

#include "gpu/gpu.hpp"
#include "hw/mmio.hpp"
#include "mini_fit.hpp"
#include "text.hpp"

#include <psx/backtrace.hpp>
#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
void dcb_text_icon(PsxContext* ctx);  // text_icon 80029F70 through its override (level_badges.cpp)
void f_80027EF4(PsxContext* ctx);     // text_draw_micro(x, y, str, clut, rgb* @sp16, ot @sp20)
}

namespace {

namespace mini = dcb::mini;

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kS5 = 21, kSp = 29, kRa = 31;
constexpr uint32_t kMiniDraw = 0x800288C8u;
constexpr uint32_t kMiniGreyRet = 0x800288B8u;  // return address of the grey wrapper's (80028898) call
constexpr uint32_t kToMini = 0x8002A37Cu;
constexpr uint32_t kTextPrim = 0x801D9714u;     // g_text_prim: next free text primitive
constexpr uint32_t kPrimSize = 0x1C;            // DR_TPAGE (2 words) + SPRT (5 words)
constexpr int kSheetFirstRow = 48;              // private sheet row 0 = TIM row 48 (text.cpp)
constexpr uint32_t kSysTimX = 0x801D9704u;  // SYSTEM.TIM VRAM x (u16, halfwords)
constexpr uint32_t kSysTimY = 0x801D9706u;  // SYSTEM.TIM VRAM y (u16)
constexpr size_t kNameMax = 20;             // card name slot: 21 bytes with the NUL
constexpr int kCell = 7;                    // mini glyph sprite: 7x7 in an 8x7 cell

/// Card-name slots, by the return address of the sjis_to_mini call (each draws the name right
/// after). The name is drawn in capitals (mini::fold_upper); its ink may use the columns
/// [x + min_dx, x + end_dx] of the draw's x (the caller's x + dx): a name that ends by x + end_dx
/// stays at x, a wider one moves left (mini::place). Measured on headless snapshots
/// (docs/re/text-engine.md §7.10).
struct Area {
    int min_dx, end_dx;  // first / last column the ink may use, from the draw's x
    int dx;              // the draw's x from the caller's x
    /// Widest pen advance (measure(): the last glyph's gap included) that fits from min_dx.
    constexpr int budget() const { return end_dx - min_dx + 2; }
};
struct Slot {
    uint32_t ra;
    Area area[2];  // by side: the battle panel's P1 / P2 (s5 at the call); else area[0]
    bool sided;
    bool micro;  // drawn in the micro font (else mini, proportional)
    const char* what;
};
constexpr Slot kSlots[] = {
    // Battle card panel (8003C200): the name bar between the DP label and the DP box. The JP x is
    // panel x + 32 - 21 * side (s5, 0 = P1), the US x (800398A0) panel x + 17 - 14 * side at the
    // same y (panel y + 2), in the micro font: P1 (left, blue) 57 at rest, right after the DP
    // label's separator (55), P2 (right, orange) 167, the first column of the bar's fill. The ink
    // may use the bar's fill: P1 56 .. 150 (border 151 .. 153), P2 167 .. 263 (DP box border at
    // 264): budgets 96 / 98; 19 capitals (HERCULESKABUTERIMON, the longest name) are 95, ink 94
    // (57 .. 150 on P1), so every name keeps the US x (only Digimon reach this slot). The bar's fill
    // is rows 93 .. 99, the micro capitals rows y .. y + 4 = 94 .. 98: centred.
    {0x8003C208u, {{-1, 93, -15}, {0, 96, -8}}, true, true, "battle panel"},
    // SUBSEG 801E6AC4 (Edit Partner, partner panel, 801E65E8): name at x 59 (y 49), nothing
    // after it on its row up to the panel's inside end at 205 (border at 206).
    {0x801E6ACCu, {{0, 146, 0}, {0, 146, 0}}, false, false, "partner"},
    // SUBSEG 801E78A4 (Edit Partner, armor panel, 801E73BC): name at the panel's x + 3 = 217,
    // the panel's inside 212 .. 303 (borders at 211 and 304).
    {0x801E78ACu, {{-5, 86, 0}, {-5, 86, 0}}, false, false, "armor"},
};

/// The support labels under the battle card panel's attack rows (the half-width label table
/// 0x80071058 by card+0xE4), drawn in the micro font at the US position: by the caller's return
/// address (through the grey wrapper for the first), x offset (+ step per side, s5) and y offset.
struct MicroSite {
    uint32_t ra;
    int dx, dx_side, dy;
    const char* what;
};
constexpr MicroSite kMicroSites[] = {
    // 8003C32C (grey): JP panel x + 22 * side + 25, y + 50; US 80039994: x + 24 * side + 24, y + 51.
    {0x8003C334u, -1, 2, 1, "battle panel label"},
    // 8003D43C: JP x + 68, y + 63; US 8003AA88: x + 68, y + 64.
    {0x8003D444u, 0, 0, 1, "battle label"},
};

std::string read_string(PsxContext& ctx, uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint8_t c = psx_read8(&ctx, addr + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

bool trace_on() { return std::getenv("DCB_TRACE_TEXT") != nullptr; }

/// Printable form of a mini string (codes as dots).
std::string shown(const std::string& raw) {
    std::string out;
    for (const char c : raw) out += (c >= 0x20 && c < 0x7F) ? c : '.';
    return out;
}

/// DCB_TRACE_TEXT: one line per mini call, like text.cpp's trace (hex: raw bytes + catalog hit).
void trace(PsxContext& ctx, const char* fn, const std::string& raw, bool translated, const char* path,
           int x, int y, uint32_t str) {
    if (!trace_on()) return;
    std::string extra;
    if (std::strcmp(std::getenv("DCB_TRACE_TEXT"), "hex") == 0) {
        extra = translated ? " catalog=yes hex=" : " catalog=no hex=";
        static const char* kHex = "0123456789abcdef";
        for (const char c : raw) {
            extra += kHex[static_cast<uint8_t>(c) >> 4];
            extra += kHex[static_cast<uint8_t>(c) & 15];
        }
    }
    std::fprintf(stderr, "[text] %s %s \"%s\" at (%d,%d) str=%08X%s%s\n", fn, path, shown(raw).c_str(), x, y, str,
                 extra.c_str(), psx::backtrace_string(&ctx).c_str());
}

// Ink columns of the mini ASCII glyphs (0x20..0x7F), read from SYSTEM.TIM in VRAM: glyph c sits at
// u = (c & 15) * 8, v = (c - 0x20) / 16 * 7 of the 4-bpp sheet (the draw function's formula).
mini::InkTable g_ink{};
bool g_ink_ready = false;

void load_ink(PsxContext& ctx) {
    if (g_ink_ready) return;
    auto* mmio = static_cast<hle::Mmio*>(psx::Machine::from(&ctx).mmio());
    if (!mmio) return;
    const uint16_t* vram = mmio->gpu().vram();
    const int sx = psx_read16(&ctx, kSysTimX) & 0x3FF, sy = psx_read16(&ctx, kSysTimY) & 0x1FF;
    const auto texel = [&](int u, int v) {
        const uint16_t h = vram[static_cast<size_t>(((sy + v) & 511) * 1024 + ((sx + u / 4) & 1023))];
        return (h >> ((u & 3) * 4)) & 15;
    };
    const auto rows = [&](int u, int v0) {
        uint8_t m = 0;
        for (int v = 0; v < kCell; ++v)
            if (texel(u, v0 + v)) m = static_cast<uint8_t>(m | (1u << v));
        return m;
    };
    int inked = 0;
    for (int c = 0x21; c < 0x80; ++c) {
        const int u0 = (c & 15) * 8, v0 = (c - 0x20) / 16 * kCell;
        int l = kCell, r = -1;
        for (int u = 0; u < kCell; ++u)
            if (rows(u0 + u, v0)) {
                l = std::min(l, u);
                r = std::max(r, u);
            }
        if (r >= 0) {
            g_ink[static_cast<size_t>(c - 0x20)] = {static_cast<int8_t>(l), static_cast<int8_t>(r - l + 1),
                                                    rows(u0 + l, v0), rows(u0 + r, v0)};
            ++inked;
        }
    }
    // SYSTEM.TIM not uploaded yet (the letters would be blank): measure again next time.
    g_ink_ready = inked > 40;
    // DCB_TRACE_TEXT: the measured table, one "c:left,width,left_rows,right_rows" per glyph.
    if (g_ink_ready && trace_on()) {
        std::fprintf(stderr, "[text] mini ink");
        for (int c = 0x21; c < 0x80; ++c) {
            const mini::Ink& g = g_ink[static_cast<size_t>(c - 0x20)];
            std::fprintf(stderr, " %02X:%d,%d,%02X,%02X", c, g.left, g.width, g.left_rows, g.right_rows);
        }
        std::fputc('\n', stderr);
    }
}

/// The ink table in use: the measured one, or 7-px cells before SYSTEM.TIM is in VRAM.
const mini::InkTable& ink() {
    static const mini::InkTable fallback = [] {
        mini::InkTable t{};
        for (size_t i = 1; i < t.size(); ++i) t[i] = {0, 7, 0x7F, 0x7F};
        return t;
    }();
    return g_ink_ready ? g_ink : fallback;
}

/// The fitted name sjis_to_mini wrote for the draw that follows: its buffer, text, font,
/// spacing, width and the slot area of its side.
struct Pending {
    uint32_t dst = 0;
    std::string text;
    bool micro = false;
    mini::Spacing spacing = mini::Spacing::Normal;
    int width = 0;
    Area area{0, 0, 0};
};
Pending g_pending;

/// Calls the original mini draw (or the micro draw, 80027EF4) with `s` copied, NUL-terminated,
/// into a frame pushed on the guest stack (the rgb pointer and OT at +16/+20 as the JP convention
/// wants).
void call_mini(PsxContext& ctx, int x, int y, const std::string& s, int clut, uint32_t rgb, int ot,
               bool micro = false) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    const uint32_t sp = ctx.r[kSp];
    const uint32_t frame = sp - ((24 + len + 1 + 7) & ~7u);
    const uint32_t str = frame + 24;
    for (uint32_t i = 0; i < len; ++i) psx_write8(&ctx, str + i, static_cast<uint8_t>(s[i]));
    psx_write8(&ctx, str + len, 0);
    psx_write32(&ctx, frame + 16, rgb);
    psx_write32(&ctx, frame + 20, static_cast<uint32_t>(ot));
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y);
    ctx.r[kA2] = str;
    ctx.r[kA3] = static_cast<uint32_t>(clut);
    ctx.r[kSp] = frame;
    if (micro)
        f_80027EF4(&ctx);
    else
        psx_call_original(&ctx, kMiniDraw);
    ctx.r[kSp] = sp;
}

/// Mini icon (code 01 N): text_icon(x, y, mode 1, N - 1), like the original.
void mini_icon(PsxContext& ctx, int x, int y, int idx, uint32_t rgb, int ot) {
    const uint32_t sp = ctx.r[kSp];
    const uint32_t frame = sp - 24;
    psx_write32(&ctx, frame + 16, rgb);
    psx_write32(&ctx, frame + 20, static_cast<uint32_t>(ot));
    ctx.r[kA0] = static_cast<uint32_t>(x);
    ctx.r[kA1] = static_cast<uint32_t>(y);
    ctx.r[kA2] = 1;
    ctx.r[kA3] = static_cast<uint32_t>(idx);
    ctx.r[kSp] = frame;
    dcb_text_icon(&ctx);  // like the original, whose jal the recompiler routes there
    ctx.r[kSp] = sp;
}

/// English in the mini font with proportional spacing (see the file comment and mini_fit.hpp).
/// Returns the width of the widest line.
int draw_proportional(PsxContext& ctx, int x, int y, const std::string& s, int clut, uint32_t rgb, int ot,
                      mini::Spacing spacing) {
    load_ink(ctx);
    const mini::InkTable& table = ink();
    return mini::layout(table, s, spacing, x, y, [&](const mini::Item& it) {
        switch (it.kind) {
            case mini::Item::Glyph:
                call_mini(ctx, it.x - table[it.code - 0x20].left, it.y, std::string(1, static_cast<char>(it.code)),
                          clut, rgb, ot);
                break;
            case mini::Item::Icon:
                mini_icon(ctx, it.x, it.y, it.code - 1, rgb, ot);
                break;
            case mini::Item::Colour:
                clut = it.code;
                break;
        }
    });
}

/// Micro icon (code 01 N, idx N - 1), as the US micro renderer: a 5x5 cell of US text_icon mode 3.
/// The JP text_icon has no such mode; its mode 1 (the mini icons: 7x7, the same CLUT row) writes
/// the packet, which is then pointed at the US cell in the private sheet (texpage word -> sheet
/// marker, as level_badges.cpp does).
void micro_icon(PsxContext& ctx, int x, int y, int idx, uint32_t rgb, int ot) {
    const uint32_t p = psx_read32(&ctx, kTextPrim);
    mini_icon(ctx, x, y, idx, rgb, ot);
    if (psx_read32(&ctx, kTextPrim) != p + kPrimSize) return;  // pool full: nothing drawn
    const mini::IconCell cell = mini::micro_icon_cell(idx);
    const uint32_t tpage = psx_read32(&ctx, p + 0x04);
    psx_write32(&ctx, p + 0x04, hle::Gpu::kSheetMarker | ((tpage >> 5) & 3u));  // keep the semi mode
    psx_write8(&ctx, p + 0x14, static_cast<uint8_t>(cell.u));
    psx_write8(&ctx, p + 0x15, static_cast<uint8_t>(cell.v - kSheetFirstRow));
    psx_write16(&ctx, p + 0x18, mini::kMicroIconSize);
    psx_write16(&ctx, p + 0x1A, mini::kMicroIconSize);
}

/// `s` in the micro font from (x, y), as the US micro renderer (mini::micro_layout): each glyph
/// through the JP micro renderer, icons through micro_icon. Returns the width.
int draw_micro(PsxContext& ctx, int x, int y, const std::string& s, int clut, uint32_t rgb, int ot) {
    return mini::micro_layout(s, x, y, [&](const mini::Item& it) {
        switch (it.kind) {
            case mini::Item::Glyph:
                call_mini(ctx, it.x, it.y, std::string(1, static_cast<char>(it.code)), clut, rgb, ot, true);
                break;
            case mini::Item::Icon:
                micro_icon(ctx, it.x, it.y, it.code - 1, rgb, ot);
                break;
            case mini::Item::Colour:
                clut = it.code;
                break;
        }
    });
}

bool japanese(const std::string& s) {
    for (const char c : s)
        if (static_cast<uint8_t>(c) >= 0x80 || c == '~') return true;
    return false;
}

bool has_lowercase(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c == 0x01 || c == 0x0C) {  // code + argument byte
            ++i;
            continue;
        }
        if (c >= 'a' && c <= 'z') return true;
    }
    return false;
}

/// An ASCII card name in the mini encoding: US icon codes in item names ("Defense Disk *b0"),
/// *a..*e + digit, become the mini icon code 01 N with the original's mapping for the bare JP
/// codes (b0 -> 01 08); bytes below 0x20 (long-name tag) and '~' (kana bank toggle) are dropped.
std::string ascii_to_mini(const std::string& src) {
    std::string out;
    const auto at = [&](size_t i) { return i < src.size() ? static_cast<uint8_t>(src[i]) : uint8_t{0}; };
    for (size_t i = 0; i < src.size(); ++i) {
        const uint8_t c = at(i), code = at(i + 1), arg = at(i + 2);
        if (c == '*' && code >= 'a' && code <= 'e' && arg >= '0' && arg <= '9') {
            static constexpr uint8_t kBase[] = {0x2F, 0x28, 0x30, 0x1B, 0x22};  // a b c d e
            out += '\x01';
            out += static_cast<char>(arg - kBase[code - 'a']);
            i += 2;
            continue;
        }
        if (c < 0x20 || c == '~') continue;
        out += static_cast<char>(c);
    }
    return out;
}

}  // namespace

extern "C" {

// 800288C8: text_draw_mini(x, y, str, clut, rgb*, ot).
void dcb_text_draw_mini(PsxContext* ctx) {
    int x = static_cast<int>(ctx->r[kA0]);
    int y = static_cast<int>(ctx->r[kA1]);
    const uint32_t str = ctx->r[kA2];
    const int clut = static_cast<int>(ctx->r[kA3]);
    const uint32_t sp = ctx->r[kSp];
    const uint32_t rgb = psx_read32(ctx, sp + 16);
    const int ot = static_cast<int>(psx_read32(ctx, sp + 20));

    const std::string raw = read_string(*ctx, str);
    // A card name fitted by sjis_to_mini just before (same buffer, same text): its font, spacing
    // and place in the slot.
    mini::Spacing spacing = mini::Spacing::Normal;
    const bool fitted = g_pending.dst != 0 && g_pending.dst == str && g_pending.text == raw;
    const bool micro_name = fitted && g_pending.micro;
    if (fitted) {
        spacing = g_pending.spacing;
        const Area& a = g_pending.area;
        x += a.dx;
        x = mini::place(x, g_pending.width, x + a.min_dx, x + a.end_dx);
    }
    g_pending = {};
    // A support label the US draws in the micro font (the caller through the grey wrapper: its
    // return address is in the wrapper's frame).
    const uint32_t caller = ctx->r[kRa] == kMiniGreyRet ? psx_read32(ctx, sp + 24) : ctx->r[kRa];
    const MicroSite* site = nullptr;
    for (const MicroSite& m : kMicroSites)
        if (m.ra == caller) site = &m;
    std::string s;
    bool translated = !fitted && dcb::text_translate(*ctx, raw, s);
    // The partner screen (SUBSEG 801E58F8) draws a support label in brackets, "(%s)": the
    // catalog knows the label, so translate inside them.
    if (!fitted && !translated && raw.size() > 2 && raw.front() == '(' && raw.back() == ')') {
        std::string inner;
        translated = dcb::text_translate(*ctx, raw.substr(1, raw.size() - 2), inner);
        if (translated) s = "(" + inner + ")";
    }
    if (!translated) s = raw;
    const uint32_t regs[4] = {ctx->r[kA0], ctx->r[kA1], ctx->r[kA2], ctx->r[kA3]};
    if (japanese(s)) {
        trace(*ctx, "mini", raw, translated, "jp", x, y, str);
        psx_call_original(ctx, kMiniDraw);
        return;
    }
    if (micro_name || (site && translated)) {
        if (site) {
            x += site->dx + site->dx_side * static_cast<int>(ctx->r[kS5] & 1);
            y += site->dy;
        }
        const int w = draw_micro(*ctx, x, y, s, clut, rgb, ot);
        trace(*ctx, "mini", raw, translated, ("micro w=" + std::to_string(w)).c_str(), x, y, str);
    } else if (has_lowercase(s) || fitted) {
        const int w = draw_proportional(*ctx, x, y, s, clut, rgb, ot, spacing);
        const std::string path =
            std::string(spacing == mini::Spacing::Tight ? "tight w=" : "prop w=") + std::to_string(w);
        trace(*ctx, "mini", raw, translated, path.c_str(), x, y, str);
    } else if (translated) {
        trace(*ctx, "mini", raw, translated, "fixed", x, y, str);
        call_mini(*ctx, x, y, s, clut, rgb, ot);
    } else {
        trace(*ctx, "mini", raw, translated, "fixed", x, y, str);
        psx_call_original(ctx, kMiniDraw);
        return;
    }
    for (int i = 0; i < 4; ++i) ctx->r[kA0 + i] = regs[i];
}

// 8002A37C: sjis_to_mini(src, dst) -> output length. ASCII (the grafted English card names) is
// copied through, bounded to the name slot, in capitals for the card-name slots; Shift-JIS takes
// the original.
void dcb_sjis_to_mini(PsxContext* ctx) {
    const uint32_t src = ctx->r[kA0], dst = ctx->r[kA1], ra = ctx->r[kRa];
    bool ascii = true;
    size_t n = 0;
    for (; n <= kNameMax; ++n) {
        const uint8_t c = psx_read8(ctx, src + static_cast<uint32_t>(n));
        if (c == 0) break;
        if (c >= 0x80) ascii = false;
    }
    if (!ascii || n == 0) return psx_call_original(ctx, kToMini);
    std::string name;
    for (size_t i = 0; i < std::min(n, kNameMax); ++i)
        name += static_cast<char>(psx_read8(ctx, src + static_cast<uint32_t>(i)));
    // [text] names: the JP Digimon names (all within the US names' 19 letters); one longer than
    // the 21-byte name buffer the result is written to keeps the US name.
    if (std::string swapped = name; (dcb::text_swap_names(swapped), swapped.size() <= kNameMax)) name = swapped;

    std::string out = ascii_to_mini(name);
    g_pending = {};
    for (const Slot& slot : kSlots) {
        if (slot.ra != ra) continue;
        out = mini::fold_upper(out);
        const int side = slot.sided ? static_cast<int>(ctx->r[kS5] & 1) : 0;
        const Area& area = slot.area[side];
        mini::Fit fit{out, mini::Spacing::Normal, 0};
        if (slot.micro) {
            fit.width = mini::micro_measure(out);
        } else {
            load_ink(*ctx);
            fit = mini::fit_name(ink(), out, area.budget());
        }
        if (trace_on())
            std::fprintf(stderr, "[text] mini-name %s side %d src=%08X \"%s\" %s w=%d (slot %d)\n", slot.what, side,
                         src, shown(out).c_str(),
                         slot.micro ? "micro" : fit.spacing == mini::Spacing::Tight ? "tight" : "normal", fit.width,
                         area.budget());
        g_pending = {dst, out, slot.micro, fit.spacing, fit.width, area};
        break;
    }
    for (size_t i = 0; i < out.size(); ++i)
        psx_write8(ctx, dst + static_cast<uint32_t>(i), static_cast<uint8_t>(out[i]));
    psx_write8(ctx, dst + static_cast<uint32_t>(out.size()), 0);
    ctx->r[kV0] = static_cast<uint32_t>(out.size());
}

}  // extern "C"
