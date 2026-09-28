// Battle hotkeys (F9-F12, [hotkeys] battle_*): set a player's HP, circle/triangle/cross attacks
// and DP during a card battle. The PC take on the US version's GameShark codes ("press L1+L2 for
// max HP P1" and so on), translated to the JP build (docs/re/battle.md).
//
// Each player's battle data is reached through the pointer table at 801DAF40 (one word per
// player), so the hotkeys follow wherever the game put it. Offsets are the US codes' minus 4:
//   +0x118 HP, +0x11A/+0x11C/+0x11E circle/triangle/cross attack, +0x120 DP,
//   +0x158/+0x15A/+0x15C the circle/triangle/cross attack of the current battle.
// The game recalculates DP (80043B00, stored at 80040F70), which would undo a written value;
// the US codes no-op that store with a code patch. Here the DP calculation is overridden
// (config/SLPS-03101/overrides.json) and returns the value set by the hotkey while it is locked.

#include "battle.hpp"

#include <psx/recomp.h>

#include <array>
#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kPlayerTable = 0x801DAF40u;  // player battle data pointers, player 0 = you
constexpr uint32_t kDpCalc = 0x80043B00u;       // DP of player a0
constexpr int kA0 = 4, kV0 = 2;

constexpr uint32_t kHp = 0x118, kAttack = 0x11A, kDp = 0x120, kBattleAttack = 0x158;
constexpr uint16_t kMaxStat = 9999, kMaxDp = 99;
/// A DP lock ends when the DP calculation has not run for this many frames (the battle is over).
constexpr uint64_t kLockIdleFrames = 600;

struct DpLock {
    bool on = false;
    uint16_t value = 0;
    uint32_t data = 0;         ///< the player's battle data pointer when locked
    uint64_t last_seen = 0;    ///< frame the DP calculation last ran for this player
};
std::array<DpLock, 2> g_locks;
uint64_t g_frame = 0;

bool in_ram(uint32_t addr) { return addr >= 0x80000000u && addr < 0x80200000u - 0x200u; }

uint32_t player_data(PsxContext& ctx, int player) {
    return psx_read32(&ctx, kPlayerTable + static_cast<uint32_t>(player) * 4u);
}

}  // namespace

namespace dcb {

std::string battle_hotkeys(PsxContext& ctx, uint32_t commands) {
    ++g_frame;
    for (DpLock& lock : g_locks)  // battle over: the DP calculation stopped running
        if (lock.on && g_frame - lock.last_seen > kLockIdleFrames) lock.on = false;

    std::string notice;
    const struct {
        uint32_t bit;
        int player;
        bool max;
    } keys[] = {{kBattleP1Max, 0, true}, {kBattleP1Zero, 0, false}, {kBattleP2Max, 1, true}, {kBattleP2Zero, 1, false}};
    for (const auto& k : keys) {
        if (!(commands & k.bit)) continue;
        const uint32_t data = player_data(ctx, k.player);
        const std::string who = k.player == 0 ? "P1" : "P2";
        if (!in_ram(data)) {
            notice = who + ": not in a battle";
            continue;
        }
        const uint16_t stat = k.max ? kMaxStat : 0, dp = k.max ? kMaxDp : 0;
        psx_write16(&ctx, data + kHp, stat);
        for (uint32_t i = 0; i < 3; ++i) {
            psx_write16(&ctx, data + kAttack + 2 * i, stat);
            psx_write16(&ctx, data + kBattleAttack + 2 * i, stat);
        }
        psx_write16(&ctx, data + kDp, dp);
        DpLock& lock = g_locks[static_cast<size_t>(k.player)];
        lock = {true, dp, data, g_frame};
        notice = who + (k.max ? ": HP/attacks 9999, DP 99" : ": HP/attacks/DP 0");
    }
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

}  // extern "C"
