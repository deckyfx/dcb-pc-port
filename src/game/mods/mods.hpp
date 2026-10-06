#pragma once
// Gameplay mods (patch/mods.hpp), applied to game files as the file server opens them
// (overrides/files.cpp). Each mod can be turned off in settings.ini ([mods]).

#include <cstdint>
#include <string>
#include <vector>

namespace dcb::mods {

/// The boss-rematch mod ([mods] boss_rematch, on by default): beaten Battle Arena bosses can be
/// fought again in their city's Battle Cafe.
void set_boss_rematch(bool on);
/// The arena-save mod ([mods] arena_save, on by default): Save in every arena battle menu, not
/// only at the 4th and 7th battles.
void set_arena_saves(bool on);
/// The Player Rooms mod ([mods] player_rooms, on by default): every city's menu lists Player Rooms,
/// not only Beginner City's, Sky City's and Wiseman Tower's.
void set_player_rooms(bool on);
/// Post-game ([mods] postgame_visitors, on by default): Apokarimon stays in Infinity Tower's Battle
/// Cafe and Nanimon in the desert city's, instead of a random city / a dice roll; both can be
/// fought again without leaving the city.
void set_postgame_visitors(bool on);
/// Post-game ([mods] no_win_requirement, on by default): BlackMetalGarurumon and BlackWarGreymon
/// without the 200 / 300 total wins (the story order stays).
void set_no_win_requirement(bool on);

/// True when a mod changes the game file `key` ("C/AREA11.PAK", as the file server names it).
bool wants(const std::string& key);
/// Rewrite `bytes`, the contents of `key`; false (bytes untouched) when no mod applies or the
/// file does not have the structure the mod expects (logged).
bool apply(const std::string& key, std::vector<uint8_t>& bytes);

}  // namespace dcb::mods
