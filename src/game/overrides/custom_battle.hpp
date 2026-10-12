#pragma once
// Custom Battle (custom_battle.cpp): the opponent list behind Beginner City's Custom Battle item.

#include <psx/recomp.h>

namespace dcb {

/// Register the screen's draw hook and window callbacks (psx::register_native); at start-up,
/// before the game runs.
void custom_battle_register();

/// In the keyword task (801EE704): runs the Custom Battle screen instead when the city's script
/// asked for it (host 0x0A cmd 0x40); false otherwise (the keyword screen as usual).
bool custom_battle_run(PsxContext* ctx);

}  // namespace dcb
