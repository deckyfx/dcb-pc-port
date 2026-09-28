// Battle hotkeys ([hotkeys] battle_p1 = F10, battle_p2 = F11, battle_reset = F12): during a card
// battle, apply the actions that are on in the trainer's Battle tab (set a player's HP,
// circle/triangle/cross attack or DP to the value chosen there), or put every changed stat back.
// The PC take on the US version's GameShark codes ("press L1+L2 for max HP P1" and so on),
// translated to the JP build (docs/re/battle.md).
//
// Each player's battle data is reached through the pointer table at 801DAF40 (one word per
// player), so the hotkeys follow wherever the game put it. Offsets are the US codes' minus 4:
//   +0x118 HP, +0x11A/+0x11C/+0x11E circle/triangle/cross attack, +0x120 DP,
//   +0x158/+0x15A/+0x15C the circle/triangle/cross attack of the current battle.
// An attack action writes both of its fields. The game recalculates DP (80043B00, stored at
// 80040F70), which would undo a written value; the US codes no-op that store with a code patch.
// Here the DP calculation is overridden (config/SLPS-03101/overrides.json) and returns the value
// set by the hotkey while it is locked.

#include "battle.hpp"

#include <psx/recomp.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kPlayerTable = 0x801DAF40u;  // player battle data pointers, player 0 = you
constexpr uint32_t kDpCalc = 0x80043B00u;       // DP of player a0
constexpr uint32_t kDeckShuffle = 0x80043E24u;  // shuffle player a0's undrawn cards
constexpr uint32_t kShufflesPending = 0x116;    // u16: how many shuffle passes deck_shuffle runs
constexpr int kA0 = 4, kV0 = 2;
constexpr uint32_t kDp = 0x120;
/// A DP lock ends when the DP calculation has not run for this many frames (the battle is over).
constexpr uint64_t kLockIdleFrames = 600;

/// The fields one action writes.
std::vector<uint32_t> fields(trainer::BattleStat stat) {
    switch (stat) {
    case trainer::BattleStat::Hp: return {0x118};
    case trainer::BattleStat::Circle: return {0x11A, 0x158};
    case trainer::BattleStat::Triangle: return {0x11C, 0x15A};
    case trainer::BattleStat::Cross: return {0x11E, 0x15C};
    case trainer::BattleStat::Dp: return {kDp};
    case trainer::BattleStat::NoShuffle: return {};  // a toggle, see dcb_deck_shuffle
    }
    return {};
}

struct DpLock {
    bool on = false;
    uint16_t value = 0;
    uint32_t data = 0;       ///< the player's battle data pointer when locked
    uint64_t last_seen = 0;  ///< frame the DP calculation last ran for this player
};
std::array<DpLock, 2> g_locks;
uint64_t g_frame = 0;
const trainer::BattleActions* g_actions = nullptr;  // the trainer's Battle tab (no-shuffle toggles)

/// Values before the first change, per player: the battle data pointer, then field offset ->
/// value. Reset puts them back (only while the pointer is still that battle's).
struct Originals {
    uint32_t data = 0;
    std::map<uint32_t, uint16_t> values;
};
std::array<Originals, 2> g_originals;

bool in_ram(uint32_t addr) { return addr >= 0x80000000u && addr < 0x80200000u - 0x200u; }

uint32_t player_data(PsxContext& ctx, int player) {
    return psx_read32(&ctx, kPlayerTable + static_cast<uint32_t>(player) * 4u);
}

std::string apply(PsxContext& ctx, int player, const trainer::BattleActions& actions) {
    const std::string who = player == 0 ? "P1" : "P2";
    const uint32_t data = player_data(ctx, player);
    if (!in_ram(data)) return who + ": not in a battle";
    Originals& orig = g_originals[static_cast<size_t>(player)];
    if (orig.data != data) orig = {data, {}};  // another battle: older originals are stale
    int applied = 0;
    for (const trainer::BattleAction& a : actions.list()) {
        if (a.player != player || !a.enabled || a.is_toggle()) continue;
        const auto value = static_cast<uint16_t>(a.value);
        for (const uint32_t off : fields(a.stat)) {
            orig.values.emplace(off, psx_read16(&ctx, data + off));  // keeps the first original
            psx_write16(&ctx, data + off, value);
        }
        if (a.stat == trainer::BattleStat::Dp) g_locks[static_cast<size_t>(player)] = {true, value, data, g_frame};
        ++applied;
    }
    if (applied == 0) return who + ": nothing is on in the trainer's Battle tab (F4)";
    return who + ": " + std::to_string(applied) + (applied == 1 ? " stat set" : " stats set");
}

std::string reset(PsxContext& ctx) {
    int restored = 0;
    for (int player = 0; player < 2; ++player) {
        Originals& orig = g_originals[static_cast<size_t>(player)];
        if (orig.data != 0 && player_data(ctx, player) == orig.data) {
            for (const auto& [off, value] : orig.values) {
                psx_write16(&ctx, orig.data + off, value);
                ++restored;
            }
        }
        orig = {};
        g_locks[static_cast<size_t>(player)].on = false;
    }
    return restored ? "battle stats put back" : "no battle stats to put back";
}

}  // namespace

namespace dcb {

std::string battle_hotkeys(PsxContext& ctx, uint32_t commands, const trainer::BattleActions& actions) {
    ++g_frame;
    g_actions = &actions;
    for (DpLock& lock : g_locks)  // battle over: the DP calculation stopped running
        if (lock.on && g_frame - lock.last_seen > kLockIdleFrames) lock.on = false;
    std::string notice;
    if (commands & kBattleP1) notice = apply(ctx, 0, actions);
    if (commands & kBattleP2) notice = apply(ctx, 1, actions);
    if (commands & kBattleReset) notice = reset(ctx);
    return notice;
}

}  // namespace dcb

extern "C" {

// 80043B00: DP of player a0. While a hotkey locked it, the locked value, so the game's own
// recalculation (stored at 80040F70) keeps it.
void dcb_dp_calc(PsxContext* ctx) {
    const uint32_t player = ctx->r[kA0];
    if (player < g_locks.size()) {
        DpLock& lock = g_locks[player];
        if (lock.on) {
            if (player_data(*ctx, static_cast<int>(player)) != lock.data) {
                lock.on = false;  // another battle's data: the lock was for the last one
            } else {
                lock.last_seen = g_frame;
                ctx->r[kV0] = lock.value;
                return;
            }
        }
    }
    psx_call_original(ctx, kDpCalc);
}

// 80043E24: deck_shuffle(player): runs game_data[player] +0x116 passes of swapping each undrawn
// card (the last n of the 30 at +0x179) with a random undrawn one, then clears +0x116. Battle
// start and card effects (Reserve Seven, card 291, sets 300 passes) both come here. With the
// trainer's "deck in order" on for that player, it only clears the pending count: the cards
// stay in deck order.
void dcb_deck_shuffle(PsxContext* ctx) {
    const auto player = static_cast<int>(ctx->r[kA0]);
    if (g_actions && player >= 0 && player < 2 && g_actions->no_shuffle(player)) {
        const uint32_t data = player_data(*ctx, player);
        if (in_ram(data)) psx_write16(ctx, data + kShufflesPending, 0);
        return;
    }
    psx_call_original(ctx, kDeckShuffle);
}

}  // extern "C"
