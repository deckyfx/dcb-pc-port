#pragma once
// Getting the game data onto the PC: `dcb --import`, `dcb --setup` and the first-run setup. The
// download carries no game data. The player provides their own discs once: the Japanese one
// (SLPS-03101, the game that runs) and the North American one (SLUS-01328, where the English
// text and art come from). Both are checked against redump.org (hle::import::verify_dump),
// imported (hle::import), and the English data is built from them (patch::build_all); after
// that the disc images are no longer needed. The single-file build unpacks the bundle it carries.

#include "cdrom/importer.hpp"
#include "patch/patch.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace platform {

/// The disc the English data is made from.
inline constexpr const char* kEnglishSerial = "SLUS-01328";

/// `dcb --import <disc.cue|disc.bin> [dest] [--force]`: import and return the process exit
/// code. `argv[1]` is "--import". Default dest: assets/dump/ in the current directory (where
/// hle::Disc::locate looks). `game_id` is the serial this build plays (for the summary). No
/// verification: this is the plain import, for either disc.
int import_command(int argc, char** argv, const char* game_id);

/// `dcb --setup <jp.cue|bin> <us.cue|bin> [--fixes DIR] [--no-verify] [--force]`: the first-run
/// setup without a window (verify, import both, build the English data into assets/ of the
/// current directory). `argv[1]` is "--setup". Returns the process exit code.
int setup_command(int argc, char** argv, const char* game_id);

/// Dumps are checked against redump.org unless `no_verify_flag` (--no-verify) or DCB_NO_VERIFY=1
/// (a developer's escape hatch for modified images).
bool verify_enabled(bool no_verify_flag);

/// What the first-run setup has to do (setup_needs()).
struct SetupNeeds {
    std::string serial;           ///< the game this build plays (the Japanese disc)
    std::filesystem::path assets; ///< the assets root: dumps in <assets>/dump/, English data in <assets>/<serial>/
    bool need_jp = false;         ///< no imported <assets>/dump/<serial>/ (import the Japanese disc)
    bool need_us = false;         ///< the English data is to be built and <assets>/dump/SLUS-01328/ is missing
    bool need_english = false;    ///< build the English data (patch::build_all)
    bool verify = true;           ///< check the dumps against redump.org
    std::filesystem::path found;  ///< the game data found without any setup (Disc::find), may be empty

    bool any() const { return need_jp || need_english; }
    std::filesystem::path dump_root() const { return assets / "dump"; }
    std::filesystem::path jp_dump() const { return dump_root() / serial; }
    std::filesystem::path us_dump() const { return dump_root() / kEnglishSerial; }
};

/// Decide what the setup must do, given the game data `found` by hle::Disc::find (may be empty).
///
/// When the setup runs. Only for the public download, which carries no game data; the private /
/// developer bundles carry English data made by the Python pipeline (tools/text, tools/assets)
/// and no stamp file, and must never trigger it nor be overwritten. So:
///   - the Japanese game data is missing (as before the English build existed), or
///   - it exists, but there is no English data at all: neither assets/<serial>.pak nor
///     assets/<serial>/text/, in the executable's directory or the current one, or
///   - patch's stamp file (<assets>/<serial>/english.stamp) exists but patch::is_built() is false:
///     a public build that was interrupted (build_english() writes an unfinished stamp first) or
///     was made by an older builder version.
/// A missing English data set with the Japanese data present needs the US dump too; a missing
/// Japanese dump alone (English data present) is just the import.
SetupNeeds setup_needs(const std::string& serial, const std::filesystem::path& found, bool verify);

/// One disc of the setup: identify `image`, and unless <dump_root>/<serial>/ is already a
/// complete import, verify it (if `verify`) and import it. Throws hle::import::ImportError.
struct Imported {
    std::string serial;
    std::filesystem::path dir;
    bool existed = false;  ///< already imported: nothing was verified or written
};
Imported import_checked(const std::filesystem::path& image, const std::filesystem::path& dump_root, bool verify,
                        const hle::import::ProgressFn& progress);

/// patch::build_all for `needs` (fixes_dir may be empty). An unfinished stamp is written first,
/// so an interrupted or failed build is started again on the next run (see setup_needs()).
/// Throws what build_all throws (patch::Cancelled when `progress` returns false).
void build_english(const SetupNeeds& needs, const std::filesystem::path& fixes_dir, const patch::ProgressFn& progress);

/// Boot-time entry: hle::Disc::find(serial, hint), then the setup when setup_needs() says so.
/// `interactive` (an SDL3 build with a display): the first-run window. Otherwise, or without a
/// display: a missing game throws the instructions; missing English data is reported and the game
/// runs in Japanese. Returns the game data to boot; throws when the setup was quit or failed.
std::filesystem::path locate_or_setup(const std::string& serial, const std::filesystem::path& hint, bool interactive,
                                      bool verify);

/// The single-file build (vfs/payload.hpp): if this program carries its bundle, unpack it next to
/// itself when that has not been done yet (progress on stdout, plus a small window when
/// `interactive` and SDL3 has a display). Returns false, after saying why (and a message box when
/// interactive), if it could not: the caller exits. A plain binary returns true at once.
bool unpack_bundled_assets(bool interactive);

#if defined(DCB_HAS_SDL3)
/// The SDL3 first-run window (sdl3_first_run.cpp).
struct FirstRunResult {
    enum Status { Done, Quit, NoDisplay } status = NoDisplay;
    std::filesystem::path game;  ///< Done: the imported Japanese data
};
FirstRunResult sdl3_first_run(const SetupNeeds& needs);

/// Run `job` on a worker thread behind a small window showing `caption` and its progress (it calls
/// the given callback with bytes done / total). Returns false, without running `job`, when SDL
/// has no display. The window cannot be closed early: the job always completes.
bool sdl3_progress_window(const char* caption,
                          const std::function<void(const std::function<void(uint64_t, uint64_t)>&)>& job);

/// An error message box (no window of our own needed); false if SDL cannot show one.
bool sdl3_error_box(const char* title, const std::string& text);
#endif

}  // namespace platform
