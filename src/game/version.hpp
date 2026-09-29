#pragma once
// Release version and window title ("Digimon Digital Card Battle PC vx.y.z").
// DCB_APP_VERSION comes from CMake (the project VERSION, currently 0.1.0).

#include <string>
#include <string_view>

namespace dcb {

/// The release version, e.g. "0.1.0".
std::string_view app_version();
/// The window title, e.g. "Digimon Digital Card Battle PC v0.1.0".
std::string window_title();

}  // namespace dcb
