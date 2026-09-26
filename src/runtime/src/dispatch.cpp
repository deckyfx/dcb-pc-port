// Indirect control flow: jr/jalr targets, calls into the overlay window, BIOS function tables,
// syscall/break, and the traps generated code raises.

#include <psx/runtime.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace psx {

RecompFunc find_function(uint32_t addr) {
    const auto* begin = recomp_function_table;
    const auto* end = recomp_function_table + recomp_function_count;
    const auto* it = std::lower_bound(begin, end, addr,
                                      [](const RecompFunctionEntry& e, uint32_t a) { return e.addr < a; });
    return (it != end && it->addr == addr) ? it->fn : nullptr;
}

/// Find the overlay function at `addr` whose code matches what is in RAM right now.
static RecompFunc find_overlay_function(PsxContext* ctx, uint32_t addr) {
    const int32_t off = psx_ram_offset(addr);
    if (off < 0 || static_cast<uint32_t>(off) + 16 > PSX_RAM_SIZE) return nullptr;
    for (uint32_t i = 0; i < recomp_overlay_count; ++i) {
        const RecompOverlay& ov = recomp_overlays[i];
        if (addr < ov.base || addr >= ov.base + ov.size) continue;
        const auto* begin = ov.entries;
        const auto* end = ov.entries + ov.count;
        const auto* it = std::lower_bound(begin, end, addr,
                                          [](const RecompOverlayEntry& e, uint32_t a) { return e.addr < a; });
        if (it != end && it->addr == addr && std::memcmp(ctx->ram + off, it->check, sizeof it->check) == 0)
            return it->fn;
    }
    return nullptr;
}

uint32_t call_guest(PsxContext& ctx, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t saved[32];
    std::memcpy(saved, ctx.r, sizeof saved);
    const uint32_t hi = ctx.hi, lo = ctx.lo;
    ctx.r[4] = a0;
    ctx.r[5] = a1;
    ctx.r[6] = a2;
    ctx.r[7] = a3;
    ctx.r[29] = (ctx.r[29] - 0x100u) & ~7u;  // fresh frame below the interrupted code's stack
    ctx.r[31] = 0;                           // guest `jr ra` returns to us through the C stack
    psx_dispatch(&ctx, addr);
    const uint32_t result = ctx.r[2];
    std::memcpy(ctx.r, saved, sizeof saved);
    ctx.hi = hi;
    ctx.lo = lo;
    return result;
}

}  // namespace psx

extern "C" {

void psx_dispatch(PsxContext* ctx, uint32_t target) {
    // Kernel calls: `jal 0xA0/0xB0/0xC0` with the function number in $t1 (r9).
    const uint32_t kernel = target & 0x1FFFFFFFu;
    if (kernel == 0xA0 || kernel == 0xB0 || kernel == 0xC0) {
        auto& machine = psx::Machine::from(ctx);
        if (auto* bios = machine.bios()) {
            bios->call(*ctx, kernel, ctx->r[9]);
            return;
        }
        std::fprintf(stderr, "[dispatch] BIOS %02X:%02X called with no HLE handler\n", kernel, ctx->r[9]);
        std::abort();
    }

    // Guest code addresses are canonicalised to KSEG0.
    const uint32_t canonical = (target & 0x1FFFFFFFu) | 0x80000000u;
    if (RecompFunc fn = psx::find_function(canonical)) {
        fn(ctx);
        return;
    }
    if (RecompFunc fn = psx::find_overlay_function(ctx, canonical)) {
        fn(ctx);
        return;
    }
    std::fprintf(stderr, "[dispatch] no recompiled function at %08X (ra=%08X); add it as a seed\n", target,
                 ctx->r[31]);
    std::abort();
}

void psx_syscall(PsxContext* ctx, uint32_t code) {
    // SYSCALL with $a0: 1 = EnterCriticalSection, 2 = ExitCriticalSection (psx-spx "SYSCALL
    // Functions"). Critical sections are modelled with the COP0 SR interrupt-enable bits
    // (IEc bit 0 + IM2 bit 10), which the native interrupt source honours.
    constexpr uint32_t kIrqEnable = 0x401u;
    uint32_t& sr = ctx->cop0[12];
    switch (ctx->r[4]) {
        case 0:  // NoFunction
            return;
        case 1:  // EnterCriticalSection: returns 1 if interrupts were enabled
            ctx->r[2] = (sr & kIrqEnable) == kIrqEnable ? 1u : 0u;
            sr &= ~kIrqEnable;
            return;
        case 2:  // ExitCriticalSection
            sr |= kIrqEnable;
            return;
        default:
            std::fprintf(stderr, "[syscall] unimplemented a0=%X (code %X) at %08X\n", ctx->r[4], code, ctx->pc);
            std::abort();
    }
}

void psx_break(PsxContext* ctx, uint32_t code) {
    std::fprintf(stderr, "[break] code=%X at %08X\n", code, ctx->pc);
    std::abort();
}

void psx_poll(PsxContext* ctx) {
    ctx->poll_budget = 256;
    if (auto* handler = psx::Machine::from(ctx).poll_handler()) handler->poll(*ctx);
}

void psx_invalid(PsxContext* ctx, uint32_t pc) {
    std::fprintf(stderr, "[trap] reached undecodable code at %08X (ra=%08X)\n", pc, ctx->r[31]);
    std::abort();
}

void psx_bad_return(PsxContext* ctx, uint32_t expected_ra) {
    std::fprintf(stderr, "[trap] jr ra to %08X, but the caller expects a return to %08X (longjmp/thread switch?)\n",
                 ctx->r[31], expected_ra);
    std::abort();
}

}  // extern "C"
