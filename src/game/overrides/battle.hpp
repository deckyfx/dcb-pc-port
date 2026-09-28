#pragma once
// Battle hotkeys (see battle.cpp): set a player's HP, attacks and DP during a card battle.

#include "platform.hpp"

#include <psx/recomp.h>

#include <cstdint>
#include <string>

namespace dcb {

using platform::kBattleP1Max;
using platform::kBattleP1Zero;
using platform::kBattleP2Max;
using platform::kBattleP2Zero;

/// Once per frame with the host commands: apply the battle_* hotkeys that were pressed. Returns a
/// notice for the screen ("P1: HP/attacks 9999, DP 99", "P2: not in a battle"), or "".
std::string battle_hotkeys(PsxContext& ctx, uint32_t commands);

}  // namespace dcb
