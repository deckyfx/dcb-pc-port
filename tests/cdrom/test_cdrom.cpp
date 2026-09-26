// CD-ROM controller tests: ReadN as libcd's CdRead drives it (INT1, acknowledge, then a data
// request and the 12-byte header check against the expected position). Per psx-spx "CDROM
// Controller I/O Ports" and "CDROM - Response/Data Queueing".

#include "cdrom/cdrom.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

constexpr uint32_t kBase = hle::CdRom::kBase;
constexpr uint64_t kSectorPeriod = 33868800 / 150;  // double speed

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

/// Mode 2 Form 1 sectors whose header carries their own MSF, like a real disc.
class FakeDisc final : public hle::Disc {
public:
    bool read(uint32_t lba, uint8_t* out) override {
        if (lba >= kSectors) return false;
        std::memset(out, 0, kRawSector);
        const uint32_t abs = lba + 150;
        out[12] = bcd(abs / 4500);
        out[13] = bcd(abs / 75 % 60);
        out[14] = bcd(abs % 75);
        out[15] = 2;
        out[18] = out[22] = 0x08;  // data submode
        return true;
    }
    uint32_t sector_count() const override { return kSectors; }
    std::string describe() const override { return "fake disc"; }

private:
    static constexpr uint32_t kSectors = 1000;
};

struct Drive {
    uint64_t now = 0;
    unsigned irqs = 0;
    hle::CdRom cd{[this] { ++irqs; }, [this] { return now; }};

    Drive() { cd.insert(std::make_unique<FakeDisc>()); }

    void advance(uint64_t cycles) {
        // The host polls in small steps, as the recompiled code's loop back-edges do.
        for (uint64_t end = now + cycles; now < end;) {
            now = std::min(end, now + 1000);
            cd.tick(now);
        }
    }
    void reg(uint32_t r, uint8_t index, uint8_t v) {
        cd.write(kBase, index);
        cd.write(kBase + r, v);
    }
    uint8_t flags() {
        cd.write(kBase, 1);
        return cd.read(kBase + 3) & 7u;
    }
    void ack() { reg(3, 1, 0x1F); }
    void command(uint8_t cmd, std::initializer_list<uint8_t> params = {}) {
        for (uint8_t p : params) reg(2, 0, p);
        reg(1, 0, cmd);
    }
    /// Wait for the next interrupt (at most `limit` cycles); returns its type.
    uint8_t wait(uint64_t limit = 4 * kSectorPeriod) {
        for (uint64_t end = now + limit; now < end && !flags();) advance(1000);
        return flags();
    }
    /// libcd's CdGetSector(&header, 3) after an INT1: data request, then the 12-byte header
    /// (2340-byte mode). Returns the header's position as an LBA.
    uint32_t header_lba() {
        reg(3, 0, 0x80);
        uint8_t h[12];
        for (uint8_t& b : h) b = cd.read(kBase + 2);
        const auto dec = [](uint8_t v) { return (v >> 4) * 10u + (v & 0xFu); };
        return (dec(h[0]) * 60 + dec(h[1])) * 75 + dec(h[2]) - 150;
    }
};

void start_read(Drive& d, uint32_t lba) {
    const uint32_t abs = lba + 150;
    d.command(0x0E, {0xA0});  // Setmode: double speed, 2340-byte sectors
    CHECK(d.wait() == 3);
    d.ack();
    d.command(0x02, {bcd(abs / 4500), bcd(abs / 75 % 60), bcd(abs % 75)});
    CHECK(d.wait() == 3);
    d.ack();
    d.command(0x06);  // ReadN
    CHECK(d.wait() == 3);
    d.ack();
}

void test_sequential_read() {
    Drive d;
    start_read(d, 100);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(d.wait() == 1);
        d.ack();
        CHECK(d.header_lba() == 100 + i);
    }
}

void test_slow_reader_keeps_acknowledged_sector() {
    // The game takes longer than a sector period to service an INT1 (interrupts masked), so the
    // next sector is already buffered when it acknowledges. The data request that follows the
    // acknowledge must still return the acknowledged sector ("CdRead: sector error" otherwise),
    // and the buffered one follows as the next INT1.
    Drive d;
    start_read(d, 200);
    CHECK(d.wait() == 1);
    d.ack();
    CHECK(d.header_lba() == 200);
    CHECK(d.wait() == 1);
    d.advance(3 * kSectorPeriod);  // slow: sector 202 is read behind the pending INT1
    d.ack();
    d.advance(100);                // libcd's callback runs right after the acknowledge
    CHECK(d.header_lba() == 201);
    CHECK(d.wait() == 1);
    d.ack();
    CHECK(d.header_lba() == 202);
    CHECK(d.wait() == 1);
    d.ack();
    CHECK(d.header_lba() == 203);
}

}  // namespace

int main() {
    test_sequential_read();
    test_slow_reader_keeps_acknowledged_sector();
    std::puts("cdrom: all tests passed");
    return 0;
}
