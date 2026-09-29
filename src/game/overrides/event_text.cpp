// English for the two scenario-script hosts besides the cities (docs/re/text-engine.md §7.11).
// The scripts come from the US disc (tools/text/scripts.py writes the loose B/BETA.MSD and
// C/EVENT/UNIT0n.MSD), so their lines are English, which the JP host code mangles:
//
// Tutorial (KAWSEG, B:\BETA.MSD). tutorial_msg_show (801ED334, a0 box centre y, a1 text) copies
//   the text into a 144-byte stack buffer, replacing every bare `p` with the player's name (no
//   length check), then opens a box sized by text_measure, draws the buffer each frame through
//   tutorial_msg_draw_cb (801ED2D8, which reads the text pointer at *g_battle_state + 0x10) and waits
//   for circle. In English every `p` of a word becomes the name, and the US code is `*p`. The
//   override does the same steps for an English line, with `*p` expanded and a buffer as long as
//   the line; a JP line (the catalog's first-turn lines included) takes the original.
//
// Fusion Shop (EVOSEG, C:\EVENT\UNIT0n.MSD, Andromon No.1-3). unit_msg_build (801EBF4C, a0 text)
//   claims one of the four 64-byte lines of the message page (unit_msg_lines 801F7CF0, via
//   unit_msg_slot_alloc 801EBEC0), writes the JP letter spacing `w1` and copies Shift-JIS and the
//   bare codes a b c e s + digit, expands h0 (player name, game_data + 0), h1 (the card name of
//   the fusion result, *(801F7418 + 4 * s16 801F7FC0) + 3), h2/h3 (h1 with all but its first
//   character as ？), and drops every other byte - all English letters. A line of 60 bytes or
//   more is marked unused. unit_msg_reveal (801EC33C, a0 x, a1 y, a2 line, a3 OT) is the
//   typewriter: one Shift-JIS character per call, -1 once the whole line shows. The override does
//   what city_text.cpp does for the city lines: the English line (codes expanded) stays host-side,
//   the line slot holds a marker and a serial, and the reveal types two characters a frame through
//   the text renderer. The first line of a page (the speaker's name) shows at once, as in JP.

#include "typewriter.hpp"

#include <psx/recomp.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

extern "C" {
void dcb_text_measure(PsxContext* ctx);   // text_measure(prop, str) through its override (ASCII port)
void dcb_task_sleep(PsxContext* ctx);     // task_sleep(frames)
void f_80016D54(PsxContext* ctx);         // window_init(win, rect, -1, -1, 8@sp16, 21@sp20, 128@sp24, 8@sp28)
void f_8001723C(PsxContext* ctx);         // window_draw(win, draw_cb, ot) -> nonzero once its resize animation ended
void f_80017084(PsxContext* ctx);         // window_resize(win, rect; -1 = shrink to nothing)
void f_8002DCD8(PsxContext* ctx);         // sound_se_play(id) (SsUtKeyOnV)
void o_EVOSEG_801EBEC0(PsxContext* ctx);  // unit_msg_slot_alloc(lines) -> line or 0
}

namespace {

using namespace dcb::typewriter;

constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kSp = 29;

// --- tutorial (KAWSEG) ---

constexpr uint32_t kTutorialShow = 0x801ED334u;
constexpr uint32_t kBattleState = 0x801DAF38u;     // g_battle_state; *state = tutorial block (+0x10 text)
constexpr uint32_t kPlayers = 0x801DAF40u;         // g_battle_players[0] (+0x1CA name)
constexpr uint32_t kTextW = 0x801D9708u, kTextH = 0x801D970Cu;  // g_text_w / g_text_h
constexpr uint32_t kWindow = 0x801FF1A0u;          // tutorial_msg_window
constexpr uint32_t kWindowTitle = 0x801E0DA0u;     // "TUTORIAL"
constexpr uint32_t kDrawCb = 0x801ED2D8u;          // tutorial_msg_draw_cb
constexpr uint32_t kFrameStep = 0x8007C0D0u;       // frames per task_sleep
constexpr uint32_t kPadBusy = 0x8008C41Cu;
constexpr uint32_t kPad = 0x8008C420u;             // -> pad words (+2 pressed this frame)
constexpr uint32_t kOriginalBuffer = 144;

void call(PsxContext& ctx, void (*fn)(PsxContext*), uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
          uint32_t a3 = 0) {
    ctx.r[kA0] = a0;
    ctx.r[kA1] = a1;
    ctx.r[kA2] = a2;
    ctx.r[kA3] = a3;
    fn(&ctx);
}

void sleep_and_update(PsxContext& ctx) {
    call(ctx, dcb_task_sleep, psx_read32(&ctx, kFrameStep));
    call(ctx, f_8001723C, kWindow, kDrawCb, 0);
}

/// `text` with every `*p` replaced by `name` (the US tutorial's name code).
std::string expand_player(const std::string& text, const std::string& name) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == 'p') {
            out += name;
            ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// --- Fusion Shop (EVOSEG) ---

constexpr uint32_t kUnitBuild = 0x801EBF4Cu;
constexpr uint32_t kUnitReveal = 0x801EC33Cu;
constexpr uint32_t kUnitLines = 0x801F7CF0u;  // 4 lines, 64 bytes each
constexpr uint32_t kLineSize = 64;
constexpr int kLineCount = 4;
constexpr uint32_t kGameData = 0x80070C2Cu;   // -> game_data (+0 player name)
constexpr uint32_t kUnitCards = 0x801F7418u;  // card record pointers
constexpr uint32_t kUnitCard = 0x801F7FC0u;   // s16 index into them
constexpr uint8_t kMarker = 0x7F;
constexpr size_t kCharsPerFrame = 2;

std::unordered_map<uint32_t, std::string> g_lines;  // serial -> English line
uint32_t g_next_serial = 1;

/// h2/h3: the name with all but its first character hidden (JP: ？ per character).
std::string masked(const std::string& name) {
    if (name.empty()) return name;
    if (sjis_lead(static_cast<uint8_t>(name[0]))) {
        std::string out = name.substr(0, 2);
        for (size_t i = 2; i + 1 < name.size(); i += 2) out += "\x81\x48";
        return out;
    }
    std::string out = name.substr(0, 1);
    for (size_t i = 1; i < name.size(); ++i) out.push_back(name[i] == ' ' ? ' ' : '?');
    return out;
}

/// The line with its name codes (`*h0`..`*h3`) filled in, as unit_msg_build does for JP `hN`.
std::string expand_names(PsxContext& ctx, const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '*' || i + 2 >= text.size() || text[i + 1] != 'h' || text[i + 2] < '0' || text[i + 2] > '9') {
            out.push_back(text[i]);
            continue;
        }
        const int n = text[i + 2] - '0';
        i += 2;
        if (n == 0) {
            out += read_string(ctx, psx_read32(&ctx, kGameData), 12);
        } else if (n <= 3) {
            const auto index = static_cast<int16_t>(psx_read16(&ctx, kUnitCard));
            const uint32_t card = psx_read32(&ctx, kUnitCards + 4u * static_cast<uint32_t>(index));
            const std::string name = read_string(ctx, card + 3, 21);
            out += n == 1 ? name : masked(name);
        }
    }
    return out;
}

uint32_t line_serial(PsxContext& ctx, uint32_t line) {
    if (psx_read8(&ctx, line) != kMarker) return 0;
    return psx_read32(&ctx, line + 4);
}

}  // namespace

extern "C" {

// KAWSEG 801ED334: tutorial_msg_show(y, text): box with the message until circle is pressed.
void dcb_tutorial_msg_show(PsxContext* ctx) {
    const std::string src = read_string(*ctx, ctx->r[kA1]);
    if (!is_english(src)) return psx_call_original(ctx, kTutorialShow);
    const uint32_t player = psx_read32(ctx, kPlayers);
    const std::string text = expand_player(src, read_string(*ctx, player + 0x1CA, 32));
    const int16_t centre_y = static_cast<int16_t>(ctx->r[kA0]);

    // The original's frame (sp+16 stack arguments, sp+32 rect, sp+40 text), the text part as
    // long as the line needs.
    const uint32_t sp = ctx->r[kSp];
    const uint32_t buffer = std::max<uint32_t>(kOriginalBuffer, static_cast<uint32_t>(text.size()) + 1);
    const uint32_t frame = sp - ((40 + buffer + 7) & ~7u);
    const uint32_t rect = frame + 32, str = frame + 40;
    ctx->r[kSp] = frame;
    for (size_t i = 0; i < text.size(); ++i) psx_write8(ctx, str + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
    psx_write8(ctx, str + static_cast<uint32_t>(text.size()), 0);

    const uint32_t state = psx_read32(ctx, kBattleState);
    psx_write8(ctx, state + 0x822, 0);
    psx_write32(ctx, psx_read32(ctx, state) + 0x10, str);  // what tutorial_msg_draw_cb draws
    call(*ctx, dcb_text_measure, 1, str);

    // Box: measured size rounded up to even, centred horizontally and on `centre_y`.
    const int32_t half_w = (static_cast<int32_t>(psx_read32(ctx, kTextW)) + 1) / 2;
    const int32_t half_h = (static_cast<int32_t>(psx_read32(ctx, kTextH)) + 1) / 2;
    psx_write16(ctx, rect + 0, static_cast<uint16_t>((320 - static_cast<int16_t>(half_w * 2)) >> 1));
    psx_write16(ctx, rect + 2, static_cast<uint16_t>(centre_y - half_h));
    psx_write16(ctx, rect + 4, static_cast<uint16_t>(half_w * 2));
    psx_write16(ctx, rect + 6, static_cast<uint16_t>(half_h * 2));
    psx_write32(ctx, frame + 16, 8);
    psx_write32(ctx, frame + 20, 21);
    psx_write32(ctx, frame + 24, 128);
    psx_write32(ctx, frame + 28, 8);
    call(*ctx, f_80016D54, kWindow, rect, 0xFFFFFFFFu, 0xFFFFFFFFu);
    psx_write32(ctx, kWindow + 44, kWindowTitle);
    psx_write8(ctx, kWindow + 56, 4);
    call(*ctx, f_8002DCD8, 0xA3);
    psx_write32(ctx, kPadBusy, 0);

    // Until the box is open and circle is pressed.
    for (;;) {
        sleep_and_update(*ctx);
        if (ctx->r[kV0] == 0) continue;
        if (psx_read16(ctx, psx_read32(ctx, kPad) + 2) & 0x20) break;
    }
    call(*ctx, f_8002DCD8, 0xA4);
    call(*ctx, f_80017084, kWindow, 0xFFFFFFFFu);
    for (int i = 0; i < 16; ++i) sleep_and_update(*ctx);
    psx_write32(ctx, kPadBusy, 0);
    ctx->r[kSp] = sp;
}

// EVOSEG 801EBF4C: unit_msg_build(text) -> line index, or -1 when the page is full.
void dcb_unit_msg_build(PsxContext* ctx) {
    const std::string src = read_string(*ctx, ctx->r[kA0]);
    if (!is_english(src)) return psx_call_original(ctx, kUnitBuild);
    const std::string text = expand_names(*ctx, src);

    ctx->r[kA0] = kUnitLines;
    o_EVOSEG_801EBEC0(ctx);  // unit_msg_slot_alloc: in use, shown = -1 on the first line, else 0
    const uint32_t line = ctx->r[kV0];
    if (line == 0) {
        ctx->r[kV0] = static_cast<uint32_t>(-1);
        return;
    }
    const uint32_t serial = g_next_serial++;
    if (g_lines.size() > 4096) g_lines.clear();  // a long session: old serials are long gone
    g_lines[serial] = text;
    const auto total = static_cast<uint16_t>(std::min<size_t>(visible_count(text), 127));
    psx_write8(ctx, line, kMarker);
    psx_write8(ctx, line + 1, 0);
    psx_write32(ctx, line + 4, serial);
    if (psx_read16(ctx, line + 60) == 0xFFFF) psx_write16(ctx, line + 60, total);  // the name line
    psx_write8(ctx, line + 63, static_cast<uint8_t>(total));
    ctx->r[kV0] = (line - kUnitLines) / kLineSize;
}

// EVOSEG 801EC33C: unit_msg_reveal(x, y, line, ot) -> 1 while revealing, -1 once shown whole.
void dcb_unit_msg_reveal(PsxContext* ctx) {
    const uint32_t line = ctx->r[kA2];
    const bool ours = line >= kUnitLines && line < kUnitLines + kLineCount * kLineSize;
    const uint32_t serial = ours ? line_serial(*ctx, line) : 0;
    const auto it = serial ? g_lines.find(serial) : g_lines.end();
    if (it == g_lines.end()) return psx_call_original(ctx, kUnitReveal);  // a JP line
    const std::string& text = it->second;
    const size_t total = visible_count(text);
    size_t shown = psx_read16(ctx, line + 60);
    const bool done = shown >= total;
    if (!done) {
        shown = std::min(total, shown + kCharsPerFrame);
        psx_write16(ctx, line + 60, static_cast<uint16_t>(shown));
    }
    draw_line(*ctx, static_cast<int>(ctx->r[kA0]), static_cast<int>(ctx->r[kA1]), ctx->r[kA3], prefix(text, shown));
    ctx->r[kV0] = done ? static_cast<uint32_t>(-1) : 1u;
}

}  // extern "C"
