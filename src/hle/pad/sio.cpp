#include "pad/sio.hpp"

#include <cstdio>
#include <cstdlib>

namespace hle {

namespace {

constexpr uint32_t kData = 0x1F801040, kStat = 0x1F801044, kMode = 0x1F801048, kCtrl = 0x1F80104A,
                   kBaud = 0x1F80104E;

constexpr uint16_t kCtrlSelect = 1u << 1;     // /JOYn output: device selected
constexpr uint16_t kCtrlAck = 1u << 4;        // acknowledge: clears IRQ and error flags
constexpr uint16_t kCtrlReset = 1u << 6;
constexpr uint16_t kCtrlAckIrq = 1u << 12;    // interrupt on /ACK
constexpr uint16_t kCtrlPort2 = 1u << 13;

// Pad /ACK: asserted this long after the byte ends, held low for kAckLength (psx-spx timings).
constexpr uint64_t kAckDelay = 338, kAckLength = 100;

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
            case 4:
                rx = static_cast<uint8_t>(buttons_[port] >> 8);
                ack = false;
                if (port == 0) last_sent_ = buttons_[port];
                break;
            default: talking_to_pad_ = false; break;
        }
    }
    ++index_;
    // 8 bits at the baud reload (mode factor 1, as libpad programs it); the reply arrives when the
    // byte has been clocked out, the ACK a little later.
    const uint64_t now = clock_();
    const uint64_t bits = 8ull * (baud_ ? baud_ : 0x88);
    busy_ = true;
    rx_pending_ = true;
    rx_next_ = rx;
    done_at_ = now + bits;
    ack_pending_ = ack;
    ack_at_ = ack ? done_at_ + kAckDelay : 0;   // no ACK: the line stays high
    ack_end_ = ack ? ack_at_ + kAckLength : 0;
    tick(now);
}

void Sio0::tick(uint64_t cycles) {
    if (rx_pending_ && cycles >= done_at_) {
        rx_pending_ = false;
        busy_ = false;
        rx_ = rx_next_;
        rx_full_ = true;
    }
    if (ack_pending_ && cycles >= ack_at_) {
        ack_pending_ = false;
        if (ctrl_ & kCtrlAckIrq) {
            irq_ = true;
            raise_irq7_();
        }
    }
}

namespace {
unsigned trace_budget() {  // DCB_TRACE_PAD=N: log the first N controller-port accesses
    static const unsigned n = std::getenv("DCB_TRACE_PAD") ? static_cast<unsigned>(std::atoi(std::getenv("DCB_TRACE_PAD"))) : 0;
    return n;
}
unsigned traced = 0;
}  // namespace

uint32_t Sio0::read(uint32_t phys, unsigned width) {
    const uint32_t v = read_reg(phys, width);
    if (traced < trace_budget()) {
        ++traced;
        std::fprintf(stderr, "[pad] rd %08X/%u -> %04X\n", phys, width, v & 0xFFFFu);
    }
    return v;
}

uint32_t Sio0::read_reg(uint32_t phys, unsigned width) {
    switch (phys) {
        case kData: {
            tick(clock_());
            const uint8_t v = rx_full_ ? rx_ : 0xFF;
            rx_full_ = false;
            return width == 1 ? v : (v | 0xFFFFFF00u);  // wider reads see the (empty) FIFO as FFh
        }
        case kStat: {
            const uint64_t now = clock_();
            tick(now);
            uint32_t s = 1u << 0;                // TX ready: the one-byte TX buffer is free
            if (!busy_) s |= 1u << 2;            // TX finished
            if (rx_full_) s |= 1u << 1;
            if (now >= ack_at_ && now < ack_end_) s |= 1u << 7;  // /ACK input low
            if (irq_) s |= 1u << 9;
            return s;
        }
        case kMode: return mode_;
        case kCtrl: return ctrl_;
        case kBaud: return baud_;
        default: return 0;
    }
}

void Sio0::write(uint32_t phys, uint32_t value, unsigned width) {
    if (traced < trace_budget()) {
        ++traced;
        std::fprintf(stderr, "[pad] wr %08X/%u <- %04X\n", phys, width, value & 0xFFFFu);
    }
    switch (phys) {
        case kData:
            if (selected_) transfer(static_cast<uint8_t>(value));
            return;
        case kMode: mode_ = static_cast<uint16_t>(value); return;
        case kBaud: baud_ = static_cast<uint16_t>(value); return;
        case kCtrl: {
            const uint16_t v = static_cast<uint16_t>(value);
            if (v & kCtrlReset) {
                // Resets the serial port only: the pads' button state is host input, not port state.
                const std::array<uint16_t, 2> buttons = buttons_;
                const uint16_t sent = last_sent_;
                *this = Sio0(std::move(raise_irq7_), std::move(clock_));
                buttons_ = buttons;
                last_sent_ = sent;
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

void Sio0::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("SIO0"), 1);
    w.pod(buttons_);
    w.u16(ctrl_);
    w.u16(mode_);
    w.u16(baud_);
    w.u8(rx_);
    w.boolean(rx_full_);
    w.boolean(irq_);
    w.boolean(busy_);
    w.boolean(ack_pending_);
    w.boolean(rx_pending_);
    w.u8(rx_next_);
    w.u64(done_at_);
    w.u64(ack_at_);
    w.u64(ack_end_);
    w.u16(last_sent_);
    w.u32(index_);
    w.boolean(selected_);
    w.boolean(talking_to_pad_);
    w.end();
}

void Sio0::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("SIO0"), 1);
    r.pod(buttons_);
    ctrl_ = r.u16();
    mode_ = r.u16();
    baud_ = r.u16();
    rx_ = r.u8();
    rx_full_ = r.boolean();
    irq_ = r.boolean();
    busy_ = r.boolean();
    ack_pending_ = r.boolean();
    rx_pending_ = r.boolean();
    rx_next_ = r.u8();
    done_at_ = r.u64();
    ack_at_ = r.u64();
    ack_end_ = r.u64();
    last_sent_ = r.u16();
    index_ = r.u32();
    selected_ = r.boolean();
    talking_to_pad_ = r.boolean();
    r.end();
}

}  // namespace hle
