// Device save states: each device saved mid-operation, loaded into a fresh instance, then both
// driven the same way must behave identically; save(load(save(x))) is byte-identical; and a
// chunk from another version or with a different size is rejected.

#include "bios/bios.hpp"
#include "cdrom/cdrom.hpp"
#include "gpu/gpu.hpp"
#include "hw/mmio.hpp"
#include "mdec/mdec.hpp"
#include "pad/sio.hpp"
#include "spu/spu.hpp"

#include <psx/state.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

template <class T>
std::vector<uint8_t> save(const T& device) {
    psx::StateWriter w;
    device.save_state(w);
    return w.take();
}

template <class T>
void load(T& device, const std::vector<uint8_t>& state) {
    psx::StateReader r(state);
    device.load_state(r);
    CHECK(r.at_end());
}

/// Load `state` into `fresh` and require that saving it again gives the same bytes.
template <class T>
void round_trip(T& fresh, const std::vector<uint8_t>& state) {
    load(fresh, state);
    CHECK(save(fresh) == state);
}

bool rejected(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const psx::StateError&) {
        return true;
    }
    return false;
}

// ---- GPU -----------------------------------------------------------------------------------

void gp0(hle::Gpu& g, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) g.gp0(w);
}

void test_gpu() {
    hle::Gpu a;
    a.gp1(0x03000000);                               // display on
    gp0(a, {0xE1000600, 0xE3000000, 0xE4077E7F, 0xE5000000});  // draw mode, area, offset
    gp0(a, {0x02203040, 0x00100010, 0x00400040});   // fill rectangle
    gp0(a, {0x30FF0000, 0x00000000, 0x0000FF00, 0x00000080, 0x000000FF, 0x00800000});  // shaded triangle
    gp0(a, {0xA0000000, 0x00200100, 0x00020002, 0x7FFF1234});  // CPU->VRAM, 2x2: half sent
    gp0(a, {0x20FFFFFF, 0x00050005});               // flat triangle: 1 of 3 vertices sent

    const std::vector<uint8_t> state = save(a);
    hle::Gpu b;
    round_trip(b, state);
    CHECK(b.receiving_vram() == a.receiving_vram());
    for (hle::Gpu* g : {&a, &b}) gp0(*g, {0x00500005, 0x00300060, 0x55554444});  // finish both
    CHECK(std::memcmp(a.vram(), b.vram(), 1024 * 512 * 2) == 0);
    CHECK(a.gpustat() == b.gpustat());
    CHECK(save(a) == save(b));

    // A chunk from a newer build, and one that lost its last byte, are refused.
    std::vector<uint8_t> newer = state;
    newer[4] = 2;  // version field
    CHECK(rejected([&] { load(b, newer); }));
    std::vector<uint8_t> shorter = state;
    shorter.pop_back();
    uint64_t size = 0;
    std::memcpy(&size, shorter.data() + 8, sizeof size);
    --size;
    std::memcpy(shorter.data() + 8, &size, sizeof size);  // consistent header, one field short
    CHECK(rejected([&] { load(b, shorter); }));
}

// ---- SPU -----------------------------------------------------------------------------------

constexpr uint32_t kSpuCnt = 0x1F801DAA, kKon0 = 0x1F801D88, kTransferAddr = 0x1F801DA6;
constexpr uint32_t kMainVolL = 0x1F801D80, kMainVolR = 0x1F801D82, kReverbVolL = 0x1F801D84;

uint32_t voice(int v, uint32_t off) { return hle::Spu::kBase + static_cast<uint32_t>(v) * 0x10 + off; }

void test_spu() {
    hle::Spu a;
    // A looping ADPCM sample at 0x1000: 4 blocks of changing nibbles, loop start on the first.
    std::array<uint32_t, 16> words{};
    auto* bytes = reinterpret_cast<uint8_t*>(words.data());
    for (int blk = 0; blk < 4; ++blk) {
        uint8_t* b = bytes + 16 * blk;
        b[0] = static_cast<uint8_t>(0x10 * (blk % 3) + 4);  // filter, shift
        b[1] = blk == 0 ? 0x04 : blk == 3 ? 0x03 : 0x00;    // loop start / loop end + repeat
        for (int i = 2; i < 16; ++i) b[i] = static_cast<uint8_t>(0x17 * (i + blk));
    }
    a.write16(kSpuCnt, 0xC000);
    a.write16(kTransferAddr, 0x1000 / 8);
    a.dma_write(words.data(), static_cast<uint32_t>(words.size()));
    a.write16(kMainVolL, 0x3FFF);
    a.write16(kMainVolR, 0xC0A0);  // sweep
    a.write16(kReverbVolL, 0x2000);
    for (int v = 0; v < 3; ++v) {
        a.write16(voice(v, 0x0), 0x3000);
        a.write16(voice(v, 0x2), 0x8050);  // sweep
        a.write16(voice(v, 0x4), static_cast<uint16_t>(0x0800 + 0x300 * v));
        a.write16(voice(v, 0x6), 0x1000 / 8);
        a.write16(voice(v, 0x8), 0x3A8F);
        a.write16(voice(v, 0xA), 0x5FC5);
    }
    a.write16(kKon0, 0x0007);
    std::vector<int16_t> cd(2000);
    for (size_t i = 0; i < cd.size(); ++i) cd[i] = static_cast<int16_t>((i * 37) % 3000 - 1500);
    a.push_cd_audio(cd.data(), cd.size() / 2);
    std::vector<int16_t> out_a(2 * 700), out_b(2 * 700);
    a.mix(out_a.data(), 300);  // voices mid-block, envelopes mid-attack

    const std::vector<uint8_t> state = save(a);
    hle::Spu b;
    round_trip(b, state);
    a.mix(out_a.data(), 700);
    b.mix(out_b.data(), 700);
    CHECK(out_a == out_b);
    CHECK(std::any_of(out_a.begin(), out_a.end(), [](int16_t s) { return s != 0; }));
    CHECK(std::memcmp(a.ram(), b.ram(), hle::Spu::kRamSize) == 0);
    CHECK(a.read16(voice(1, 0xC)) == b.read16(voice(1, 0xC)));  // ENVX
}

// ---- CD-ROM --------------------------------------------------------------------------------

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

class FakeDisc final : public hle::Disc {
public:
    bool read(uint32_t lba, uint8_t* out) override {
        if (lba >= 1000) return false;
        std::memset(out, 0, kRawSector);
        const uint32_t abs = lba + 150;
        out[12] = bcd(abs / 4500);
        out[13] = bcd(abs / 75 % 60);
        out[14] = bcd(abs % 75);
        out[15] = 2;
        out[18] = out[22] = 0x08;
        for (uint32_t i = 24; i < 24 + 2048; ++i) out[i] = static_cast<uint8_t>(lba * 7 + i);
        return true;
    }
    uint32_t sector_count() const override { return 1000; }
    std::string describe() const override { return "fake disc"; }
};

struct Drive {
    uint64_t now = 0;
    unsigned irqs = 0;
    hle::CdRom cd{[this] { ++irqs; }, [this] { return now; }};
    std::string log;  ///< everything the "game" saw

    Drive() { cd.insert(std::make_unique<FakeDisc>()); }
    void reg(uint32_t r, uint8_t index, uint8_t v) {
        cd.write(hle::CdRom::kBase, index);
        cd.write(hle::CdRom::kBase + r, v);
    }
    /// A libcd-like reader: poll every 1000 cycles, on an interrupt read the response, ack,
    /// and for data (INT1) take the first bytes of the sector.
    void run(uint64_t cycles) {
        for (uint64_t end = now + cycles; now < end;) {
            now += 1000;
            cd.tick(now);
            cd.write(hle::CdRom::kBase, 1);
            const uint8_t flags = cd.read(hle::CdRom::kBase + 3) & 7u;
            if (!flags) continue;
            log += "int" + std::to_string(flags) + "@" + std::to_string(now) + ":";
            while (cd.read(hle::CdRom::kBase) & 0x20) log += std::to_string(cd.read(hle::CdRom::kBase + 1)) + ",";
            reg(3, 1, 0x1F);  // ack
            if (flags == 1) {
                reg(3, 0, 0x80);  // BFRD
                std::array<uint32_t, 4> w{};
                cd.dma_read(w.data(), 4);
                for (uint32_t x : w) log += std::to_string(x) + ".";
            }
            log += " ";
        }
    }
};

void test_cdrom() {
    Drive a;
    a.reg(3, 1, 0x1F);
    a.reg(2, 1, 0x1F);  // interrupts on
    a.reg(2, 0, 0x80);  // Setmode: double speed
    a.reg(1, 0, 0x0E);
    a.run(100000);
    for (uint8_t p : {bcd(0), bcd(2), bcd(16)}) a.reg(2, 0, p);
    a.reg(1, 0, 0x02);  // Setloc 00:02:16
    a.run(100000);
    a.reg(1, 0, 0x06);  // ReadN
    a.run(900000);      // a few sectors in, responses queued

    const std::vector<uint8_t> state = save(a.cd);
    Drive b;
    b.now = a.now;
    b.log = a.log;
    round_trip(b.cd, state);
    a.run(2000000);
    b.run(2000000);
    CHECK(a.log == b.log);
    CHECK(a.log.find("int1") != std::string::npos);
}

// ---- MDEC ----------------------------------------------------------------------------------

void test_mdec() {
    hle::Mdec a;
    a.write_command(0x40000001u);
    for (uint32_t i = 0; i < 32; ++i) a.write_command(0x08080808u + i);
    a.write_command(0x60000000u);
    for (uint32_t i = 0; i < 32; ++i) a.write_command(0x5A827D8Au ^ (i * 0x01010101u));
    // Colour macroblock, 15-bit: 6 blocks of DC + one AC + EOB = 18 halfwords = 9 words.
    std::vector<uint16_t> hw;
    for (int blk = 0; blk < 6; ++blk) {
        hw.push_back(static_cast<uint16_t>((2u << 10) | static_cast<uint16_t>(20 * blk + 5)));
        hw.push_back(static_cast<uint16_t>((1u << 10) | 0x3F0u));
        hw.push_back(0xFE00);
    }
    std::vector<uint32_t> words{0x20000000u | (3u << 27) | static_cast<uint32_t>(hw.size() / 2)};
    for (size_t i = 0; i < hw.size(); i += 2) words.push_back(hw[i] | uint32_t{hw[i + 1]} << 16);
    a.dma_write(words.data(), 5);  // half the macroblock

    const std::vector<uint8_t> state = save(a);
    hle::Mdec b;
    round_trip(b, state);
    for (hle::Mdec* m : {&a, &b}) m->dma_write(words.data() + 5, static_cast<uint32_t>(words.size() - 5));
    CHECK(a.output_words_available() == 128 && b.output_words_available() == 128);
    std::vector<uint32_t> out_a(128), out_b(128);
    a.dma_read(out_a.data(), 64);
    b.dma_read(out_b.data(), 64);
    // Mid-drain as well.
    hle::Mdec c;
    round_trip(c, save(a));
    std::vector<uint32_t> out_c = out_a;
    a.dma_read(out_a.data() + 64, 64);
    b.dma_read(out_b.data() + 64, 64);
    c.dma_read(out_c.data() + 64, 64);
    CHECK(out_a == out_b && out_a == out_c);
    CHECK(a.status() == b.status());
}

// ---- SIO0 ----------------------------------------------------------------------------------

struct Port {
    uint64_t now = 0;
    unsigned irqs = 0;
    hle::Sio0 sio{[this] { ++irqs; }, [this] { return now; }};
};

void test_sio() {
    constexpr uint32_t kData = 0x1F801040, kStat = 0x1F801044, kCtrl = 0x1F80104A, kBaud = 0x1F80104E;
    Port a;
    a.sio.set_buttons(0, 0xBFF7);
    a.sio.write(kBaud, 0x88, 2);
    a.sio.write(kCtrl, 0x1003, 2);  // select, ACK interrupts
    a.sio.write(kData, 0x01, 1);
    a.now += 5000;
    a.sio.tick(a.now);
    (void)a.sio.read(kData, 1);
    a.sio.write(kData, 0x42, 1);  // byte in flight

    const std::vector<uint8_t> state = save(a.sio);
    Port b;
    b.now = a.now;
    round_trip(b.sio, state);
    std::string la, lb;
    for (Port* p : {&a, &b}) {
        std::string& l = p == &a ? la : lb;
        for (const uint8_t tx : {uint8_t{0}, uint8_t{0}, uint8_t{0}}) {
            p->now += 5000;
            p->sio.tick(p->now);
            l += std::to_string(p->sio.read(kStat, 2)) + "/" + std::to_string(p->sio.read(kData, 1)) + " ";
            p->sio.write(kCtrl, p->sio.read(kCtrl, 2) | 0x10, 2);
            p->sio.write(kData, tx, 1);
        }
    }
    CHECK(la == lb);
    CHECK(a.irqs == b.irqs + 1 || a.irqs == b.irqs);  // b counts only after the load
    CHECK(a.sio.last_sent() == b.sio.last_sent() && a.sio.last_sent() == 0xBFF7);
}

// ---- Whole MMIO and BIOS -------------------------------------------------------------------

void test_mmio_and_bios() {
    hle::Mmio a;
    a.write(0x1F801074, 0x0D, 4);        // I_MASK
    a.write(0x1F8010F0, 0x0FEDCBA9, 4);  // DPCR
    a.write(0x1F801104, 0x0158, 4);      // timer 0 mode
    a.write(0x1F801060, 0x00000B88, 4);  // RAM size register (plain storage)
    a.write(0x1F801814, 0x03000000, 4);  // GPU display on
    a.write(0x1F801C00, 0x1234, 2);      // SPU voice 0 volume
    a.raise_irq(3);
    const std::vector<uint8_t> state = save(a);
    hle::Mmio b;
    round_trip(b, state);
    CHECK(b.read(0x1F801074, 4) == 0x0D);
    CHECK(b.read(0x1F801060, 4) == 0x0B88);
    CHECK(b.pending_irqs() == a.pending_irqs());

    hle::Bios bios;
    const std::vector<uint8_t> bios_state = save(bios);
    hle::Bios bios2;
    round_trip(bios2, bios_state);
    // Wrong chunk: a GPU state is not a BIOS state.
    hle::Gpu gpu;
    CHECK(rejected([&] { load(bios2, save(gpu)); }));
}

}  // namespace

int main() {
    test_gpu();
    test_spu();
    test_cdrom();
    test_mdec();
    test_sio();
    test_mmio_and_bios();
    std::puts("state.devices: ok");
    return 0;
}
