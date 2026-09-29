#pragma once
// The port's built-in cheats (the trainer's General tab), as GameShark text.

#include <string_view>

namespace dcb {

/// Presets for this game (addresses from docs/re/save-data.md).
std::string_view cheat_presets();

}  // namespace dcb
