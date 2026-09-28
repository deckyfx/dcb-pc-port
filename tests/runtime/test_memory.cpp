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

    // Write watch (DCB_WATCH): the span check sees any store that touches a watched byte, and
    // only those; a bad spec watches nothing. Stores still land either way.
    CHECK(psx_watch_configure("80010100+4") == 1);
    CHECK(psx_watch_lo == 0x10100u && psx_watch_size == 4u);
    CHECK(PSX_WATCHED(0x100FF, 2));   // 16-bit store over the first watched byte
    CHECK(PSX_WATCHED(0x100FD, 4));   // 32-bit store ending on it
    CHECK(PSX_WATCHED(0x10103, 1));   // last watched byte
    CHECK(!PSX_WATCHED(0x10104, 4));  // just after
    CHECK(!PSX_WATCHED(0x100FC, 4));  // just before
    psx_write16(ctx, 0x80010102u, 0xBEEFu);
    CHECK(psx_read16(ctx, 0x80010102u) == 0xBEEFu);
    CHECK(psx_watch_configure("800E0000-800E1800,801DAF40+0x200") == 1);
    CHECK(psx_watch_lo == 0x0E0000u && psx_watch_size == 0x1DAF40u + 0x200u - 0x0E0000u);
    CHECK(psx_watch_configure("nonsense") == 0 && psx_watch_size == 0u);
    CHECK(!PSX_WATCHED(0, 4) && !PSX_WATCHED(0x10100, 1));  // nothing watched: never a hit

    std::puts("runtime.memory: ok");
    return 0;
}
