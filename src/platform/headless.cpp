#include "platform.hpp"

namespace platform {

namespace {

class Headless final : public Platform {
public:
    bool pump_events() override { return true; }
    uint16_t pad_buttons(unsigned /*port*/) const override { return 0xFFFF; }  // nothing pressed
};

}  // namespace

std::unique_ptr<Platform> make_headless() { return std::make_unique<Headless>(); }

}  // namespace platform
