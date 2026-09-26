#include "system.hpp"

#include "bios/bios.hpp"
#include "hw/mmio.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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
    try {
        self->reap_dead_tasks();
        uint32_t start_pc = 0;
        {
            const Task& me = *self->current_;  // not held while the task runs (see switch_context)
            self->apply_resume(ctx, me.resume_tcb, true);
            start_pc = me.start_pc;
        }
        psx_dispatch(&ctx, start_pc);
        // The entry returned: MIPS would jump to $ra, which a task system points at its exit routine.
        psx_dispatch(&ctx, ctx.r[31]);
    } catch (const std::exception& e) {
        self->guest_finished(e.what());  // exceptions cannot cross fibers: hand them to the host
    } catch (...) {
        self->guest_finished("unknown exception in a game task");
    }
    std::fprintf(stderr, "[task] exit routine %08X returned into a finished task\n", ctx.r[31]);
    std::abort();
}

// ---------------------------------------------------------------------------------------------
// Host loop

void System::main_fiber(void* arg) {
    auto* self = static_cast<System*>(arg);
    try {
        psx_dispatch(&self->ctx_, self->main_entry_);
    } catch (const std::exception& e) {
        self->guest_finished(e.what());
    } catch (...) {
        self->guest_finished("unknown exception in the game");
    }
    self->guest_finished("");  // the boot executable returned
}

void System::start(uint32_t entry) {
    host_ = psx::Fiber::current();
    main_entry_ = entry;
    auto t = std::make_unique<Task>();
    t->cookie = next_cookie_++;
    // Recompiled code nests native frames as deep as the guest calls: give the main path what
    // the OS thread had (and more). The stack is committed lazily.
    t->owned = psx::Fiber::create(&System::main_fiber, this, 16u << 20);
    t->fiber = t->owned.get();
    guest_ = t->fiber;
    current_ = t.get();
    tasks_[t->cookie] = std::move(t);
}

bool System::resume_guest() {
    if (guest_done_ || !guest_) return false;
    guest_->resume();  // returns at the game's next VBLANK (or when it finishes)
    return !guest_done_;
}

void System::yield_to_host() {
    guest_ = psx::Fiber::current();
    host_->resume();
}

void System::guest_finished(std::string error) {
    error_ = std::move(error);
    guest_done_ = true;
    host_->resume();
    std::fprintf(stderr, "[dcb] a finished game fiber was resumed\n");
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
    const uint32_t my_cookie = me.cookie;
    target->fiber->resume();

    // Something resumed us: restore our registers, then apply what the resumer asked for.
    // Only the cookie survives the switch: loading a save state while this fiber was suspended
    // replaces every Task object (this frame's `me` and `target` would dangle).
    const auto self = tasks_.find(my_cookie);
    if (self == tasks_.end()) {
        std::fprintf(stderr, "[task] resumed task %08X is not in the task list\n", my_cookie);
        std::abort();
    }
    Task& back = *self->second;
    current_ = &back;
    std::memcpy(ctx.r, back.regs, sizeof back.regs);
    ctx.hi = back.hi;
    ctx.lo = back.lo;
    ctx.cop0[12] = back.sr;
    apply_resume(ctx, back.resume_tcb, back.resume_full);
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
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min(ahead, std::chrono::microseconds(100000)));
        sleep_ns_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    } else if (ahead < -std::chrono::milliseconds(250)) {
        start_ = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(guest);
    }
}

void System::resync_pacing() {
    const auto guest = std::chrono::duration<double>(static_cast<double>(ctx_.cycles) / kCpuHz);
    start_ = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(guest);
}

void System::poll(PsxContext& ctx) {
    mmio_.tick(ctx.cycles);
    const uint64_t due = ctx.cycles / cycles_per_vblank();
    if (due > vblanks_) {
        vblanks_ = due;  // coalesce: a late poll delivers one VBLANK, like a missed frame
        mmio_.raise_irq(0);
        mmio_.vblank();
        if (host_) yield_to_host();  // the host presents, reads input and paces, then resumes us
    }
    deliver(ctx);
}

void System::idle(PsxContext& ctx) {
    // Deliver what is already pending first; only then let a little guest time pass (~60 us, the
    // scale of a DMA or CD response), so native waits end as soon as their event can arrive.
    poll(ctx);
    ctx.cycles += 2048;
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
    // Kernel interrupt chains (libpad's VBLANK pad reader, ...), on the kernel's own stack.
    in_irq_ = true;
    sr &= ~kSrIrqEnable;
    bios_.run_interrupt_chains(ctx, kKernelStack);
    sr = saved_sr;
    in_irq_ = false;

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

// ---------------------------------------------------------------------------------------------
// Save states. At a frame boundary every game fiber is suspended: the one that reached VBLANK
// inside yield_to_host(), the others inside switch_context(). Their stacks hold recompiled frames
// and HLE frames that own no heap memory (docs/HOST_MAIN_LOOP.md, "Save states"), so the stack
// bytes plus the saved registers are the whole native side of a task.

namespace {
constexpr uint32_t kSysVersion = 1;
constexpr size_t kMaxTasks = 4096;
constexpr size_t kMaxStackImage = 64u << 20;
constexpr size_t kMaxContext = 64u << 10;

void write_image(psx::StateWriter& w, const psx::Fiber::Image& image) {
    w.u64(image.stack_base);
    w.u64(image.stack_bytes);
    w.u64(image.data_base);
    w.vec(image.data);
    w.vec(image.context);
    w.u64(image.entry);
    w.u64(image.arg);
}

psx::Fiber::Image read_image(psx::StateReader& r) {
    psx::Fiber::Image image;
    image.stack_base = r.u64();
    image.stack_bytes = r.u64();
    image.data_base = r.u64();
    r.vec(image.data, kMaxStackImage);
    r.vec(image.context, kMaxContext);
    image.entry = r.u64();
    image.arg = r.u64();
    return image;
}
}  // namespace

bool System::can_save_state(std::string* why) const {
    const auto no = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (!psx::Fiber::snapshots_supported()) return no("save states are not supported on this platform");
    if (!host_ || !guest_ || guest_done_) return no("the game is not running");
    if (psx::Fiber::current() != host_) return no("save states are taken by the host, between frames");
    if (!current_ || current_->fiber != guest_) return no("the suspended fiber is not the current task");
    for (const auto& [cookie, task] : tasks_)
        if (!task->owned) return no("a task runs on the thread's own stack");
    return true;
}

void System::save_state(psx::StateWriter& w) const {
    std::string why;
    if (!can_save_state(&why)) throw psx::StateError("save state: " + why);
    w.begin(psx::state_tag("SYS "), kSysVersion);
    w.u64(vblanks_);
    w.u64(vblank_event_sent_);
    w.boolean(in_irq_);
    w.u64(reinterpret_cast<uintptr_t>(irq_env_));  // points into a saved stack: same address on load
    w.u32(dispatcher_hook_);
    w.u32(dispatcher_);
    w.u32(next_cookie_);
    w.u32(main_entry_);
    w.u32(current_->cookie);
    w.size(tasks_.size());
    for (const auto& [cookie, t] : tasks_) {
        w.u32(t->cookie);
        w.u32(t->start_pc);
        w.pod(t->regs);
        w.u32(t->hi);
        w.u32(t->lo);
        w.u32(t->sr);
        w.u32(t->resume_tcb);
        w.boolean(t->resume_full);
        w.boolean(t->dead);
        write_image(w, t->owned->capture());
    }
    w.end();
}

void System::load_state(psx::StateReader& r) {
    std::string why;
    if (!can_save_state(&why)) throw psx::StateError("load state: " + why);

    // Read and check everything first; nothing changes until the chunk is known to be good.
    struct Saved {
        Task task;
        psx::Fiber::Image image;
    };
    r.begin(psx::state_tag("SYS "), kSysVersion);
    const uint64_t vblanks = r.u64();
    const uint64_t vblank_event_sent = r.u64();
    const bool in_irq = r.boolean();
    const uint64_t irq_env = r.u64();
    const uint32_t dispatcher_hook = r.u32();
    const uint32_t dispatcher = r.u32();
    const uint32_t next_cookie = r.u32();
    const uint32_t main_entry = r.u32();
    const uint32_t current = r.u32();
    const size_t count = r.size(kMaxTasks);
    std::vector<Saved> saved(count);
    bool have_current = false;
    for (Saved& s : saved) {
        s.task.cookie = r.u32();
        s.task.start_pc = r.u32();
        r.pod(s.task.regs);
        s.task.hi = r.u32();
        s.task.lo = r.u32();
        s.task.sr = r.u32();
        s.task.resume_tcb = r.u32();
        s.task.resume_full = r.boolean();
        s.task.dead = r.boolean();
        s.image = read_image(r);
        if (!is_cookie(s.task.cookie)) r.fail("bad task cookie");
        if (s.task.cookie == current) have_current = !s.task.dead;
    }
    if (!have_current) r.fail("the running task is missing");
    for (size_t i = 1; i < saved.size(); ++i)
        if (saved[i].task.cookie <= saved[i - 1].task.cookie) r.fail("task list out of order");
    r.end();

    // Commit. Destroying the current fibers returns their stacks to the free list, from which
    // each saved fiber claims its own stack back by address.
    current_ = nullptr;
    guest_ = nullptr;
    tasks_.clear();
    for (Saved& s : saved) {
        auto t = std::make_unique<Task>();
        t->cookie = s.task.cookie;
        t->start_pc = s.task.start_pc;
        std::memcpy(t->regs, s.task.regs, sizeof t->regs);
        t->hi = s.task.hi;
        t->lo = s.task.lo;
        t->sr = s.task.sr;
        t->resume_tcb = s.task.resume_tcb;
        t->resume_full = s.task.resume_full;
        t->dead = s.task.dead;
        t->owned = psx::Fiber::restore(s.image);
        t->fiber = t->owned.get();
        tasks_[t->cookie] = std::move(t);
    }
    vblanks_ = vblanks;
    vblank_event_sent_ = vblank_event_sent;
    in_irq_ = in_irq;
    irq_env_ = reinterpret_cast<std::jmp_buf*>(static_cast<uintptr_t>(irq_env));
    dispatcher_hook_ = dispatcher_hook;
    dispatcher_ = dispatcher;
    next_cookie_ = next_cookie;
    main_entry_ = main_entry;
    current_ = tasks_.at(current).get();
    guest_ = current_->fiber;
}

std::string System::describe_tasks() const {
    std::string s = std::to_string(tasks_.size()) + (tasks_.size() == 1 ? " task:" : " tasks:");
    for (const auto& [cookie, t] : tasks_) {
        char buf[16];
        std::snprintf(buf, sizeof buf, " %08X%s", cookie, t.get() == current_ ? "*" : "");
        s += buf;
    }
    return s;
}

void System::reset_pacing() {
    const auto guest = std::chrono::duration<double>(static_cast<double>(ctx_.cycles) / kCpuHz);
    start_ = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(guest);
}

}  // namespace hle
