// The port's built-in cheats for SLPS-03101, shown on the trainer's Presets tab. Same GameShark
// syntax as the cheat file; how each address was found: docs/re/save-data.md.

#include "cheat_presets.hpp"

namespace dcb {

std::string_view cheat_presets() {
    // Card collection: one byte per card at 800E0646 + card number (low 3 bits = copies, the game
    // caps it at 6). C4 = obtained + seen + 4 copies. Held while on: turn on, save in game, turn
    // off. Cards 172-190 (the armor Digimon and the 02 partners) are special: the game's add-card
    // function (8004850C) refuses them, and the partner's own card is kept at one copy with the
    // "full" flag (51). They are written only while their byte is 0 (E0 condition), so the
    // partner card and any you already own are left alone.
    //
    // Digi parts: one bit per part at 800DF200-800DF20F, parts 0-126.
    return R"(# Built-in presets (the trainer's Presets tab).
[All cards x4 (save in game, then turn off)] off
5000AC01 0000
300E0646 00C4
50006E01 0000
300E0705 00C4
E00E06F2 0000
300E06F2 00C4
E00E06F3 0000
300E06F3 00C4
E00E06F4 0000
300E06F4 00C4
E00E06F5 0000
300E06F5 00C4
E00E06F6 0000
300E06F6 00C4
E00E06F7 0000
300E06F7 00C4
E00E06F8 0000
300E06F8 00C4
E00E06F9 0000
300E06F9 00C4
E00E06FA 0000
300E06FA 00C4
E00E06FB 0000
300E06FB 00C4
E00E06FC 0000
300E06FC 00C4
E00E06FD 0000
300E06FD 00C4
E00E06FE 0000
300E06FE 00C4
E00E06FF 0000
300E06FF 00C4
E00E0700 0000
300E0700 00C4
E00E0701 0000
300E0701 00C4
E00E0702 0000
300E0702 00C4
E00E0703 0000
300E0703 00C4
E00E0704 0000
300E0704 00C4

[All 127 Digi parts] off
50000702 0000
800DF200 FFFF
800DF20E 7FFF
)";
}

}  // namespace dcb
