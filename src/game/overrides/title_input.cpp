// Title screen: any button skips the light-ball animation and answers "Press Start Button".
//
// The title (OPENSEG title_init 801EE850) is a state machine: states 0-5 play the light-ball
// animation, 6-7 wait at "Press Start Button"; both test only Start (0x0800) in the pad's
// "pressed this frame" word (801EEAAC / 801EEB68) and go to state 8, the main menu. The pad words
// are computed once a frame by pad_update (8001ABF4) into the struct at *8008C420 (+10 = pressed
// this frame, PS1 button bits, active high). This override runs the game's pad_update, then, only
// while the title code is resident and in states 0-7, adds Start when any button was pressed.
// The main menu (state 8 on) keeps its own buttons.

#include <psx/recomp.h>

#include <cstdint>

namespace {

constexpr uint32_t kPadUpdate = 0x8001ABF4u;
constexpr uint32_t kPadStruct = 0x8008C420u;  // pointer to the pad words
constexpr uint32_t kPressed = 10;             // u16: buttons pressed this frame
constexpr uint16_t kStart = 0x0800;
constexpr uint32_t kTitleState = 0x801F9EE0u;  // OPENSEG title state (u32)
constexpr uint32_t kLastTitleState = 7;        // 0-5 light ball, 6-7 "Press Start Button"

/// The title code is loaded (overlays share the window, so the state address is only the title's
/// while OPENSEG is resident): the state load and the two Start tests are where they should be.
bool title_resident(PsxContext& ctx) {
    return psx_read32(&ctx, 0x801EEA68u) == 0x8C439EE0u &&  // lw v1, -24864(v0)  (the state)
           psx_read32(&ctx, 0x801EEAACu) == 0x30420800u &&  // andi v0, v0, 0x800 (light ball)
           psx_read32(&ctx, 0x801EEB68u) == 0x30420800u;    // andi v0, v0, 0x800 (Press Start)
}

}  // namespace

extern "C" {

// 8001ABF4: pad_update (once a frame).
void dcb_pad_update(PsxContext* ctx) {
    psx_call_original(ctx, kPadUpdate);
    if (!title_resident(*ctx) || psx_read32(ctx, kTitleState) > kLastTitleState) return;
    const uint32_t pad = psx_read32(ctx, kPadStruct);
    if (pad < 0x80000000u || pad >= 0x80200000u) return;
    const uint16_t pressed = psx_read16(ctx, pad + kPressed);
    if (pressed != 0) psx_write16(ctx, pad + kPressed, static_cast<uint16_t>(pressed | kStart));
}

}  // extern "C"
