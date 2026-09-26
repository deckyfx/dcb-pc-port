#pragma once
// One-time asset import: turns the player's own disc dump (.cue + .bin, or a raw .bin) into the
// extracted form that ExtractedDisc runs from:
//
//   <dest>/<serial>/layout.txt     sector map
//   <dest>/<serial>/iso_meta.bin   raw sectors no file covers (system area, descriptors,
//                                  directories, gaps, post-gap)
//   <dest>/<serial>/fs/...         the ISO9660 files (XA/STR files as whole 2352-byte sectors,
//                                  with a ".raw2352" suffix)
//
// The output is byte-identical to tools/disc/extract_disc.py (layout.txt, iso_meta.bin, fs/); the
// Python tool additionally writes developer files (manifest.json, exe/, system_area.bin) the game
// does not need. This is the player path: no Python, no disc image needed afterwards.
//
// The import is all-or-nothing: everything is written into a hidden temporary directory next to
// the destination, which is renamed into place only once complete.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hle::import {

/// Why an import failed (for a UI that wants to react differently, e.g. "choose another file").
enum class ErrorCode {
    Io,             ///< cannot read the image or write the destination
    UnsupportedFormat,  ///< not a raw 2352-byte image (cooked .iso, .chd, .pbp, ...)
    AudioTrack,     ///< an audio track / audio-only cue sheet: no data
    NotPlayStation, ///< a data CD, but not a PlayStation disc
    UnknownSerial,  ///< a PlayStation disc, but not one this program imports
    Truncated,      ///< the image is shorter than its own filesystem says
    AlreadyExists,  ///< <dest>/<serial> exists and overwrite was not requested
    NoSpace,        ///< not enough free space at the destination
    Cancelled,      ///< the progress callback asked to stop
};

class ImportError : public std::runtime_error {
public:
    ImportError(ErrorCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

/// A track of a cue sheet.
struct CueTrack {
    int number = 0;
    std::string type;             ///< "MODE2/2352", "AUDIO", ...
    std::filesystem::path file;   ///< the FILE it belongs to, resolved against the cue's directory
};

/// Parse cue-sheet text. FILE names may be quoted or not; paths are resolved against `base_dir`.
std::vector<CueTrack> parse_cue(std::string_view text, const std::filesystem::path& base_dir);

/// The image holding the data track: for a .cue, the FILE of its first MODEx track (matched
/// case-insensitively when the exact name is missing, as cue sheets made on Windows often are);
/// anything else is returned as is. Throws ImportError (AudioTrack for audio-only cue sheets).
std::filesystem::path resolve_data_track(const std::filesystem::path& image);

/// "cdrom:\\SLPS_031.01;1" (the BOOT line of SYSTEM.CNF) -> "SLPS-03101"; empty if the boot
/// file name does not look like a serial.
std::string serial_from_boot(std::string_view boot);

/// A disc this program knows about.
struct KnownGame {
    const char* serial;
    const char* title;
};
/// SLPS-03101 (Japan, the version this port runs) and SLUS-01328 (North
/// America; kept for the planned "Japanese code + English assets" build).
inline constexpr KnownGame kKnownGames[] = {
    {"SLPS-03101", "Digimon World: Digital Card Arena (Japan)"},
    {"SLUS-01328", "Digimon Digital Card Battle (North America)"},
};
/// Title of a known serial, or nullptr.
const char* known_title(std::string_view serial);

struct Progress {
    uint64_t done = 0;   ///< work units (sectors) finished
    uint64_t total = 0;  ///< work units overall (fixed once the filesystem has been read)
    const char* stage = "";  ///< "Reading the disc", "Copying files", "Finishing"
    std::string item;        ///< current file, if any
};
/// Called regularly from the importing thread; return false to cancel.
using ProgressFn = std::function<bool(const Progress&)>;

struct Options {
    /// Serials to accept (default: kKnownGames). Anything else fails with UnknownSerial.
    std::vector<std::string> accepted_serials;
    bool overwrite = false;  ///< replace an existing <dest>/<serial>
    ProgressFn progress;
};

struct DiscInfo {
    std::string serial;        ///< from SYSTEM.CNF, e.g. "SLPS-03101"
    std::string volume_id;     ///< ISO9660 volume identifier
    std::filesystem::path data_track;  ///< the .bin actually read
    uint32_t sectors = 0;
};

struct Result {
    DiscInfo disc;
    std::filesystem::path dir;  ///< <dest>/<serial>
    uint32_t files = 0;
    uint64_t bytes = 0;         ///< bytes written
};

/// Check the image and read its serial without writing anything.
DiscInfo identify(const std::filesystem::path& image);

/// Import `image` (.cue or raw .bin) into `dest_root`/<serial>/. Throws ImportError.
Result import_disc(const std::filesystem::path& image, const std::filesystem::path& dest_root, const Options& options = {});

}  // namespace hle::import
