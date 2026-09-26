#include "platform.hpp"

namespace platform {

namespace {

class Headless final : public Platform {
public:
    bool pump_events() override { return true; }
    uint16_t pad_buttons(unsigned /*port*/) const override { return 0xFFFF; }  // nothing pressed
    void present(const uint16_t* /*vram*/, const DisplayArea& /*area*/) override {}
    void queue_audio(const int16_t* /*stereo*/, size_t /*frames*/) override {}
};

}  // namespace

std::unique_ptr<Platform> make_headless() { return std::make_unique<Headless>(); }

}  // namespace platform
