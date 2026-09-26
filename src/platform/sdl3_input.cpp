// SDL name lookups for [keyboard] / [gamepad] bindings. Only SDL's static name tables are
// used, so this works (and is unit-tested) without SDL_Init or any device.

#if defined(DCB_HAS_SDL3)

#include "settings.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace platform {

namespace {

std::optional<int> resolve_scancode(std::string_view name) {
    if (name.empty()) return std::nullopt;
    const std::string n(name);
    if (SDL_strcasecmp(n.c_str(), "Comma") == 0) return SDL_SCANCODE_COMMA;  // ',' separates list items
    const SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return std::nullopt;
    return static_cast<int>(sc);
}

std::optional<int> resolve_gamepad(std::string_view name) {
    if (name.empty()) return std::nullopt;

    // Positional face-button names read the same on every controller brand.
    struct Alias {
        const char* name;
        SDL_GamepadButton button;
    };
    static constexpr Alias kAliases[] = {
        {"south", SDL_GAMEPAD_BUTTON_SOUTH},
        {"east", SDL_GAMEPAD_BUTTON_EAST},
        {"west", SDL_GAMEPAD_BUTTON_WEST},
        {"north", SDL_GAMEPAD_BUTTON_NORTH},
    };
    const std::string n(name);
    for (const Alias& a : kAliases)
        if (SDL_strcasecmp(n.c_str(), a.name) == 0) return static_cast<int>(a.button);

    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(n.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) return static_cast<int>(button);

    // Axis halves: "+leftx" / "-lefty"; triggers only have a positive half and may omit '+'.
    const char sign = (n[0] == '+' || n[0] == '-') ? n[0] : '\0';
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(n.c_str() + (sign != '\0' ? 1 : 0));
    if (axis == SDL_GAMEPAD_AXIS_INVALID) return std::nullopt;
    if (sdl3_axis_is_trigger(axis)) {
        if (sign == '-') return std::nullopt;
        return encode_gamepad_axis(axis, false);
    }
    if (sign == '\0') return std::nullopt;  // a stick axis needs a direction
    return encode_gamepad_axis(axis, sign == '-');
}

}  // namespace

bool sdl3_axis_is_trigger(int axis) {
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

BindingResolvers sdl3_binding_resolvers() { return BindingResolvers{resolve_scancode, resolve_gamepad}; }

}  // namespace platform

#endif  // DCB_HAS_SDL3
