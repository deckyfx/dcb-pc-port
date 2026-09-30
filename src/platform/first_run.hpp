#pragma once
// Getting the game data onto the PC: `dcb --import` and the first-run import. The download
// carries no game data; the player imports a dump of their own disc once (hle::import), after
// which the disc image is no longer needed.

#include <filesystem>
#include <string>

namespace platform {

/// `dcb --import <disc.cue|disc.bin> [dest] [--force]`: import and return the process exit
/// code. `argv[1]` is "--import". Default dest: assets/dump/ in the current directory (where
/// hle::Disc::locate looks). `game_id` is the serial this build plays (for the summary).
int import_command(int argc, char** argv, const char* game_id);

/// hle::Disc::locate(serial, hint); when nothing is found and `interactive` (an SDL3 build with a
/// display), explain what is needed, let the player pick their dump, import it into assets/dump/
/// with a progress window, and return the imported tree. Otherwise (headless, cancelled) throws
/// with instructions.
std::filesystem::path locate_or_import(const std::string& serial, const std::filesystem::path& hint, bool interactive);

#if defined(DCB_HAS_SDL3)
/// The SDL3 first-run dialog flow (sdl3_first_run.cpp). Returns the imported tree for `serial`
/// under `dest_root`, or an empty path if the player quit or SDL has no display.
std::filesystem::path sdl3_first_run(const std::string& serial, const std::filesystem::path& dest_root);
#endif

}  // namespace platform
