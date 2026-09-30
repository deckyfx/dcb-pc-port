#pragma once
// Getting the game data onto the PC: `dcb --import` and the first-run import. The download
// carries no game data; the player imports a dump of their own disc once (hle::import), after
// which the disc image is no longer needed. The single-file build unpacks the bundle it carries.

#include <cstdint>
#include <filesystem>
#include <functional>
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

/// The single-file build (vfs/payload.hpp): if this program carries its bundle, unpack it next to
/// itself when that has not been done yet (progress on stdout, plus a small window when
/// `interactive` and SDL3 has a display). Returns false, after saying why (and a message box when
/// interactive), if it could not: the caller exits. A plain binary returns true at once.
bool unpack_bundled_assets(bool interactive);

#if defined(DCB_HAS_SDL3)
/// The SDL3 first-run dialog flow (sdl3_first_run.cpp). Returns the imported tree for `serial`
/// under `dest_root`, or an empty path if the player quit or SDL has no display.
std::filesystem::path sdl3_first_run(const std::string& serial, const std::filesystem::path& dest_root);

/// Run `job` on a worker thread behind a small window showing `caption` and its progress (it calls
/// the given callback with bytes done / total). Returns false, without running `job`, when SDL
/// has no display. The window cannot be closed early: the job always completes.
bool sdl3_progress_window(const char* caption,
                          const std::function<void(const std::function<void(uint64_t, uint64_t)>&)>& job);

/// An error message box (no window of our own needed); false if SDL cannot show one.
bool sdl3_error_box(const char* title, const std::string& text);
#endif

}  // namespace platform
