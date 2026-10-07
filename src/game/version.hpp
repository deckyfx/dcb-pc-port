#pragma once
// Release version and window title ("Digimon Digital Card Battle PC vx.y.z").
// DCB_APP_VERSION comes from CMake (the project VERSION, currently 1.0.1).

#include <string>
#include <string_view>

namespace dcb {

/// The release version, e.g. "1.0.1".
std::string_view app_version();
/// The window title, e.g. "Digimon Digital Card Battle PC v1.0.1".
std::string window_title();

}  // namespace dcb
