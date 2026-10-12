// Custom Battle: a list of the opponents the player has fought, to battle one of them again
// (docs/wiki/Mods.md; the game side of patch::mods::add_custom_battle, docs/re/city-menu.md).
//
// Beginner City's menu gets a 5th place, the blank button (item 10), which runs host 0x0A cmd 0x40.
// The game's city host knows no such command (an unknown one ends the script: r0 = 0). The override
// of msd_vm_step (80021198) turns it into 0x0A cmd 15, the host's own screen command (Wizardmon's
// spell): it closes the message window, sets the city mode to 10 (a task runs; the script waits)
// and spawns the keyword task (801EE704), whose override (keyword.cpp) runs this screen instead.
// The screen:
//   - lists the decks whose record says "fought" (game_data+0xA90, bit 15: every city battle sets
//     it, under the deck's record index, so variants of one opponent share an entry), from DECK2.DEK
//     (assets/<serial>/files/B, English: owner +73, deck name +60, prize pack +101, EXP +102), with
//     the player's wins (+0x858) and losses (+0x974) against each; not deck 0 (the tutorial) or
//     the player's preset decks (142-158);
//   - opens three windows (the keyword screen's, free while this one is up), a per-frame draw hook
//     that draws them, and runs Up/Down, L1/R1 (a page), Circle (battle) and Cross (back);
//   - closes them, puts the deck (or -1) in r1, sets the mode back to 0 and reopens the message
//     window (801E3CC0(1)), as the keyword task ends. The script then runs `cmd2(r1)`: the game's
//     own battle, Deck Select, rewards and records included.
// The draw hook and the window callbacks are native functions at reserved guest addresses
// (psx::register_native), so the game calls them like its own.
//
// When the city menu opens it probes each place (city_menu_probe_item runs the script ahead to the
// place's host command); the command is not run then.
//
// The menu label: the blank button has no text; the override of the menu's draw (801E33A4) writes
// "Custom Battle" over the item whose sprite shows label row 10.

#include "custom_battle.hpp"

#include "native_files.hpp"
#include "text.hpp"

#include "patch/mods.hpp"

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

constexpr int kSp = 29, kV0 = 2, kA0 = 4;

// Native functions (psx::register_native).
constexpr uint32_t kDrawHook = psx::kNativeBase + 0x04;
constexpr uint32_t kListCb = psx::kNativeBase + 0x08;
constexpr uint32_t kInfoCb = psx::kNativeBase + 0x0C;
constexpr uint32_t kHelpCb = psx::kNativeBase + 0x10;

// The game.
constexpr uint32_t kVmStep = 0x80021198u;      // msd_vm_step(vm, regs) -> 1 at a host command
constexpr uint32_t kMenuDraw = 0x801E33A4u;    // SAISEG city_menu_draw
constexpr uint32_t kMenuProbe = 0x801E3DC4u;   // SAISEG city_menu_probe_item: runs the script ahead
constexpr uint32_t kHost = 0x801F7E80u;        // -> city host state: +4 the VM, +8 the registers (r1 at +4)
constexpr uint32_t kCityMode = 0x801F7CDEu;    // u8: 10 while a city task runs, 0 resumes the script
constexpr uint32_t kMenuType = 0x801F7C52u;    // u8: 1 the city menu, 0 a choice list
constexpr uint32_t kMenuCount = 0x801F7C54u;   // s8: its items
constexpr uint32_t kMenuItems = 0x801F8910u;   // its item sprites (0x90-byte objects)
constexpr uint32_t kPrimBuffer = 0x8007C0D4u;  // u8: which of a sprite's two POLY_FT4 is drawn
constexpr uint32_t kGameData = 0x80070C2Cu;    // -> game_data
constexpr uint32_t kPad = 0x8008C420u;         // -> pad 0: +10 pressed this frame, +14 with repeat
constexpr uint32_t kGrey = 0x80070A64u;        // text_draw_grey's colour
constexpr uint32_t kPacks = 0x80070EF8u;       // prize pack names (pointers, JP)
// The keyword screen's windows and grid cursor (SAISEG .bss), free while this screen is up.
constexpr uint32_t kWinList = 0x801F7A20u, kWinInfo = 0x801F7AC0u, kWinHelp = 0x801F7B60u;
constexpr uint32_t kCursor = 0x801F7A70u;

constexpr uint32_t kSleep = 0x80014D3Cu;
constexpr uint32_t kWindowInit = 0x80016D54u, kWindowResize = 0x80017084u, kWindowDraw = 0x8001723Cu;
constexpr uint32_t kHookAdd = 0x80016988u, kHookRemove = 0x800169C4u;
constexpr uint32_t kCursorInit = 0x80019240u, kCursorTarget = 0x800193CCu, kCursorDraw = 0x80019448u;
constexpr uint32_t kSound = 0x8002DCD8u;       // 0 confirm, 1 cancel, 2 move, 4 open
constexpr uint32_t kTextGrey = 0x8002ADC8u;    // text_draw_grey(x, y, clut, prop, ot, str): codes, icons
constexpr uint32_t kMsgReopen = 0x801E3CC0u;   // the message window back (as the keyword task ends)

constexpr uint16_t kKeywordCmd = 15;  // city host 0x0A cmd 15: the keyword screen

constexpr uint16_t kUp = 0x1000, kDown = 0x4000, kL1 = 0x4, kR1 = 0x8, kCircle = 0x20, kCross = 0x40;
constexpr int kOt = 30;            // the windows' OT entry (the keyword screen's)
constexpr int kRowH = 14, kRows = 11;
constexpr int kCustomBattleItem = 10;  // the city menu's blank button

constexpr int kDecks = 159, kOpponents = 142, kDekRecord = 104;

struct Opponent {
    int deck = 0;
    std::string owner, name, pack;
    int wins = 0, losses = 0, exp = 0;
};

struct Screen {
    bool open = false;
    std::vector<Opponent> list;
    int cursor = 0, top = 0;
    uint32_t buf = 0;  // guest scratch on the task's stack: rects, the help line
} g;

/// While the city menu probes its items (city_menu_probe_item runs the script from each pick to its
/// next host command, to grey out a place whose command is 0x0A cmd 14), host commands it reaches
/// are only looked at, not run.
bool g_probing = false;

/// Calls guest `fn`; arguments past the fourth go on the stack, where the callee reads them
/// (psx::call_guest gives it the frame at (sp - 0x100) & ~7).
uint32_t call(PsxContext& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t frame = (ctx.r[kSp] - 0x100u) & ~7u;
    uint32_t a[4] = {};
    size_t i = 0;
    for (const uint32_t v : args) {
        if (i < 4) a[i] = v;
        else psx_write32(&ctx, frame + 16 + 4 * static_cast<uint32_t>(i - 4), v);
        ++i;
    }
    return psx::call_guest(ctx, fn, a[0], a[1], a[2], a[3]);
}

void put_rect(PsxContext& ctx, uint32_t at, int x, int y, int w, int h) {
    psx_write16(&ctx, at, static_cast<uint16_t>(x));
    psx_write16(&ctx, at + 2, static_cast<uint16_t>(y));
    psx_write16(&ctx, at + 4, static_cast<uint16_t>(w));
    psx_write16(&ctx, at + 6, static_cast<uint16_t>(h));
}

std::string guest_string(PsxContext& ctx, uint32_t at, size_t limit) {
    std::string s;
    for (size_t i = 0; i < limit; ++i) {
        const auto c = static_cast<char>(psx_read8(&ctx, at + static_cast<uint32_t>(i)));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

bool in_ram(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

/// The decks the player has fought (see the top of the file).
std::vector<Opponent> opponents(PsxContext& ctx) {
    std::vector<Opponent> out;
    const std::string path = dcb::asset_path("files/B/DECK2.DEK");
    FILE* in = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!in) return out;
    std::vector<uint8_t> dek(8 + kDecks * kDekRecord);
    const bool ok = std::fread(dek.data(), 1, dek.size(), in) == dek.size();
    std::fclose(in);
    const uint32_t gd = psx_read32(&ctx, kGameData);
    if (!ok || !in_ram(gd)) return out;
    for (int d = 1; d < kOpponents; ++d) {
        if (!(psx_read16(&ctx, gd + 0xA90 + 2 * d) & 0x8000)) continue;
        const uint8_t* r = dek.data() + 8 + d * kDekRecord;
        Opponent o;
        o.deck = d;
        o.owner.assign(reinterpret_cast<const char*>(r + 73), strnlen(reinterpret_cast<const char*>(r + 73), 21));
        o.name.assign(reinterpret_cast<const char*>(r + 60), strnlen(reinterpret_cast<const char*>(r + 60), 13));
        o.name = dcb::text_full_name(o.name) + " Deck";
        dcb::text_swap_names(o.owner);
        dcb::text_swap_names(o.name);
        o.wins = psx_read16(&ctx, gd + 0x858 + 2 * d);
        o.losses = psx_read16(&ctx, gd + 0x974 + 2 * d);
        o.exp = r[102];
        if (const uint32_t p = psx_read32(&ctx, kPacks + 4u * r[101]); in_ram(p)) {
            const std::string jp = guest_string(ctx, p, 32);
            if (!dcb::text_translate(ctx, jp, o.pack)) o.pack.clear();
        }
        out.push_back(std::move(o));
    }
    return out;
}

void text(PsxContext& ctx, int x, int y, int ot, const std::string& s) {
    dcb::text_draw_verbatim(ctx, x, y, 7, 0, kGrey, ot, s, nullptr);
}

// --- the native functions -------------------------------------------------------------------------

/// Window callbacks (a0 = the window: content x at +0, y at +2, OT entry at +58).
struct Window {
    int x, y, ot;
};
Window window(PsxContext& ctx) {
    const uint32_t win = ctx.r[kA0];
    return {static_cast<int16_t>(psx_read16(&ctx, win)), static_cast<int16_t>(psx_read16(&ctx, win + 2)),
            static_cast<int16_t>(psx_read16(&ctx, win + 58))};
}

void list_cb(PsxContext* ctx) {
    if (!g.open) return;
    const Window w = window(*ctx);
    if (g.list.empty()) {
        text(*ctx, w.x + 6, w.y + 4, w.ot, "No opponents yet.");
        return;
    }
    for (int i = 0; i < kRows && g.top + i < static_cast<int>(g.list.size()); ++i)
        text(*ctx, w.x + 8, w.y + 4 + i * kRowH, w.ot, g.list[static_cast<size_t>(g.top + i)].owner);
    put_rect(*ctx, g.buf + 24, w.x + 3, w.y + 2 + (g.cursor - g.top) * kRowH, 144, 13);
    call(*ctx, kCursorTarget, {kCursor, g.buf + 24});
    call(*ctx, kCursorDraw, {kCursor, static_cast<uint32_t>(w.ot)});
}

void info_cb(PsxContext* ctx) {
    if (!g.open || g.list.empty()) return;
    const Window w = window(*ctx);
    const Opponent& o = g.list[static_cast<size_t>(g.cursor)];
    int y = w.y + 4;
    const auto line = [&](const std::string& s) {
        text(*ctx, w.x + 6, y, w.ot, s);
        y += kRowH;
    };
    line(o.owner);
    line(o.name);
    y += kRowH / 2;
    line("Wins " + std::to_string(o.wins));
    line("Losses " + std::to_string(o.losses));
    y += kRowH / 2;
    line("EXP +" + std::to_string(o.exp));
    if (!o.pack.empty()) line(o.pack);
    y = w.y + 4 + 10 * kRowH;
    line(std::to_string(g.cursor + 1) + " / " + std::to_string(g.list.size()));
}

void help_cb(PsxContext* ctx) {
    if (!g.open) return;
    const Window w = window(*ctx);
    // text_draw_grey with the help line in the task's scratch (button icons: *b0 circle, *b2 cross).
    call(*ctx, kTextGrey, {static_cast<uint32_t>(w.x + 6), static_cast<uint32_t>(w.y + 2), 7, 0,
                           static_cast<uint32_t>(w.ot), g.buf + 64});
}

void draw_hook(PsxContext* ctx) {
    call(*ctx, kWindowDraw, {kWinList, kListCb, kOt});
    call(*ctx, kWindowDraw, {kWinInfo, kInfoCb, kOt});
    call(*ctx, kWindowDraw, {kWinHelp, kHelpCb, kOt});
}

/// The screen's task (the keyword task's place, see dcb_msd_vm_step); see the top of the file.
void task(PsxContext* ctx) {
    PsxContext& c = *ctx;
    const uint32_t sp = c.r[kSp];
    g.buf = (sp - 0x100u) & ~7u;  // scratch above every frame the calls below push
    c.r[kSp] = g.buf;
    g.list = opponents(c);
    g.cursor = g.top = 0;
    const std::string help = "*b0 Battle   *b2 Back   L1/R1 Page";
    for (size_t i = 0; i <= help.size(); ++i) psx_write8(&c, g.buf + 64 + static_cast<uint32_t>(i), i < help.size() ? help[i] : 0);

    put_rect(c, g.buf, 16, 20, 152, kRows * kRowH + 6);
    put_rect(c, g.buf + 8, 176, 20, 128, kRows * kRowH + 6);
    put_rect(c, g.buf + 16, 16, 186, 288, 18);
    call(c, kWindowInit, {kWinList, g.buf, 0xFFFFFFFFu, 0xFFFFFFFFu, 8, 38, 128, 12});
    call(c, kWindowInit, {kWinInfo, g.buf + 8, 0xFFFFFFFFu, 0xFFFFFFFFu, 8, 38, 128, 12});
    call(c, kWindowInit, {kWinHelp, g.buf + 16, 0xFFFFFFFFu, 0xFFFFFFFFu, 8, 38, 128, 12});
    put_rect(c, g.buf + 24, 20, 22, 144, 13);
    call(c, kCursorInit, {kCursor, g.buf + 24, 0xFFFFFFFFu});
    g.open = true;
    call(c, kHookAdd, {kDrawHook});

    int result = -1;
    const int n = static_cast<int>(g.list.size());
    for (;;) {
        call(c, kSleep, {1});
        const uint32_t pad = psx_read32(&c, kPad);
        if (!in_ram(pad)) continue;
        const uint16_t pressed = psx_read16(&c, pad + 10), held = psx_read16(&c, pad + 14);
        const int before = g.cursor;
        if (n > 0 && (held & kUp)) g.cursor = (g.cursor + n - 1) % n;
        else if (n > 0 && (held & kDown)) g.cursor = (g.cursor + 1) % n;
        else if (n > 0 && (held & kL1)) g.cursor = std::max(0, g.cursor - kRows);
        else if (n > 0 && (held & kR1)) g.cursor = std::min(n - 1, g.cursor + kRows);
        if (g.cursor != before) call(c, kSound, {2});
        g.top = std::clamp(g.top, std::max(0, g.cursor - kRows + 1), g.cursor);
        if ((pressed & kCircle) && n > 0) {
            call(c, kSound, {0});
            result = g.list[static_cast<size_t>(g.cursor)].deck;
            break;
        }
        if (pressed & kCross) {
            call(c, kSound, {1});
            break;
        }
    }

    for (const uint32_t win : {kWinList, kWinInfo, kWinHelp}) call(c, kWindowResize, {win, 0xFFFFFFFFu});
    call(c, kSleep, {12});
    g.open = false;
    call(c, kHookRemove, {kDrawHook});
    const uint32_t host = psx_read32(&c, kHost);
    psx_write32(&c, psx_read32(&c, host + 8) + 4, static_cast<uint32_t>(result));  // r1
    psx_write8(&c, kCityMode, 0);
    call(c, kMsgReopen, {1});
    c.r[kSp] = sp;
}

/// Set when the city's script reaches host 0x0A cmd 0x40 (see dcb_msd_vm_step).
bool g_pending = false;

}  // namespace

bool dcb::custom_battle_run(PsxContext* ctx) {
    if (!g_pending) return false;
    g_pending = false;
    task(ctx);
    return true;
}

void dcb::custom_battle_register() {
    psx::register_native(kDrawHook, draw_hook);
    psx::register_native(kListCb, list_cb);
    psx::register_native(kInfoCb, info_cb);
    psx::register_native(kHelpCb, help_cb);
}

extern "C" {

// 80021198 msd_vm_step(vm, regs): the original, then the port's host command (Custom Battle) when
// the city's script reaches it.
void dcb_msd_vm_step(PsxContext* ctx) {
    const uint32_t vm = ctx->r[kA0];
    psx_call_original(ctx, kVmStep);
    if (g_probing || ctx->r[kV0] != 1 || psx_read16(ctx, vm + 0x18) != 0x0A || psx_read16(ctx, vm + 0x1A) != patch::mods::kCustomBattleCmd) return;
    const uint32_t host = psx_read32(ctx, kHost);
    if (!in_ram(host) || psx_read32(ctx, host + 4) != vm) return;  // not the city's script
    // The host runs it as its own screen command, 0x0A cmd 15 (Wizardmon's spell): closes the
    // message window, mode 10, spawns the keyword task (801EE704), returns with the script still
    // running (an unknown command would end it: r0 = 0). That task's override runs this screen.
    psx_write16(ctx, vm + 0x1A, kKeywordCmd);
    g_pending = true;
}

// SAISEG 801E3DC4 city_menu_probe_item(host): the original, the Custom Battle command not run.
void dcb_city_menu_probe(PsxContext* ctx) {
    g_probing = true;
    psx_call_original(ctx, kMenuProbe);
    g_probing = false;
}

// SAISEG 801E33A4 city_menu_draw: the original, then "Custom Battle" on the blank button.
void dcb_city_menu_draw(PsxContext* ctx) {
    psx_call_original(ctx, kMenuDraw);
    if (psx_read8(ctx, kMenuType) != 1) return;
    const int count = static_cast<int8_t>(psx_read8(ctx, kMenuCount));
    const uint32_t buffer = psx_read8(ctx, kPrimBuffer) & 1;
    for (int i = 0; i < count && i < 5; ++i) {
        const uint32_t obj = psx_read32(ctx, kMenuItems + 4u * static_cast<uint32_t>(i));
        if (!in_ram(obj)) continue;
        const uint32_t prim = obj + buffer * 0x28;
        if (psx_read8(ctx, prim + 0x0D) != kCustomBattleItem * 16) continue;  // v0: the label row
        const int x = static_cast<int16_t>(psx_read16(ctx, prim + 8));
        const int y = static_cast<int16_t>(psx_read16(ctx, prim + 10));
        dcb::text_draw_verbatim(*ctx, x + 8, y + 2, 7, 0, kGrey, 0x1F, "Custom Battle", nullptr);
    }
}

}  // extern "C"
