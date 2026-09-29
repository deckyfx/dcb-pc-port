#pragma once
// Game toggles: the trainer's on/off switches outside a card battle (shown on the General tab under
// the presets). Fusion Shop rolls and progression flags; the game side lives in
// src/game/overrides/fusion.cpp, the flags in docs/re/save-data.md ("Progression flags").
// No SDL, no guest runtime: unit-tested on its own (tests/trainer).
//
// In the cheat file the toggles are lines the cheat parser never sees:
//   !toggle fusion_mutate on
//   !toggle digimentals off

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace trainer {

enum class GameToggle : uint8_t {
    FusionMutate,  ///< every card fusion mutates (special fusions are kept)
    FusionJewel,   ///< a mutation always gives a Digi-Jewel (cards 273-284)
    Digimentals,   ///< the 13 Digimental city flags are held set (handed out at the next city Menu)
};

struct ToggleItem {
    std::string id;     ///< "fusion_mutate": the name in the cheat file
    GameToggle kind = GameToggle::FusionMutate;
    bool enabled = false;
    std::string label;  ///< the panel text
    /// The status line when it is switched on, when the label alone does not say what to do next
    /// (empty: "<label> on").
    std::string on_status;
};

class GameToggles {
public:
    /// The toggles, all off.
    GameToggles();

    const std::vector<ToggleItem>& list() const { return list_; }
    bool on(GameToggle kind) const;
    void set_enabled(size_t i, bool on);

    /// True for lines this module owns in the cheat file ("!toggle ...", "#!toggle ...").
    static bool owns_line(std::string_view line);
    /// Reads one "!toggle <id> on|off" line. False (and nothing changes) when it is not valid.
    bool parse_line(std::string_view line);
    /// The block for the cheat file: a comment line, then one line per toggle.
    std::string text() const;

private:
    std::vector<ToggleItem> list_;
};

// ---------------------------------------------------------------------------------------------
// Fusion Shop roll (EVOSEG fusion_roll, 801EF738; docs/re/save-data.md)
// ---------------------------------------------------------------------------------------------

/// What fusion_roll decided (EVOSEG 801F7FD5, also script register r17).
enum class FusionKind : uint8_t { Normal = 0, Special = 1, Mutation = 2 };

/// Digi-Jewel cards (Digi-Garnet 273 .. Digi-Turquoise 284): only a fusion mutation makes them.
inline constexpr int kFirstDigiJewel = 273;
inline constexpr int kLastDigiJewel = 284;
inline bool is_digi_jewel(int card) { return card >= kFirstDigiJewel && card <= kLastDigiJewel; }

/// Whether a roll gives what the toggles ask for. A special fusion (a fixed recipe) is always
/// kept; otherwise `need_mutation` wants a mutation and `need_jewel` a mutation into a Digi-Jewel.
bool fusion_roll_wanted(FusionKind kind, int card, bool need_mutation, bool need_jewel);

// ---------------------------------------------------------------------------------------------
// City progression flags (game_data + 0x23CC; docs/re/save-data.md)
// ---------------------------------------------------------------------------------------------

/// Offset of the city flag bits in game_data: script register rN (12 <= N <= 362) is bit N-12.
inline constexpr uint32_t kCityFlagsOffset = 0x23CC;
inline constexpr int kFirstCityFlag = 12;
inline constexpr int kLastCityFlag = 362;

/// The Digimental flags (SAISEG table 801E0EE4, digimental_sync): Veemon's three, then two each
/// for Hawkmon, Armadillomon, Gatomon, Patamon and Wormmon.
inline constexpr std::array<int, 13> kDigimentalFlags = {295, 296, 297, 299, 300, 302, 303,
                                                         308, 309, 305, 306, 311, 312};

/// Byte offset (from kCityFlagsOffset) and bit mask of city flag register `reg`.
struct FlagBit {
    uint32_t byte = 0;
    uint8_t mask = 0;
};
FlagBit city_flag_bit(int reg);

/// Sets the Digimental flags in the city flag bytes `flags` (kCityFlagsOffset onwards, at least
/// 44 bytes). Returns true when a byte changed.
bool set_digimental_flags(uint8_t* flags);

}  // namespace trainer
