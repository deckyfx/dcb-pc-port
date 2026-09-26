#pragma once
// Host platform seam: window, presentation, audio and input. The HLE GPU/SPU/pad layers
// talk to this interface, never to SDL directly, so backends stay swappable.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace platform {

/// PS1 VRAM geometry: 1024x512 16-bit words.
inline constexpr int kVramWidth = 1024;
inline constexpr int kVramHeight = 512;

/// Output sample rate for queue_audio (the SPU's native rate).
inline constexpr int kAudioRate = 44100;

/// The part of VRAM the PS1 is showing (from GP1 display settings).
struct DisplayArea {
    int x = 0, y = 0;           ///< top-left in VRAM (in 24-bit mode x is in 16-bit units, as on hardware)
    int width = 320, height = 240;
    bool rgb24 = false;         ///< 24-bit mode (FMV) vs 15-bit
    bool enabled = true;        ///< display disabled -> present black
};

/// PS1 digital pad bits. pad_buttons() is active LOW: a pressed button clears its bit.
enum Button : uint16_t {
    Select = 1u << 0,
    L3 = 1u << 1,
    R3 = 1u << 2,
    Start = 1u << 3,
    Up = 1u << 4,
    Right = 1u << 5,
    Down = 1u << 6,
    Left = 1u << 7,
    L2 = 1u << 8,
    R2 = 1u << 9,
    L1 = 1u << 10,
    R1 = 1u << 11,
    Triangle = 1u << 12,
    Circle = 1u << 13,
    Cross = 1u << 14,
    Square = 1u << 15,
};

class Platform {
public:
    virtual ~Platform() = default;
    /// Process OS events; returns false when the user asked to quit (window closed / Escape).
    virtual bool pump_events() = 0;
    /// Current PS1 pad state (active-low bit layout); 0xFFFF = nothing pressed.
    virtual uint16_t pad_buttons(unsigned port) const = 0;
    /// Show the display area of `vram` (1024x512 PS1 pixels). Cheap; call once per VBLANK.
    virtual void present(const uint16_t* vram, const DisplayArea& area) = 0;
    /// Queue `frames` interleaved stereo s16 frames at kAudioRate. Latency is kept bounded.
    virtual void queue_audio(const int16_t* stereo, size_t frames) = 0;
};

/// Headless backend: no window, no audio, nothing pressed (tests / CI / batch runs).
std::unique_ptr<Platform> make_headless();

#if defined(DCB_HAS_SDL3)
/// SDL3 backend: window + renderer, keyboard/gamepad on port 0, audio stream, configured from
/// settings.ini at its default location (DCB_SETTINGS, portable next to the executable, or the
/// per-user config directory; see settings.hpp). Returns nullptr (after logging) if SDL cannot
/// initialise video.
std::unique_ptr<Platform> make_sdl3(const char* title);
/// As above with an explicit settings file (created with defaults if it does not exist).
std::unique_ptr<Platform> make_sdl3(const char* title, const std::filesystem::path& settings_path);
#endif

/// Convert the display area to RGBA8888 (bytes R,G,B,A in memory, i.e. the uint32 value is
/// R | G<<8 | B<<16 | 0xFF<<24 on a little-endian host), for any backend and for tests.
/// `out_rgba` must hold width*height pixels. A disabled display produces opaque black.
/// X and Y wrap around VRAM like the hardware display fetcher.
void convert_display(const uint16_t* vram, const DisplayArea& area, uint32_t* out_rgba);

}  // namespace platform
