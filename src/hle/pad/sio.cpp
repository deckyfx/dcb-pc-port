#include "pad/sio.hpp"

namespace hle {

namespace {

constexpr uint32_t kData = 0x1F801040, kStat = 0x1F801044, kMode = 0x1F801048, kCtrl = 0x1F80104A,
                   kBaud = 0x1F80104E;

constexpr uint16_t kCtrlSelect = 1u << 1;     // /JOYn output: device selected
constexpr uint16_t kCtrlAck = 1u << 4;        // acknowledge: clears IRQ and error flags
constexpr uint16_t kCtrlReset = 1u << 6;
constexpr uint16_t kCtrlAckIrq = 1u << 12;    // interrupt on /ACK
constexpr uint16_t kCtrlPort2 = 1u << 13;

}  // namespace

void Sio0::transfer(uint8_t tx) {
    // Digital pad (ID 5A41h): 01 42 00 00 00 -> FF 41 5A lo hi; every byte but the last is ACKed.
    const unsigned port = (ctrl_ & kCtrlPort2) ? 1u : 0u;
    const bool pad_present = port == 0;
    uint8_t rx = 0xFF;
    bool ack = false;
    if (index_ == 0) {
        talking_to_pad_ = pad_present && tx == 0x01;
        ack = talking_to_pad_;
    } else if (talking_to_pad_) {
        switch (index_) {
            case 1: rx = 0x41; ack = tx == 0x42; talking_to_pad_ = ack; break;
            case 2: rx = 0x5A; ack = true; break;
            case 3: rx = static_cast<uint8_t>(buttons_[port]); ack = true; break;
            case 4: rx = static_cast<uint8_t>(buttons_[port] >> 8); ack = false; break;
            default: talking_to_pad_ = false; break;
        }
    }
    ++index_;
    rx_ = rx;
    rx_full_ = true;
    ack_ = ack;
    if (ack && (ctrl_ & kCtrlAckIrq)) {
        irq_ = true;
        raise_irq7_();
    }
}

uint32_t Sio0::read(uint32_t phys, unsigned width) {
    switch (phys) {
        case kData: {
            const uint8_t v = rx_full_ ? rx_ : 0xFF;
            rx_full_ = false;
            return width == 1 ? v : (v | 0xFFFFFF00u);  // wider reads see the (empty) FIFO as FFh
        }
        case kStat: {
            uint32_t s = (1u << 0) | (1u << 2);  // TX ready, TX finished (transfers are instant)
            if (rx_full_) s |= 1u << 1;
            if (ack_) s |= 1u << 7;              // /ACK input low
            if (irq_) s |= 1u << 9;
            ack_ = false;                        // the /ACK pulse is short
            return s;
        }
        case kMode: return mode_;
        case kCtrl: return ctrl_;
        case kBaud: return baud_;
        default: return 0;
    }
}

void Sio0::write(uint32_t phys, uint32_t value, unsigned) {
    switch (phys) {
        case kData:
            if (selected_) transfer(static_cast<uint8_t>(value));
            return;
        case kMode: mode_ = static_cast<uint16_t>(value); return;
        case kBaud: baud_ = static_cast<uint16_t>(value); return;
        case kCtrl: {
            const uint16_t v = static_cast<uint16_t>(value);
            if (v & kCtrlReset) {
                *this = Sio0(std::move(raise_irq7_));
                return;
            }
            if (v & kCtrlAck) irq_ = false;
            const bool select = v & kCtrlSelect;
            if (select && !selected_) index_ = 0;  // a new transaction begins
            if (!select) talking_to_pad_ = false;
            selected_ = select;
            ctrl_ = static_cast<uint16_t>(v & ~(kCtrlAck | kCtrlReset));
            return;
        }
        default: return;
    }
}

}  // namespace hle
