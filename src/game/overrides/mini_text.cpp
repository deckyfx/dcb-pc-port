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
//     Name slots: each caller has a width (kSlots). A name wider than its slot is drawn in tight
//     spacing (mini_fit.hpp), and one still too wide is replaced by its short name
//     (config/SLPS-03101/text/short-names.tsv -> assets/<serial>/en_short_names.txt). The
//     spacing is handed to the draw that follows (g_pending), not written into the string.
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
//       - capitals only (a translated "FULL SET!", the JP disc's own "RANK UP!", "L1      "):
//         the original on a guest-stack copy, keeping the fixed cells (padding by spaces
//         depends on them).
//   The US drew these strings in its 4x5 micro font (US 80027DE8, JP 80027EF4) with lowercase
//   folded to capitals; the JP micro cells have no lowercase, and 4x5 capitals read worse than
//   the 8x7 mini letters at the same width, so the mini font is kept.
//
// DCB_TRACE_TEXT=1 / hex logs each mini draw like the main renderer's trace ("[text] mini ...")
// and each fitted card name ("[text] mini-name ...").

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
#include <unordered_map>

extern "C" {
void dcb_text_icon(PsxContext* ctx);  // text_icon 80029F70 through its override (level_badges.cpp)
}

namespace {

namespace mini = dcb::mini;

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kSp = 29, kRa = 31;
constexpr uint32_t kMiniDraw = 0x800288C8u;
constexpr uint32_t kToMini = 0x8002A37Cu;
constexpr uint32_t kSysTimX = 0x801D9704u;  // SYSTEM.TIM VRAM x (u16, halfwords)
constexpr uint32_t kSysTimY = 0x801D9706u;  // SYSTEM.TIM VRAM y (u16)
constexpr size_t kNameMax = 20;             // card name slot: 21 bytes with the NUL
constexpr int kCell = 7;                    // mini glyph sprite: 7x7 in an 8x7 cell

/// Card-name slots, by the return address of the sjis_to_mini call (each draws the name right
/// after): the widest pen advance (mini_fit measure(), the last glyph's gap included) that stays
/// clear of what follows. Measured on headless snapshots (docs/re/text-engine.md §7.10).
struct Slot {
    uint32_t ra;
    int width;
    const char* what;
};
constexpr Slot kSlots[] = {
    // Battle card panel (8003C200): name at panel x + 32, the field's dark ends 1 px before
    // the DP box border (175 .. 263 at rest); ink up to 261 keeps 2 px clear.
    {0x8003C208u, 88, "battle panel"},
    // SUBSEG 801E6AC4 (Edit Partner, partner panel, 801E65E8): name at x 59, the panel's
    // inside ends at 203. Every card name fits (the widest is 112).
    {0x801E6ACCu, 144, "partner"},
    // SUBSEG 801E78A4 (Edit Partner, armor panel, 801E73BC): name at the panel's x + 3 = 217,
    // the inside ends at 303; ink up to 301.
    {0x801E78ACu, 86, "armor"},
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

// Short card names (assets/<serial>/en_short_names.txt, "full\tshort" lines, built by
// tools/text/short_names.py from config/<serial>/text/short-names.tsv). Missing file: none.
const std::unordered_map<std::string, std::string>& short_names() {
    static const std::unordered_map<std::string, std::string> names = [] {
        const std::string path = std::string("assets/") + DCB_GAME_ID + "/en_short_names.txt";
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return mini::parse_short_names(text);
    }();
    return names;
}

/// The fitted name sjis_to_mini wrote for the draw that follows: its buffer, text and spacing.
struct Pending {
    uint32_t dst = 0;
    std::string text;
    mini::Spacing spacing = mini::Spacing::Normal;
};
Pending g_pending;

/// Calls the original mini draw with `s` copied, NUL-terminated, into a frame pushed on the guest
/// stack (the rgb pointer and OT at +16/+20 as the JP convention wants).
void call_mini(PsxContext& ctx, int x, int y, const std::string& s, int clut, uint32_t rgb, int ot) {
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
    const int x = static_cast<int>(ctx->r[kA0]);
    const int y = static_cast<int>(ctx->r[kA1]);
    const uint32_t str = ctx->r[kA2];
    const int clut = static_cast<int>(ctx->r[kA3]);
    const uint32_t sp = ctx->r[kSp];
    const uint32_t rgb = psx_read32(ctx, sp + 16);
    const int ot = static_cast<int>(psx_read32(ctx, sp + 20));

    const std::string raw = read_string(*ctx, str);
    // A card name fitted by sjis_to_mini just before (same buffer, same text): its spacing.
    mini::Spacing spacing = mini::Spacing::Normal;
    const bool fitted = g_pending.dst != 0 && g_pending.dst == str && g_pending.text == raw;
    if (fitted) spacing = g_pending.spacing;
    g_pending = {};
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
    if (has_lowercase(s) || fitted) {
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
// copied through, bounded to the name slot and fitted to the caller's slot; Shift-JIS takes the
// original.
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

    std::string out = ascii_to_mini(name);
    g_pending = {};
    for (const Slot& slot : kSlots) {
        if (slot.ra != ra) continue;
        load_ink(*ctx);
        const auto it = short_names().find(name);
        const std::string short_mini = it == short_names().end() ? std::string() : ascii_to_mini(it->second);
        const mini::Fit fit = mini::fit_name(ink(), out, short_mini, slot.width);
        if (trace_on())
            std::fprintf(stderr, "[text] mini-name %s src=%08X \"%s\" -> \"%s\" %s w=%d (slot %d, full %d)\n",
                         slot.what, src, shown(out).c_str(), shown(fit.text).c_str(),
                         fit.spacing == mini::Spacing::Tight ? "tight" : "normal", fit.width, slot.width,
                         mini::measure(ink(), out, mini::Spacing::Normal));
        out = fit.text;
        g_pending = {dst, out, fit.spacing};
        break;
    }
    for (size_t i = 0; i < out.size(); ++i)
        psx_write8(ctx, dst + static_cast<uint32_t>(i), static_cast<uint8_t>(out[i]));
    psx_write8(ctx, dst + static_cast<uint32_t>(out.size()), 0);
    ctx->r[kV0] = static_cast<uint32_t>(out.size());
}

}  // extern "C"
