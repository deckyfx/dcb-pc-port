#include "system.hpp"

#include "bios/bios.hpp"
#include "hw/mmio.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace hle {

namespace {

constexpr uint32_t kSrIrqEnable = 0x401u;  // IEc (bit 0) + IM2 (bit 10): hardware interrupts on
constexpr int kSp = 29, kFp = 30, kGp = 28, kRa = 31, kS0 = 16;

}  // namespace

System::System(PsxContext& ctx, Mmio& mmio, Bios& bios) : ctx_(ctx), mmio_(mmio), bios_(bios) {
    pacing_ = std::getenv("DCB_FAST") == nullptr;
    // Kernel process/thread control: *(0x108) -> PCB, *PCB -> current TCB. Games that switch
    // tasks from interrupts (like this one) read and rewrite that TCB's saved registers.
    psx_write32(&ctx_, 0x80000108u, 0x80000E00u);
    psx_write32(&ctx_, 0x80000E00u, kKernelTcb);
}

System& System::from(PsxContext& ctx) {
    return *static_cast<System*>(psx::Machine::from(&ctx).poll_handler());
}

System::Task& System::current_task() {
    if (!current_) {  // adopt the thread as the first task
        auto t = std::make_unique<Task>();
        t->cookie = next_cookie_++;
        t->fiber = psx::Fiber::current();
        current_ = t.get();
        tasks_[t->cookie] = std::move(t);
    }
    return *current_;
}

uint32_t System::current_cookie() { return current_task().cookie; }

void System::apply_resume(PsxContext& ctx, uint32_t tcb, bool full) {
    if (!tcb) return;
    if (full) {
        for (uint32_t i = 1; i < 32; ++i) ctx.r[i] = psx_read32(&ctx, tcb + 0x20 + 4 * i);
        ctx.hi = psx_read32(&ctx, tcb + 0xA4);
        ctx.lo = psx_read32(&ctx, tcb + 0xA8);
    } else {
        for (uint32_t i = 16; i <= 23; ++i) ctx.r[i] = psx_read32(&ctx, tcb + 0x20 + 4 * i);  // s0-s7
        ctx.r[28] = psx_read32(&ctx, tcb + 0x90);  // gp
        ctx.r[29] = psx_read32(&ctx, tcb + 0x94);  // sp
        ctx.r[30] = psx_read32(&ctx, tcb + 0x98);  // s8
        ctx.r[2] = psx_read32(&ctx, tcb + 0x28);   // v0
    }
    const uint32_t sr = psx_read32(&ctx, tcb + 0xAC);
    ctx.cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);  // rfe
    ctx.cop0[13] = psx_read32(&ctx, tcb + 0xB0);
}

void System::reap_dead_tasks() {
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->second->dead && it->second.get() != current_) it = tasks_.erase(it);
        else ++it;
    }
}

void System::task_main(void* arg) {
    auto* self = static_cast<System*>(arg);
    PsxContext& ctx = self->ctx_;
    Task& me = *self->current_;
    self->reap_dead_tasks();
    self->apply_resume(ctx, me.resume_tcb, true);
    psx_dispatch(&ctx, me.start_pc);
    // The entry returned: MIPS would jump to $ra, which a task system points at its exit routine.
    psx_dispatch(&ctx, ctx.r[31]);
    std::fprintf(stderr, "[task] exit routine %08X returned into a finished task\n", ctx.r[31]);
    std::abort();
}

void System::switch_context(PsxContext& ctx, uint32_t tcb, bool full) {
    Task& me = current_task();
    const uint32_t epc = psx_read32(&ctx, tcb + 0xA0);
    if (epc == me.cookie) {  // picked ourselves: resume in place
        apply_resume(ctx, tcb, full);
        return;
    }

    Task* target = nullptr;
    if (is_cookie(epc)) {
        const auto it = tasks_.find(epc);
        if (it == tasks_.end() || it->second->dead) {
            std::fprintf(stderr, "[task] resume of unknown/finished task cookie %08X\n", epc);
            std::abort();
        }
        target = it->second.get();
    } else {
        auto t = std::make_unique<Task>();
        t->cookie = next_cookie_++;
        t->start_pc = epc;
        t->owned = psx::Fiber::create(&System::task_main, this);
        t->fiber = t->owned.get();
        target = t.get();
        tasks_[t->cookie] = std::move(t);
        full = true;  // a fresh task starts from its whole register block
    }

    std::memcpy(me.regs, ctx.r, sizeof me.regs);
    me.hi = ctx.hi;
    me.lo = ctx.lo;
    me.sr = ctx.cop0[12];
    target->resume_tcb = tcb;
    target->resume_full = full;
    current_ = target;
    target->fiber->resume();

    // Something resumed us: restore our registers, then apply what the resumer asked for.
    current_ = &me;
    std::memcpy(ctx.r, me.regs, sizeof me.regs);
    ctx.hi = me.hi;
    ctx.lo = me.lo;
    ctx.cop0[12] = me.sr;
    apply_resume(ctx, me.resume_tcb, me.resume_full);
    reap_dead_tasks();
}

void System::pace() {
    // Guest time runs ahead of the wall clock when the host is fast: wait at frame boundaries so
    // the game runs at 59.94 fps. If the host falls behind, never sleep (and do not try to catch up).
    if (!pacing_) return;
    const auto guest = std::chrono::duration<double>(static_cast<double>(ctx_.cycles) / kCpuHz);
    const auto real = std::chrono::steady_clock::now() - start_;
    const auto ahead = std::chrono::duration_cast<std::chrono::microseconds>(guest - real);
    if (ahead.count() > 0) {
        std::this_thread::sleep_for(std::min(ahead, std::chrono::microseconds(100000)));
    } else if (ahead < -std::chrono::milliseconds(250)) {
        start_ = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(guest);
    }
}

void System::poll(PsxContext& ctx) {
    const uint64_t due = ctx.cycles / cycles_per_vblank();
    if (due > vblanks_) {
        vblanks_ = due;  // coalesce: a late poll delivers one VBLANK, like a missed frame
        mmio_.raise_irq(0);
        pace();
    }
    deliver(ctx);
}

void System::idle(PsxContext& ctx) {
    ctx.cycles = (ctx.cycles / cycles_per_vblank() + 1) * cycles_per_vblank();
    poll(ctx);
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

    // Exception entry, as the kernel does it: the interrupted context goes into the current TCB
    // (epc = this task's cookie), with the SR mode bits pushed.
    uint32_t saved[32];
    std::memcpy(saved, ctx.r, sizeof saved);
    const uint32_t hi = ctx.hi, lo = ctx.lo, pc = ctx.pc, saved_sr = sr;
    const uint32_t cookie = current_cookie();
    for (uint32_t i = 0; i < 32; ++i) psx_write32(&ctx, kKernelTcb + 0x08 + 4 * i, ctx.r[i]);
    psx_write32(&ctx, kKernelTcb + 0x88, cookie);
    psx_write32(&ctx, kKernelTcb + 0x8C, hi);
    psx_write32(&ctx, kKernelTcb + 0x90, lo);
    psx_write32(&ctx, kKernelTcb + 0x94, (saved_sr & ~0x3Fu) | ((saved_sr & 0xFu) << 2));
    psx_write32(&ctx, kKernelTcb + 0x98, 0x400u);  // cause: hardware interrupt

    // Kernel duties before the custom exit: VBLANK drives root counter 3, whose event
    // (class F2000003h, spec 2) games use as their per-frame callback.
    if ((mmio_.pending_irqs() & 1u) && vblank_event_sent_ != vblanks_) {
        vblank_event_sent_ = vblanks_;
        in_irq_ = true;
        sr &= ~kSrIrqEnable;
        bios_.deliver_event(ctx, 0xF2000003u, 0x0002u);
        sr = saved_sr;
        in_irq_ = false;
    }
    const uint32_t hook = bios_.interrupt_hook();
    const uint32_t dispatcher = hook ? find_dispatcher(ctx, hook) : 0;
    if (!dispatcher) {
        leave_interrupt(ctx, saved, hi, lo, pc, saved_sr, cookie);
        return;
    }

    // Run the handler on the context libetc stored in its jmp_buf (interrupt stack, gp,
    // callee-saved registers), with interrupts off.
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

    leave_interrupt(ctx, saved, hi, lo, pc, saved_sr, cookie);
}

void System::leave_interrupt(PsxContext& ctx, const uint32_t* saved, uint32_t hi, uint32_t lo, uint32_t pc,
                             uint32_t sr, uint32_t cookie) {
    std::memcpy(ctx.r, saved, 32 * sizeof(uint32_t));
    ctx.hi = hi;
    ctx.lo = lo;
    ctx.pc = pc;
    ctx.cop0[12] = sr;
    // A handler that rewrote the kernel TCB (a task scheduler) resumes a different task.
    if (psx_read32(&ctx, kKernelTcb + 0x88) != cookie) switch_context(ctx, kKernelTcb - 0x18, true);
}

void System::return_from_exception() {
    if (!irq_env_) {
        std::fprintf(stderr, "[irq] ReturnFromException outside an interrupt (ra=%08X)\n", ctx_.r[kRa]);
        std::abort();
    }
    std::longjmp(*irq_env_, 1);
}

}  // namespace hle
