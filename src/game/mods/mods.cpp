#include "mods.hpp"

#include "patch/mods.hpp"

#include <cstdio>

namespace dcb::mods {

namespace {
bool g_boss_rematch = true;
bool g_arena_saves = true;
bool g_player_rooms = true;
}  // namespace

void set_boss_rematch(bool on) { g_boss_rematch = on; }
void set_arena_saves(bool on) { g_arena_saves = on; }
void set_player_rooms(bool on) { g_player_rooms = on; }

namespace {

/// The mods for one city file ("C/AREA05.PAK"); none for other files.
patch::mods::CityMods mods_for(const std::string& key) {
    patch::mods::CityMods m;
    const bool city = key.size() == 12 && key.rfind("C/AREA", 0) == 0 && key.compare(8, 4, ".PAK") == 0;
    if (!city) return m;
    if (g_boss_rematch) m.rematches = patch::mods::rematches_for(key);
    m.arena_saves = g_arena_saves;
    m.player_rooms = g_player_rooms;
    return m;
}

}  // namespace

bool wants(const std::string& key) {
    const patch::mods::CityMods m = mods_for(key);
    return !m.rematches.empty() || m.arena_saves || m.player_rooms;
}

bool apply(const std::string& key, std::vector<uint8_t>& bytes) {
    const patch::mods::CityMods m = mods_for(key);
    if (m.rematches.empty() && !m.arena_saves && !m.player_rooms) return false;
    std::string why;
    std::optional<patch::Bytes> out = patch::mods::patch_city_pak(bytes, m, &why);
    if (!why.empty()) std::fprintf(stderr, "[mods] %s: not applied: %s\n", key.c_str(), why.c_str());
    if (!out) return false;
    std::printf("[mods] %s patched\n", key.c_str());
    bytes = std::move(*out);
    return true;
}

}  // namespace dcb::mods
