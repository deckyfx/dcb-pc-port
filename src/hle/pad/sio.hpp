#pragma once
// Controller port (SIO0, 0x1F801040-0x1F80104F) with a digital pad in port 1. libpad reads the
// pads itself over this serial port from its VBLANK handler, byte by byte, waiting for each ACK.
// Memory cards are served by the BIOS layer, so card addresses (81h) get no answer here.
// Reference: psx-spx "Controllers and Memory Cards" (I/O ports, controller protocol).

#include <psx/state.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace hle {

class Sio0 {
public:
    static constexpr uint32_t kBase = 0x1F801040, kEnd = 0x1F801050;

    /// `raise_irq7` fires when a byte is acknowledged with ACK interrupts enabled; `clock` is the
    /// current guest time in CPU cycles. Transfers take real time: libpad clears IRQ7 right after
    /// sending a byte and then waits for the pad's ACK to raise it again, so an instant ACK is lost.
    Sio0(std::function<void()> raise_irq7, std::function<uint64_t()> clock)
        : raise_irq7_(std::move(raise_irq7)), clock_(std::move(clock)) {}

    /// Advance to guest time `cycles`: completes the byte in flight and raises the ACK interrupt.
    void tick(uint64_t cycles);

    uint32_t read(uint32_t phys, unsigned width);
    void write(uint32_t phys, uint32_t value, unsigned width);

    /// Digital pad state, active low (0xFFFF = nothing pressed).
    void set_buttons(unsigned port, uint16_t buttons) { buttons_[port & 1u] = buttons; }
    /// Last button bytes the game actually read for port 1 (lo | hi << 8), for tracing.
    uint16_t last_sent() const { return last_sent_; }

    /// Save state: port registers, the byte in flight and the pad protocol position ("SIO0").
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

private:
    std::function<void()> raise_irq7_;
    std::function<uint64_t()> clock_;
    std::array<uint16_t, 2> buttons_{0xFFFF, 0xFFFF};
    uint16_t ctrl_ = 0, mode_ = 0, baud_ = 0;
    uint8_t rx_ = 0xFF;
    bool rx_full_ = false, irq_ = false;
    // The byte in flight: its reply lands at done_at_, the /ACK pulse spans [ack_at_, ack_end_).
    bool busy_ = false, ack_pending_ = false, rx_pending_ = false;
    uint8_t rx_next_ = 0xFF;
    uint64_t done_at_ = 0, ack_at_ = 0, ack_end_ = 0;
    uint16_t last_sent_ = 0xFFFF;
    unsigned index_ = 0;   ///< byte position in the current transaction
    bool selected_ = false, talking_to_pad_ = false;

    void transfer(uint8_t tx);
    uint32_t read_reg(uint32_t phys, unsigned width);
};

}  // namespace hle
