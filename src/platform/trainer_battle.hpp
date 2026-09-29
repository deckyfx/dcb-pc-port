#pragma once
// Battle actions: the trainer's Battle tab. Each action sets one stat of one player during a card
// battle to a chosen value; the hotkeys apply the enabled ones ([hotkeys] battle_p1 = F10 for P1,
// battle_p2 = F11 for P2) and battle_reset (F12) puts the stats back. The game side lives in
// src/game/overrides/battle.cpp; this is the list, its values and its lines in the cheat file.
// No SDL, no guest memory: unit-tested on its own.
//
// Values follow the game's rules: multiples of 10, from 0 up to 9990 (HP, attacks) or 90 (DP).
//
// In the cheat file the actions are lines the cheat parser never sees:
//   !battle p1_hp on 9990
//   !battle p2_dp off 0

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trainer {

/// NoShuffle is not a value: while it is on, that player's deck is never shuffled (cards are drawn
/// in deck order); the hotkeys skip it. Win has no value either, but the hotkey applies it: that
/// player wins at the next battle phase (score set to 2 of 3, the opponent's HP to 0).
enum class BattleStat : uint8_t { Hp, Circle, Triangle, Cross, Dp, NoShuffle, Win };

struct BattleAction {
    std::string id;     ///< "p1_hp": the name in the cheat file
    int player = 0;     ///< 0 = P1 (you), 1 = P2
    BattleStat stat = BattleStat::Hp;
    bool enabled = false;
    int value = 0;      ///< always a multiple of 10 within [0, max()]
    int max() const { return !has_value() ? 0 : stat == BattleStat::Dp ? 90 : 9990; }
    /// A toggle that works on its own (no value, not applied by a hotkey).
    bool is_toggle() const { return stat == BattleStat::NoShuffle; }
    /// A stat set to `value` (the others are on/off only).
    bool has_value() const { return stat != BattleStat::NoShuffle && stat != BattleStat::Win; }
    /// "P1 HP", "P2 circle attack", ...
    std::string label() const;
};

/// A value made valid: clamped to [0, max] and rounded down to a multiple of 10.
int snap_battle_value(int value, int max);

class BattleActions {
public:
    /// The ten actions (P1 then P2: HP, circle, triangle, cross, DP), then the two no-shuffle
    /// toggles (P1, P2), then the two wins (P1, P2), all off. P1 values start at the game's caps, P2 values at 0 (the useful
    /// direction for each side).
    BattleActions();

    const std::vector<BattleAction>& list() const { return list_; }
    /// Whether `player`'s deck must keep its order (the no-shuffle toggle is on).
    bool no_shuffle(int player) const;
    void set_enabled(size_t i, bool on);
    /// Sets the value (snapped). Returns the value stored.
    int set_value(size_t i, int value);

    /// True for lines this module owns in the cheat file ("!battle ...", "#!battle ...").
    static bool owns_line(std::string_view line);
    /// Reads one "!battle <id> on|off <value>" line. False (and nothing changes) when the line is
    /// not a valid action line; the value is snapped.
    bool parse_line(std::string_view line);
    /// The block for the cheat file: a comment line, then one line per action.
    std::string text() const;

private:
    std::vector<BattleAction> list_;
};

}  // namespace trainer
