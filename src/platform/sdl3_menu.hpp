#pragma once
// SDL3 glue for the native pause menu (sdl3_menu.cpp); used by the SDL3 backend only.

#if defined(DCB_HAS_SDL3)

#include "menu.hpp"

#include <SDL3/SDL.h>

#include <vector>

namespace platform {

/// Route one SDL event to the menu. The menu toggle ([hotkeys] menu, default
/// F1) and Escape open/close it; while open, arrows/Enter/Escape navigate and
/// gamepad d-pad/south/east do the same (Start+Select opens it, Start closes).
/// `trainer_open`: when true, toggle keys are left for the trainer (no menu
/// on top of the panel). Returns the action for main.cpp (Action::None if the
/// event only moved the selection or was not for the menu).
menu::Action menu_handle_event(menu::Menu& m, const SDL_Event& ev, const std::vector<int>& toggle_keys,
                               SDL_Gamepad* gamepad, bool& start_held, bool& select_held, bool trainer_open);

/// Track Start/Select holds for the Start+Select chord (call on every gamepad
/// button event, even when the menu is closed).
void menu_track_chord(const SDL_Event& ev, bool& start_held, bool& select_held);

/// Draw the menu centred over the current frame, scaled like the trainer panel.
void menu_draw(SDL_Renderer* renderer, const menu::Menu& m);

}  // namespace platform

#endif  // DCB_HAS_SDL3
