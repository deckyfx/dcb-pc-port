#pragma once
// SDL3 glue for the trainer panel (sdl3_trainer.cpp); used by the SDL3 backend only.

#if defined(DCB_HAS_SDL3)

#include "trainer.hpp"

#include <SDL3/SDL.h>

#include <vector>

namespace platform {

/// Route one SDL event to the trainer. A key in `toggle_keys` ([hotkeys] trainer) opens or
/// closes the panel; while it is open every key-down and text-input event is the panel's
/// (Escape closes it). Returns true if the event was consumed. Starts / stops SDL text input
/// on `window` as the panel opens and closes.
bool trainer_handle_event(trainer::Trainer& t, SDL_Window* window, const SDL_Event& ev,
                          const std::vector<int>& toggle_keys);

/// Draw the panel centred over the current frame, scaled to stay readable at any window size.
void trainer_draw(SDL_Renderer* renderer, const trainer::Trainer& t);

}  // namespace platform

#endif  // DCB_HAS_SDL3
