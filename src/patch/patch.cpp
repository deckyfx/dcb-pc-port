#include "patch.hpp"

#include <fstream>

namespace patch {

namespace {
/// Bumped whenever the outputs change, so older English data is rebuilt.
constexpr const char* kBuilderVersion = "1";
}  // namespace

void build_all(const Inputs& in, const ProgressFn& progress) {
    build_text(in, progress);
    build_art(in, progress);
    std::ofstream(in.assets / in.serial / kStampName) << kBuilderVersion << "\n";
}

bool is_built(const std::filesystem::path& assets, const std::string& serial) {
    std::ifstream in(assets / serial / kStampName);
    std::string version;
    return static_cast<bool>(std::getline(in, version)) && version == kBuilderVersion;
}

}  // namespace patch
