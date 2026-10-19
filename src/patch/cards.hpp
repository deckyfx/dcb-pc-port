#pragma once
// New cards: their definitions (config/<serial>/cards/*.ini, built into the program) and the card
// database they go into (docs/re/cards.md, docs/wiki/Cards.md).
//
// A new Digimon card is numbered from 301 up and its record goes right after the 191 Digimon
// records of B:\CARD2.CDD, before the Option and Digivolve records, with the header's Digimon count
// raised to match: the game finds the Option and Digivolve records from that count, and finds a
// Digimon's record from its deck index (record = Digimon base + index * 0x134), so new card 301 is
// Digimon index 191 to everything that works by index. The port maps the card numbers (301 <->
// index 191) where the game works by card number (src/game/cards/).
//
// The definition format (INI; `#` comments; unknown keys are an error):
//   [card]
//   number        301, 302, ... (consecutive)
//   base          the existing Digimon card (0-190) whose record is copied first
//   name          up to 20 letters
//   specialty     fire | ice | nature | darkness | rare
//   level         rookie | armor | champion | ultimate
//   hp, dp, plus_p, fusion, rarity      numbers (rarity 0: never a battle reward)
//   cross_effect  none | first_attack | circle_to_0 | triangle_to_0 | cross_to_0 |
//                 counter_circle | counter_triangle | counter_cross | crash | eat_up_hp |
//                 jamming | x3_fire | x3_ice | x3_nature | x3_darkness | x3_rare
//   support_from  a card (0-190) whose support is copied: its conditions, effects and text
//   model         the battle model, F:\<model>.PAK
//   model_copy_of a model the port serves as <model> when assets/<serial>/files/F/<model>.PAK is
//                 absent (a copy to change later)
//   art           the card art under assets/<serial>/ (an image)
//   [attack circle] / [attack triangle] / [attack cross]
//   name          up to 21 letters
//   power         a number

#include "patch/containers.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace patch::cards {

constexpr int kFirstNew = 301;      ///< the first new card number
constexpr int kDigimon = 191;       ///< the game's Digimon cards (0-190)
constexpr int kDigimonRecord = 0x134;
constexpr size_t kHeader = 8;       ///< "ADCD", u16 Digimon count, u8 Option count, u8 Digivolve count

struct Attack {
    std::string name;
    int power = 0;
};

/// One new Digimon card.
struct Card {
    int number = 0;
    int base = -1;
    std::string name;
    int specialty = 0;  ///< 0 fire .. 4 rare
    int level = 0;      ///< 0 rookie, 1 armor, 2 champion, 3 ultimate
    int hp = 0, dp = 0, plus_p = 0, fusion = 0, rarity = 0;
    int cross_effect = 0;
    int support_from = -1;
    int model = 0, model_copy_of = -1;
    std::string art;
    std::array<Attack, 3> attacks;
};

/// Parses one definition; nullopt (with `why`) on a missing or malformed field.
std::optional<Card> parse(const std::string& text, std::string* why = nullptr);

/// The cards of the built-in definitions, by number (301, 302, ...); a definition that does not
/// parse, or a gap in the numbers, is left out (and reported in `why`).
std::vector<Card> load_builtin(std::string* why = nullptr);

/// The Digimon index of a new card (301 -> 191), or -1.
inline int index_of(int number, size_t count) {
    return number >= kFirstNew && number < kFirstNew + static_cast<int>(count) ? kDigimon + (number - kFirstNew) : -1;
}

/// CARD2.CDD with `cards` inserted after the Digimon records (header count raised); nullopt (with
/// `why`) when the file is not the expected layout or a card's base / support is not a Digimon.
std::optional<Bytes> extend_database(View cdd, const std::vector<Card>& cards, std::string* why = nullptr);

/// F:\nnn.PAK (a battle model: entries {s16 type, s16 id, u32 size, data}, a negative type ends it)
/// as model `to`: the entries the game finds by the model id (type 0 the model, type 5 its
/// texture: `id == from`) re-tagged `to`. nullopt (with `why`) when it is not a PAK.
std::optional<Bytes> copy_model(View pak, int from, int to, std::string* why = nullptr);

/// B:\M_CARD.ARC (the mini cards: u32 offsets, one per card plus the end, then a TIM per card)
/// with an entry per new card appended: `minis[i]` (a TIM) when given and not empty, else a copy
/// of the card's base. nullopt (with `why`) when the file is not the expected layout.
std::optional<Bytes> extend_mini_cards(View arc, const std::vector<Card>& cards, const std::vector<Bytes>& minis = {},
                                       std::string* why = nullptr);

}  // namespace patch::cards
