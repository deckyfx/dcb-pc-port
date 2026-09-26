#pragma once
// Host platform seam: window, presentation, audio and input. The HLE GPU/SPU/pad layers
// talk to this interface, never to SDL directly, so backends stay swappable.

#include <cstdint>
#include <memory>

namespace platform {

class Platform {
public:
    virtual ~Platform() = default;
    /// Process OS events; returns false when the user asked to quit.
    virtual bool pump_events() = 0;
    /// Current PS1 pad state (active-low bit layout, as read from the controller port).
    virtual uint16_t pad_buttons(unsigned port) const = 0;
};

/// Headless backend used until the SDL3 backend exists (and for tests / CI).
std::unique_ptr<Platform> make_headless();

}  // namespace platform
