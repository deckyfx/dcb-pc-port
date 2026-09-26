#include "system.hpp"

#include "bios/bios.hpp"
#include "hw/mmio.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hle {

namespace {

constexpr uint32_t kSrIrqEnable = 0x401u;  // IEc (bit 0) + IM2 (bit 10): hardware interrupts on
constexpr int kSp = 29, kFp = 30, kGp = 28, kRa = 31, kS0 = 16;

}  // namespace

System::System(PsxContext& ctx, Mmio& mmio, Bios& bios) : ctx_(ctx), mmio_(mmio), bios_(bios) {}

double System::seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

uint64_t System::cpu_cycles() const { return static_cast<uint64_t>(seconds() * kCpuHz); }

uint64_t System::hblanks() const { return static_cast<uint64_t>(seconds() * kHblankHz); }

void System::poll(PsxContext& ctx) {
    const auto due = static_cast<uint64_t>(seconds() * kVblankHz);
    if (due > vblanks_) {
        vblanks_ = due;  // coalesce: a late poll delivers one VBLANK, like a missed frame
        mmio_.raise_irq(0);
    }
    deliver(ctx);
}

uint32_t System::find_dispatcher(PsxContext& ctx, uint32_t hook) {
    if (hook == dispatcher_hook_) return dispatcher_;
    // libetc: `if (setjmp(buf)) dispatcher();` - buf[0] is the return address of setjmp, and the
    // first jal after it calls the dispatcher. Resolving it here keeps this independent of the
    // library version and link address.
    const uint32_t resume = psx_read32(&ctx, hook);
    dispatcher_hook_ = hook;
    dispatcher_ = 0;
    for (uint32_t pc = resume; pc < resume + 32; pc += 4) {
        const uint32_t w = psx_read32(&ctx, pc);
        if (w >> 26 == 3) {
            dispatcher_ = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            break;
        }
    }
    std::fprintf(stderr, "[irq] interrupt dispatcher %08X (hook %08X)\n", dispatcher_, hook);
    return dispatcher_;
}

void System::deliver(PsxContext& ctx) {
    if (in_irq_ || !mmio_.pending_irqs()) return;
    uint32_t& sr = ctx.cop0[12];
    if ((sr & kSrIrqEnable) != kSrIrqEnable) return;  // critical section

    // Kernel duties before the custom exit: VBLANK drives root counter 3, whose event
    // (class F2000003h, spec 2) games use as their per-frame callback.
    if ((mmio_.pending_irqs() & 1u) && vblank_event_sent_ != vblanks_) {
        vblank_event_sent_ = vblanks_;
        in_irq_ = true;
        bios_.deliver_event(ctx, 0xF2000003u, 0x0002u);
        in_irq_ = false;
    }
    const uint32_t hook = bios_.interrupt_hook();
    if (!hook) return;
    const uint32_t dispatcher = find_dispatcher(ctx, hook);
    if (!dispatcher) return;

    // Exception entry: save everything, run the handler on the context libetc stored in its
    // jmp_buf (interrupt stack, gp, callee-saved registers), with interrupts off.
    uint32_t saved[32];
    std::memcpy(saved, ctx.r, sizeof saved);
    const uint32_t hi = ctx.hi, lo = ctx.lo, pc = ctx.pc, saved_sr = sr;
    ctx.r[kSp] = psx_read32(&ctx, hook + 0x04);
    ctx.r[kFp] = psx_read32(&ctx, hook + 0x08);
    for (int i = 0; i < 8; ++i) ctx.r[kS0 + i] = psx_read32(&ctx, hook + 0x0C + 4u * static_cast<uint32_t>(i));
    ctx.r[kGp] = psx_read32(&ctx, hook + 0x2C);
    ctx.r[kRa] = 0;
    sr &= ~kSrIrqEnable;

    std::jmp_buf env;
    in_irq_ = true;
    irq_env_ = &env;
    if (setjmp(env) == 0) {
        psx_dispatch(&ctx, dispatcher);  // normally leaves through ReturnFromException
    }
    irq_env_ = nullptr;
    in_irq_ = false;

    std::memcpy(ctx.r, saved, sizeof saved);
    ctx.hi = hi;
    ctx.lo = lo;
    ctx.pc = pc;
    sr = saved_sr;
}

void System::return_from_exception() {
    if (!irq_env_) {
        std::fprintf(stderr, "[irq] ReturnFromException outside an interrupt (ra=%08X)\n", ctx_.r[kRa]);
        std::abort();
    }
    std::longjmp(*irq_env_, 1);
}

}  // namespace hle
