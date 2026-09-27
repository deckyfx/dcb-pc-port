#pragma once
// Native file access for the game's file API (see files.cpp).

#include <string>

namespace hle {
class Disc;
}

namespace dcb {

/// Serve the game's file API from `disc` (read directly, not through the CD drive model).
void attach_native_files(hle::Disc* disc, const std::string& serial);

}  // namespace dcb
