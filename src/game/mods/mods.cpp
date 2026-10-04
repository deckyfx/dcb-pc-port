#include "mods.hpp"

#include "patch/mods.hpp"

#include <cstdio>

namespace dcb::mods {

namespace {
bool g_boss_rematch = true;
}  // namespace

void set_boss_rematch(bool on) { g_boss_rematch = on; }

bool wants(const std::string& key) { return g_boss_rematch && !patch::mods::rematches_for(key).empty(); }

bool apply(const std::string& key, std::vector<uint8_t>& bytes) {
    if (!wants(key)) return false;
    std::string why;
    std::optional<patch::Bytes> out = patch::mods::patch_city_pak(bytes, patch::mods::rematches_for(key), &why);
    if (!out) {
        std::fprintf(stderr, "[mods] boss rematch: %s left as it is (%s)\n", key.c_str(), why.c_str());
        return false;
    }
    std::printf("[mods] boss rematch: %s\n", key.c_str());
    bytes = std::move(*out);
    return true;
}

}  // namespace dcb::mods
