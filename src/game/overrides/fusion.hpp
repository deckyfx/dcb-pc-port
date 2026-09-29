#pragma once
// Game toggles outside battle (see fusion.cpp): the Fusion Shop roll and the progression flags
// held by the trainer's toggles (trainer_toggles.hpp).

#include "trainer_toggles.hpp"

#include <psx/recomp.h>

namespace dcb {

/// Once per frame at the frame boundary: remembers the toggles for the fusion override and holds
/// the flags of the toggles that are on (the Digimental city flags).
void game_toggles_frame(PsxContext& ctx, const trainer::GameToggles& toggles);

}  // namespace dcb
