// Address-translation and slow-path tests for the guest memory bus.

#include <psx/runtime.hpp>

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

int main() {
    static psx::Machine machine;
    PsxContext* ctx = &machine.ctx();

    // KUSEG, KSEG0 and KSEG1 alias the same RAM, and RAM mirrors every 2 MiB below 8 MiB.
    psx_write32(ctx, 0x80010000u, 0xDEADBEEFu);
    CHECK(psx_read32(ctx, 0x00010000u) == 0xDEADBEEFu);
    CHECK(psx_read32(ctx, 0xA0010000u) == 0xDEADBEEFu);
    CHECK(psx_read32(ctx, 0x80210000u) == 0xDEADBEEFu);
    CHECK(psx_read16(ctx, 0x80010002u) == 0xDEADu);
    CHECK(psx_read8(ctx, 0x80010000u) == 0xEFu);

    // Scratchpad is separate from RAM and reachable through KSEG0.
    psx_write32(ctx, 0x1F800010u, 0x12345678u);
    CHECK(psx_read32(ctx, 0x9F800010u) == 0x12345678u);
    CHECK(psx_read32(ctx, 0x80000010u) != 0x12345678u);

    CHECK(psx_ram_offset(0x1F801810u) == -1);  // GPU register is not RAM

    std::puts("runtime.memory: ok");
    return 0;
}
