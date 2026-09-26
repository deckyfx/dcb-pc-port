// SIO0 controller port tests: the digital pad exchange as libpad drives it, with real byte and
// /ACK timing (libpad clears IRQ7 after sending a byte and then waits for the ACK to raise it).
// Protocol per psx-spx "Controllers and Memory Cards".

#include "pad/sio.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

constexpr uint32_t kData = 0x1F801040, kStat = 0x1F801044, kMode = 0x1F801048, kCtrl = 0x1F80104A,
                   kBaud = 0x1F80104E;

struct Port {
    uint64_t now = 0;
    unsigned irqs = 0;
    hle::Sio0 sio{[this] { ++irqs; }, [this] { return now; }};

    void advance(uint64_t cycles) {
        now += cycles;
        sio.tick(now);
    }
    uint16_t stat() { return static_cast<uint16_t>(sio.read(kStat, 2)); }

    /// libpad's port setup: reset, mode, baud, select with ACK interrupts.
    void open(uint16_t ctrl) {
        sio.write(kCtrl, 0x40, 2);
        sio.write(kCtrl, 0, 2);
        sio.write(kMode, 0x0D, 2);
        sio.write(kBaud, 0x88, 2);
        sio.write(kCtrl, ctrl, 2);
    }

    /// One byte the way libpad sends it; returns the reply and whether the pad ACKed.
    uint8_t exchange(uint8_t tx, bool& acked) {
        const unsigned before = irqs;
        sio.write(kData, tx, 1);
        CHECK((stat() & 0x0002) == 0);  // the reply is not there yet
        CHECK((stat() & 0x0080) == 0);  // nor the ACK
        sio.write(kCtrl, sio.read(kCtrl, 2) | 0x10, 2);  // _padClrIntSio0
        advance(8 * 0x88);
        CHECK(stat() & 0x0002);
        const uint8_t rx = static_cast<uint8_t>(sio.read(kData, 1));
        advance(400);
        acked = irqs != before;
        if (acked) CHECK(stat() & 0x0200);
        sio.write(kCtrl, sio.read(kCtrl, 2) | 0x10, 2);
        CHECK((stat() & 0x0200) == 0);
        advance(200);
        return rx;
    }
};

void digital_pad_exchange() {
    Port p;
    p.sio.set_buttons(0, 0xFFF7);  // Start held
    p.open(0x1003);
    bool ack = false;
    CHECK(p.exchange(0x01, ack) == 0xFF && ack);
    CHECK(p.exchange(0x42, ack) == 0x41 && ack);
    CHECK(p.exchange(0x00, ack) == 0x5A && ack);
    CHECK(p.exchange(0x00, ack) == 0xF7 && ack);
    CHECK(p.exchange(0x00, ack) == 0xFF && !ack);  // last byte: no ACK
}

void empty_port_two() {
    Port p;
    p.open(0x3003);
    bool ack = true;
    CHECK(p.exchange(0x01, ack) == 0xFF && !ack);
}

void reset_keeps_buttons() {
    // libpad resets the port (ctrl bit 6) before every poll; the pad state must survive it.
    Port p;
    p.sio.set_buttons(0, 0xBFFF);  // Cross held
    p.open(0x1003);
    bool ack = false;
    p.exchange(0x01, ack);
    p.exchange(0x42, ack);
    p.exchange(0x00, ack);
    p.exchange(0x00, ack);
    CHECK(p.exchange(0x00, ack) == 0xBF);
}

}  // namespace

int main() {
    digital_pad_exchange();
    empty_port_two();
    reset_keeps_buttons();
    std::puts("sio: ok");
    return 0;
}
