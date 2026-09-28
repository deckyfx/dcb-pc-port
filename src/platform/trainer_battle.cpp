// Battle actions (see trainer_battle.hpp).

#include "trainer_battle.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace trainer {

namespace {

struct StatInfo {
    BattleStat stat;
    const char* id;
    const char* name;
};
constexpr StatInfo kStats[] = {
    {BattleStat::Hp, "hp", "HP"},
    {BattleStat::Circle, "circle", "circle attack"},
    {BattleStat::Triangle, "triangle", "triangle attack"},
    {BattleStat::Cross, "cross", "cross attack"},
    {BattleStat::Dp, "dp", "DP"},
};

}  // namespace

std::string BattleAction::label() const {
    for (const StatInfo& s : kStats)
        if (s.stat == stat) return std::string(player == 0 ? "P1 " : "P2 ") + s.name;
    return "?";
}

int snap_battle_value(int value, int max) { return std::clamp(value, 0, max) / 10 * 10; }

BattleActions::BattleActions() {
    for (int player = 0; player < 2; ++player) {
        for (const StatInfo& s : kStats) {
            BattleAction a;
            a.id = std::string(player == 0 ? "p1_" : "p2_") + s.id;
            a.player = player;
            a.stat = s.stat;
            a.value = player == 0 ? a.max() : 0;
            list_.push_back(a);
        }
    }
}

void BattleActions::set_enabled(size_t i, bool on) {
    if (i < list_.size()) list_[i].enabled = on;
}

int BattleActions::set_value(size_t i, int value) {
    if (i >= list_.size()) return 0;
    list_[i].value = snap_battle_value(value, list_[i].max());
    return list_[i].value;
}

bool BattleActions::owns_line(std::string_view line) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    line.remove_prefix(start);
    return line.substr(0, 7) == "!battle" || line.substr(0, 8) == "#!battle";
}

bool BattleActions::parse_line(std::string_view line) {
    std::istringstream in{std::string(line)};
    std::string tag, id, state, value_text;
    if (!(in >> tag >> id >> state >> value_text) || tag != "!battle" || (state != "on" && state != "off")) return false;
    char* end = nullptr;
    const long value = std::strtol(value_text.c_str(), &end, 10);
    if (end == value_text.c_str() || *end != 0) return false;
    for (BattleAction& a : list_) {
        if (a.id != id) continue;
        a.enabled = state == "on";
        a.value = snap_battle_value(static_cast<int>(std::clamp<long>(value, 0, a.max())), a.max());
        return true;
    }
    return false;
}

std::string BattleActions::text() const {
    std::string out = "#!battle Battle tab (F10 applies the P1 lines that are on, F11 the P2 lines, F12 resets):\n";
    for (const BattleAction& a : list_)
        out += "!battle " + a.id + (a.enabled ? " on " : " off ") + std::to_string(a.value) + "\n";
    return out;
}

}  // namespace trainer
