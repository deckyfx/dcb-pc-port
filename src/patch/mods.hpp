#pragma once
// Gameplay mods applied to game files as the game loads them (src/game/mods/mods.cpp serves them
// through the file API). Each one patches data the game already reads; nothing here is game data.
//
// Boss rematches: the Battle Arena bosses (the usurpers who take over an arena, and the story
// bosses) can be fought once; afterwards they are gone. This mod adds them to the same city's
// Battle Cafe once beaten:
//   Jungle City (C\AREA02.PAK)      Wormmon, Tryout Deck (deck 10), cafe slot 15, after r56 = 1
//   Igloo City (C\AREA03.PAK)       Stingmon, Black Storm (deck 14), slot 14, after r87 = 1
//   Junk City (C\AREA04.PAK)        Shadramon, Evil Fire (deck 18), slot 6, after r119 = 1
//   Dark City (C\AREA05.PAK)        the Digimon Emperor (deck 23), slot 11, after r138 = 1
//   Infinity Tower (C\AREA11.PAK)   A (deck 140), slot 14, after r185 = 1 and r184 = 0
// Their faces are in the city's cafe portrait sheet already, in cells the cafe never lists (the
// usurpers' cells belong to deck-info entries: Wormmon's is listed only until he is beaten,
// Stingmon's and Shadramon's never), so a rematch never takes a slot of a regular member, who
// joins the cafe later in the story (add_rematches refuses such a slot).
//
// The city script (MSD, docs/re/text-engine.md §5.1; cafe structure in docs/re/battle-cafe.md) is
// only appended to: existing records keep their offsets, because a save made in a city stores
// its script position (city host 0x0B cmd 6). The patch redirects the cafe menu (`cmd2()`), its
// fall-through record and its selection dispatch into new code at the end of the script.

#include "patch/containers.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace patch::mods {

/// A script register test: true when r`reg` == `value`.
struct RegEquals {
    uint16_t reg = 0;
    int32_t value = 0;
};

/// One boss added to a city's Battle Cafe.
struct Rematch {
    uint16_t slot = 0;               ///< cafe list entry (`cmd3(slot)`): face sheet cell, 0-15
    uint16_t deck = 0;               ///< DECK2.DEK record of the opponent
    std::vector<RegEquals> unlocked; ///< all true: listed (the boss has been beaten)
    std::string name;                ///< speaker line ("*c4" name "*c7")
    std::string challenge;           ///< asked before the Yes / No choice
    std::string declined;            ///< after "No"
    std::string player_won;          ///< after the player wins
    std::string player_lost;         ///< after the player loses
};

/// The rematches of a city script file ("C/AREA05.PAK"); empty for any other file.
const std::vector<Rematch>& rematches_for(const std::string& file);

/// The MSD city script with `list` added to its Battle Cafe, or nullopt (with `why`) when the
/// script does not have the expected cafe structure.
std::optional<Bytes> add_rematches(View script, const std::vector<Rematch>& list, std::string* why = nullptr);

/// A city PAK (C\AREAnn.PAK) with its script chunk passed through add_rematches; nullopt as above.
std::optional<Bytes> patch_city_pak(View pak, const std::vector<Rematch>& list, std::string* why = nullptr);

}  // namespace patch::mods
