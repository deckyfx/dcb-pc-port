#include "bios/bios.hpp"

#include <cstdio>
#include <cstdlib>

namespace hle {

namespace {

constexpr int kV0 = 2;
constexpr int kRa = 31;

}  // namespace

void Bios::call(PsxContext& ctx, uint32_t table, uint32_t function) {
    // Calls are added as the recompiled game reaches them; the abort shows which one is next.
    // Reference: psx-spx "BIOS Function Summary".
    std::fprintf(stderr, "[bios] unimplemented %02X:%02X (ra=%08X)\n", table, function, ctx.r[kRa]);
    ctx.r[kV0] = 0;
    std::abort();
}

}  // namespace hle
