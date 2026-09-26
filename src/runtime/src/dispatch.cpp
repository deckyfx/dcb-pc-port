// Indirect control flow: jr/jalr targets, BIOS function tables, syscall/break.

#include <psx/runtime.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace psx {

RecompFunc find_function(uint32_t addr) {
    const auto* begin = recomp_function_table;
    const auto* end = recomp_function_table + recomp_function_count;
    const auto* it = std::lower_bound(begin, end, addr,
                                      [](const RecompFunctionEntry& e, uint32_t a) { return e.addr < a; });
    return (it != end && it->addr == addr) ? it->fn : nullptr;
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
    std::fprintf(stderr, "[dispatch] no recompiled function at %08X (ra=%08X); add it to the function list\n",
                 target, ctx->r[31]);
    std::abort();
}

void psx_syscall(PsxContext* ctx, uint32_t code) {
    // SYSCALL with $a0: 1 = EnterCriticalSection, 2 = ExitCriticalSection, 3 = ChangeThread.
    std::fprintf(stderr, "[syscall] code=%X a0=%X (not yet implemented)\n", code, ctx->r[4]);
}

void psx_break(PsxContext* ctx, uint32_t code) {
    std::fprintf(stderr, "[break] code=%X pc~%08X\n", code, ctx->pc);
    std::abort();
}

}  // extern "C"
