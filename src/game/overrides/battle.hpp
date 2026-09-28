#pragma once
// Battle hotkeys (see battle.cpp): apply the trainer's Battle-tab actions during a card battle.

#include "platform.hpp"
#include "trainer_battle.hpp"

#include <psx/recomp.h>

#include <cstdint>
#include <string>

namespace dcb {

using platform::kBattleP1;
using platform::kBattleP2;
using platform::kBattleReset;

/// Once per frame with the host commands: battle_p1 / battle_p2 apply the actions that are on for
/// that player, battle_reset puts every changed stat back. Returns a notice for the screen, or "".
std::string battle_hotkeys(PsxContext& ctx, uint32_t commands, const trainer::BattleActions& actions);

}  // namespace dcb
