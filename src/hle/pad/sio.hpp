#pragma once
// Controller port (SIO0, 0x1F801040-0x1F80104F) with a digital pad in port 1. libpad reads the
// pads itself over this serial port from its VBLANK handler, byte by byte, waiting for each ACK.
// Memory cards are served by the BIOS layer, so card addresses (81h) get no answer here.
// Reference: psx-spx "Controllers and Memory Cards" (I/O ports, controller protocol).

#include <array>
#include <cstdint>
#include <functional>

namespace hle {

class Sio0 {
public:
    static constexpr uint32_t kBase = 0x1F801040, kEnd = 0x1F801050;

    /// `raise_irq7` fires when a byte is acknowledged with ACK interrupts enabled.
    explicit Sio0(std::function<void()> raise_irq7) : raise_irq7_(std::move(raise_irq7)) {}

    uint32_t read(uint32_t phys, unsigned width);
    void write(uint32_t phys, uint32_t value, unsigned width);

    /// Digital pad state, active low (0xFFFF = nothing pressed).
    void set_buttons(unsigned port, uint16_t buttons) { buttons_[port & 1u] = buttons; }

private:
    std::function<void()> raise_irq7_;
    std::array<uint16_t, 2> buttons_{0xFFFF, 0xFFFF};
    uint16_t ctrl_ = 0, mode_ = 0, baud_ = 0;
    uint8_t rx_ = 0xFF;
    bool rx_full_ = false, ack_ = false, irq_ = false;
    unsigned index_ = 0;   ///< byte position in the current transaction
    bool selected_ = false, talking_to_pad_ = false;

    void transfer(uint8_t tx);
};

}  // namespace hle
