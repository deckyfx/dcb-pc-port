// The port's built-in cheats for SLPS-03101, shown on the trainer's Presets tab. Same GameShark
// syntax as the cheat file; how each address was found: docs/re/save-data.md.

#include "cheat_presets.hpp"

namespace dcb {

std::string_view cheat_presets() {
    // Card collection: one byte per card at 800E0646 + card number (low 3 bits = copies, the game
    // caps it at 6). C4 = obtained + seen + 4 copies. Cards 172-190 are left out: the game's
    // add-card function (8004850C) refuses them. Held while on: turn on, save in game, turn off.
    //
    // Digi parts: one bit per part at 800DF200-800DF20F, parts 0-126.
    return R"(# Built-in presets (the trainer's Presets tab).
[All cards x4 (save in game, then turn off)] off
5000AC01 0000
300E0646 00C4
50006E01 0000
300E0705 00C4

[All 127 Digi parts] off
50000702 0000
800DF200 FFFF
800DF20E 7FFF
)";
}

}  // namespace dcb
