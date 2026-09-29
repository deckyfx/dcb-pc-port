// Game toggles (see trainer_toggles.hpp).

#include "trainer_toggles.hpp"

#include <sstream>

namespace trainer {

GameToggles::GameToggles() {
    list_ = {
        {"fusion_mutate", GameToggle::FusionMutate, false, "Fusion: every fusion mutates (special kept)"},
        {"fusion_jewel", GameToggle::FusionJewel, false, "Fusion: mutations give a Digi-Jewel"},
        {"digimentals", GameToggle::Digimentals, false, "All Digimentals (given at the city Menu)"},
    };
}

bool GameToggles::on(GameToggle kind) const {
    for (const ToggleItem& t : list_)
        if (t.kind == kind) return t.enabled;
    return false;
}

void GameToggles::set_enabled(size_t i, bool on) {
    if (i < list_.size()) list_[i].enabled = on;
}

bool GameToggles::owns_line(std::string_view line) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    line.remove_prefix(start);
    return line.substr(0, 7) == "!toggle" || line.substr(0, 8) == "#!toggle";
}

bool GameToggles::parse_line(std::string_view line) {
    std::istringstream in{std::string(line)};
    std::string tag, id, state, extra;
    if (!(in >> tag >> id >> state) || tag != "!toggle" || (state != "on" && state != "off") || (in >> extra))
        return false;
    for (ToggleItem& t : list_) {
        if (t.id != id) continue;
        t.enabled = state == "on";
        return true;
    }
    return false;
}

std::string GameToggles::text() const {
    std::string out = "#!toggle Battle tab, game toggles (Fusion Shop, progression flags):\n";
    for (const ToggleItem& t : list_) out += "!toggle " + t.id + (t.enabled ? " on\n" : " off\n");
    return out;
}

bool fusion_roll_wanted(FusionKind kind, int card, bool need_mutation, bool need_jewel) {
    if (kind == FusionKind::Special) return true;  // a fixed recipe: never re-rolled
    if ((need_mutation || need_jewel) && kind != FusionKind::Mutation) return false;
    return !need_jewel || is_digi_jewel(card);
}

FlagBit city_flag_bit(int reg) {
    const int bit = reg - kFirstCityFlag;
    return {static_cast<uint32_t>(bit / 8), static_cast<uint8_t>(1u << (bit % 8))};
}

bool set_digimental_flags(uint8_t* flags) {
    bool changed = false;
    for (const int reg : kDigimentalFlags) {
        const FlagBit b = city_flag_bit(reg);
        if ((flags[b.byte] & b.mask) == 0) {
            flags[b.byte] = static_cast<uint8_t>(flags[b.byte] | b.mask);
            changed = true;
        }
    }
    return changed;
}

}  // namespace trainer
