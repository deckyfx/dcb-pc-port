// Slow-path guest memory: everything that is not main RAM.

#include <psx/runtime.hpp>

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kMmioBase = 0x1F801000u;
constexpr uint32_t kMmioEnd = 0x1F803000u;
constexpr uint32_t kCacheControl = 0xFFFE0130u;

enum class Region { Scratch, Mmio, CacheControl, Unmapped };

Region classify(uint32_t addr, uint32_t& phys) {
    if (addr == kCacheControl) return Region::CacheControl;
    phys = addr & 0x1FFFFFFFu;
    // The scratchpad is only visible through KUSEG and KSEG0, not uncached KSEG1.
    if (phys - PSX_SCRATCH_BASE < PSX_SCRATCH_SIZE && addr < 0xA0000000u) return Region::Scratch;
    if (phys >= kMmioBase && phys < kMmioEnd) return Region::Mmio;
    return Region::Unmapped;
}

uint32_t read(PsxContext* ctx, uint32_t addr, unsigned width) {
    uint32_t phys = 0;
    switch (classify(addr, phys)) {
        case Region::Scratch: {
            uint32_t v = 0;
            std::memcpy(&v, ctx->scratch + (phys & (PSX_SCRATCH_SIZE - 1)), width);
            return v;
        }
        case Region::Mmio:
            if (auto* mmio = psx::Machine::from(ctx).mmio()) return mmio->read(phys, width);
            break;
        case Region::CacheControl:
            return 0;
        case Region::Unmapped:
            break;
    }
    std::fprintf(stderr, "[mem] unhandled read%u  %08X\n", width * 8, addr);
    return 0;
}

void write(PsxContext* ctx, uint32_t addr, uint32_t value, unsigned width) {
    uint32_t phys = 0;
    switch (classify(addr, phys)) {
        case Region::Scratch:
            std::memcpy(ctx->scratch + (phys & (PSX_SCRATCH_SIZE - 1)), &value, width);
            return;
        case Region::Mmio:
            if (auto* mmio = psx::Machine::from(ctx).mmio()) return mmio->write(phys, value, width);
            break;
        case Region::CacheControl:
            return;
        case Region::Unmapped:
            break;
    }
    std::fprintf(stderr, "[mem] unhandled write%u %08X = %08X\n", width * 8, addr, value);
}

}  // namespace

extern "C" {

uint8_t psx_slow_read8(PsxContext* ctx, uint32_t addr) { return static_cast<uint8_t>(read(ctx, addr, 1)); }
uint16_t psx_slow_read16(PsxContext* ctx, uint32_t addr) { return static_cast<uint16_t>(read(ctx, addr, 2)); }
uint32_t psx_slow_read32(PsxContext* ctx, uint32_t addr) { return read(ctx, addr, 4); }
void psx_slow_write8(PsxContext* ctx, uint32_t addr, uint8_t v) { write(ctx, addr, v, 1); }
void psx_slow_write16(PsxContext* ctx, uint32_t addr, uint16_t v) { write(ctx, addr, v, 2); }
void psx_slow_write32(PsxContext* ctx, uint32_t addr, uint32_t v) { write(ctx, addr, v, 4); }

}  // extern "C"
