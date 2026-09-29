// Fusion Shop and progression toggles (the trainer's game toggles, trainer_toggles.hpp).
//
// Card fusion (EVOSEG, the city's Fusion Shop; docs/re/save-data.md "Fusion Shop"): when the
// second card is picked, fusion_roll (EVOSEG 801EF738) decides the result once:
//   - a fixed recipe from the special-fusion table (EVOSEG 801E1AFC) is a special fusion;
//   - else rand() % 100 <= (fusion value A + B) / 10 (card byte +0x18) is a mutation: then
//     rand() % 100 < 21 gives a Digi-Jewel (273 + rand() % 12; Fake Sevens, card 200, when six
//     are owned), < 61 Fake Sevens, else a card of a higher fusion value;
//   - else the normal result.
// It leaves the kind at 801F7FD5 (0 normal, 1 special, 2 mutation) and the card at 801F7FC0, and
// sets the shop script's registers (r11, r13 mutation, r14 special, r15/r16 type, r17 kind).
//
// The Mutation Detector (the Fusion Mutation prediction data, Izzy's reward: city flag r267,
// copied to game_data + 0x2C bit 1 when the Fusion Shop opens) does not block anything: with it
// the shop keeper warns "this fusion will mutate" and the player may still say yes. The JP text
// says the data tells you in advance; the US text ("It prevents mutations") is a mistranslation.
//
// dcb_fusion_roll re-runs the original roll until it gives what the toggles ask for: every
// fusion a mutation ("fusion_mutate"), mutations into a Digi-Jewel ("fusion_jewel"). A special
// fusion is always kept. The roll only reads the two cards and rand(), and each run rewrites all
// of its outputs, so re-running it is the game's own logic with other random numbers.

#include "fusion.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kFusionRoll = 0x801EF738u;  // EVOSEG fusion_roll
constexpr uint32_t kRollKind = 0x801F7FD5u;    // u8: 0 normal, 1 special, 2 mutation
constexpr uint32_t kRollCard = 0x801F7FC0u;    // s16: the resulting card
constexpr uint32_t kGameDataPtr = 0x80070C2Cu; // game_data*
/// Enough re-rolls for the rarest ask (a mutation at 1% into one of the jewels the player still
/// lacks); if none comes, the last roll stands.
constexpr int kMaxRolls = 20000;

const trainer::GameToggles* g_toggles = nullptr;

bool in_ram(uint32_t addr) { return addr >= 0x80000000u && addr < 0x80200000u - 0x4000u; }

trainer::FusionKind roll_kind(PsxContext& ctx) {
    return static_cast<trainer::FusionKind>(psx_read8(&ctx, kRollKind));
}
int roll_card(PsxContext& ctx) { return static_cast<int16_t>(psx_read16(&ctx, kRollCard)); }

}  // namespace

namespace dcb {

void game_toggles_frame(PsxContext& ctx, const trainer::GameToggles& toggles) {
    g_toggles = &toggles;
    if (!toggles.on(trainer::GameToggle::Digimentals)) return;
    // Held like a cheat: set every frame while on, so a loaded save gets them too.
    const uint32_t game_data = psx_read32(&ctx, kGameDataPtr);
    if (!in_ram(game_data)) return;
    const uint32_t flags = game_data + trainer::kCityFlagsOffset;
    for (const int reg : trainer::kDigimentalFlags) {
        const trainer::FlagBit b = trainer::city_flag_bit(reg);
        const uint8_t v = psx_read8(&ctx, flags + b.byte);
        if ((v & b.mask) == 0) psx_write8(&ctx, flags + b.byte, static_cast<uint8_t>(v | b.mask));
    }
}

}  // namespace dcb

extern "C" {

// EVOSEG 801EF738: fusion_roll() -> the resulting card (see the top of this file).
void dcb_fusion_roll(PsxContext* ctx) {
    psx_call_original(ctx, kFusionRoll);
    if (!g_toggles) return;
    const bool mutate = g_toggles->on(trainer::GameToggle::FusionMutate);
    const bool jewel = g_toggles->on(trainer::GameToggle::FusionJewel);
    // "Jewel" alone changes only the fusions that mutated anyway.
    if (!mutate && !(jewel && roll_kind(*ctx) == trainer::FusionKind::Mutation)) return;
    int rolls = 1;
    while (!trainer::fusion_roll_wanted(roll_kind(*ctx), roll_card(*ctx), true, jewel) && rolls < kMaxRolls) {
        psx_call_original(ctx, kFusionRoll);
        ++rolls;
    }
    static const bool trace = std::getenv("DCB_TRACE_FUSION") != nullptr;
    if (trace)
        std::fprintf(stderr, "[fusion] kind %d card %d after %d roll(s)\n", static_cast<int>(roll_kind(*ctx)),
                     roll_card(*ctx), rolls);
}

}  // extern "C"
