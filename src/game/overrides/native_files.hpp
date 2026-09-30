#pragma once
// Native file access for the game's file API (see files.cpp).

#include <string>

namespace hle {
class Disc;
}

namespace dcb {

/// Serve the game's file API from `disc` (read directly, not through the CD drive model).
/// `assets_dir`: the assets/<serial>/ folder holding loose files (may be "" when absent);
/// the CWD-relative one is always tried as well. Call before the game opens any file.
void attach_native_files(hle::Disc* disc, const std::string& serial, const std::string& assets_dir = {});

/// Loose asset file `rel` under assets/<serial>/: the attached folder first, then the
/// CWD-relative one. "" when neither exists.
std::string asset_path(const std::string& rel);

}  // namespace dcb
