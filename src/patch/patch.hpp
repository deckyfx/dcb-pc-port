#pragma once
// English data for the JP build, made on the player's machine from their own two dumps (the
// public channel: the program ships without any game data). C++ port of the offline Python
// pipeline (tools/text/en_text.py, tools/assets/swap_us_images.py); outputs match it byte for
// byte. No SDL: used by the first-run setup (src/platform) and the dcb_patch CLI.
//
// Inputs are imported dumps (hle::import, <root>/<serial>/ with fs/ and exe/). Outputs go under
// an assets root, laid out as the game reads them:
//   <assets>/<serial>/         en_font.bin, en_bigfont.bin, en_names.txt, text/, files/, disc/
//   <assets>/<serial>.pak      the US art (replacement textures) and sprites.txt

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace patch {

struct Progress {
    const char* stage = "";  ///< "English text", "US art", ...
    uint64_t done = 0;
    uint64_t total = 0;
};
/// Called from the building thread; return false to cancel (build_* then throw Cancelled).
using ProgressFn = std::function<bool(const Progress&)>;

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

struct Inputs {
    std::filesystem::path jp_dump;    ///< imported SLPS-03101 (fs/, exe/)
    std::filesystem::path us_dump;    ///< imported SLUS-01328 (fs/, exe/)
    std::filesystem::path fixes_dir;  ///< optional: community .xdelta fixes ("" = none)
    std::filesystem::path assets;     ///< the assets root to write into
    std::string serial = "SLPS-03101";
};

/// The loose English files under <assets>/<serial>/ (en_text.py's outputs).
void build_text(const Inputs& in, const ProgressFn& progress = {});
/// The US art: <assets>/<serial>.pak holding the us/*.raw uploads swap_us_images.py makes and
/// their manifest (art_swap.hpp; no JP PNGs: without an entry the game draws its own upload), and
/// the US movie as the disc override <assets>/<serial>/disc/DIGIMON.MOV.raw2352 (same size only).
void build_art(const Inputs& in, const ProgressFn& progress = {});
/// Both, then the stamp file (kStampName) that marks the English data complete.
void build_all(const Inputs& in, const ProgressFn& progress = {});

/// <assets>/<serial>/<kStampName>: written last by build_all; the first-run setup builds again
/// when it is missing (an interrupted build) or names another builder version.
inline constexpr const char* kStampName = "english.stamp";
bool is_built(const std::filesystem::path& assets, const std::string& serial);

}  // namespace patch
