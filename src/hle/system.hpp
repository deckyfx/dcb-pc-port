#pragma once
// Time and interrupts. A host clock drives the PS1 timers and raises VBLANK at the NTSC rate;
// pending interrupts are delivered to the game's own dispatcher (libetc, registered through
// HookEntryInt) whenever generated code polls (loop back-edges) or reads a hardware register.
//
// The game runs on fibers only; the OS thread is the host. start() creates the game's main fiber,
// resume_guest() runs the game until its next VBLANK, where it switches back to the host with its
// whole stack suspended (see docs/HOST_MAIN_LOOP.md). Host code touches game state only there.

#include <psx/fiber.hpp>
#include <psx/runtime.hpp>
#include <psx/state.hpp>

#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <map>
#include <string>
#include <memory>

namespace hle {

class Bios;
class Mmio;

class System final : public psx::PollHandler {
public:
    static constexpr double kCpuHz = 33868800.0;  ///< R3000A clock
    static constexpr double kHblankHz = 15734.0;  ///< NTSC line rate
    static constexpr double kVblankHz = 59.94;    ///< NTSC field rate

    System(PsxContext& ctx, Mmio& mmio, Bios& bios);

    /// PollHandler: advance time, raise due interrupts, deliver them if the game allows.
    void poll(PsxContext& ctx) override;
    /// Same, from a hardware-register access (the context is the one this system drives).
    void io_poll() { poll(ctx_); }

    uint64_t cpu_cycles() const { return ctx_.cycles; }
    uint64_t hblanks() const { return ctx_.cycles * static_cast<uint64_t>(kHblankHz) / static_cast<uint64_t>(kCpuHz); }

    /// A native wait (no guest code runs): deliver pending interrupts, then advance guest time a little.
    void idle(PsxContext& ctx);

    /// Host time spent sleeping for frame pacing (ns), for the performance overlay.
    uint64_t sleep_ns() const { return sleep_ns_; }

    // ---- Host loop ----------------------------------------------------------------------------

    /// Create the game's main fiber, starting at `entry` when first resumed. Call from the host.
    void start(uint32_t entry);
    /// Run the game until its next VBLANK. Returns false once the game can no longer run (its
    /// entry returned or threw; see error()).
    bool resume_guest();
    /// Why the game stopped, if it stopped with an error (empty otherwise).
    const std::string& error() const { return error_; }
    /// Wait at a frame boundary so the game runs at 59.94 fps (no-op with DCB_FAST=1). Host side.
    void pace();
    /// Line the pacing clock up with the game's clock, after running unthrottled (fast-forward) or
    /// paused, so pace() neither sleeps off the time gained nor rushes to make up a gap.
    void resync_pacing();

    /// Whether a save state can be taken now: the game is suspended at a frame boundary, every
    /// task runs on a fiber of its own, and the fiber backend supports snapshots. Otherwise
    /// `why` says what is missing.
    bool can_save_state(std::string* why = nullptr) const;
    /// Save state: time, interrupt and task bookkeeping, and every task's fiber (stack bytes and
    /// registers). Host side, at a frame boundary only (chunk "SYS ").
    void save_state(psx::StateWriter& w) const;
    /// Replace the tasks with the saved ones: their fibers resume on the same stack addresses.
    /// The whole chunk is read and checked before anything changes. Call reset_pacing() once
    /// the CPU state (guest time) is loaded too (hle::load_guest does).
    void load_state(psx::StateReader& r);
    /// Guest time jumped (a state was loaded): pace from here instead of catching up or sleeping.
    void reset_pacing();
    /// The live tasks' cookies, e.g. "3 tasks: DCB00000 DCB00004* DCB00007" (* = running), for logs.
    std::string describe_tasks() const;

    /// B0:17: leave the interrupt handler and resume the interrupted code.
    [[noreturn]] void return_from_exception();

    // ---- Guest task contexts ----------------------------------------------------------------
    // A guest "task block" uses the kernel TCB layout shifted by 0x18: GPRs at +0x20, epc +0xA0,
    // hi +0xA4, lo +0xA8, SR +0xAC, cause +0xB0. Each guest task runs on its own fiber; where the
    // guest stores a suspended task's resume address (epc), a cookie naming its fiber is stored.

    static constexpr uint32_t kCookieBase = 0xDCB00000u;
    static bool is_cookie(uint32_t epc) { return (epc & 0xFFF00000u) == kCookieBase; }

    /// The system driving this context (overrides reach it through the guest context).
    static System& from(PsxContext& ctx);

    /// Cookie of the task running now (store it as the epc of a task that suspends itself).
    uint32_t current_cookie();

    /// Resume the task described by the block at `tcb`: its epc is a cookie (suspended fiber) or a
    /// code address (fresh task, started on a new fiber with the block's registers). `full` picks
    /// the resume mode: all registers, or the callee-saved set + v0 (a task that yielded).
    /// Returns when something resumes the caller, with its registers restored the same way.
    void switch_context(PsxContext& ctx, uint32_t tcb, bool full);

    /// The running task ends; its fiber is destroyed after the next switch away from it.
    void end_current_task() { current_->dead = true; }

    /// Guest address of the kernel TCB (what *(*0x80000108) points to).
    static constexpr uint32_t kKernelTcb = 0x80000E10u;
    /// Kernel interrupt stack (top), in the kernel RAM area games never touch.
    static constexpr uint32_t kKernelStack = 0x8000EFF0u;

private:
    PsxContext& ctx_;
    Mmio& mmio_;
    Bios& bios_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    uint64_t vblanks_ = 0;
    uint64_t vblank_event_sent_ = 0;
    bool in_irq_ = false;
    std::jmp_buf* irq_env_ = nullptr;
    uint32_t dispatcher_hook_ = 0, dispatcher_ = 0;

    struct Task {
        uint32_t cookie = 0;
        std::unique_ptr<psx::Fiber> owned;  ///< null for the thread's own fiber
        psx::Fiber* fiber = nullptr;
        uint32_t start_pc = 0;              ///< fresh task entry
        uint32_t regs[32] = {};
        uint32_t hi = 0, lo = 0, sr = 0;
        uint32_t resume_tcb = 0;            ///< set by whoever resumes this task
        bool resume_full = false;
        bool dead = false;
    };
    std::map<uint32_t, std::unique_ptr<Task>> tasks_;
    Task* current_ = nullptr;
    uint32_t next_cookie_ = kCookieBase;

    Task& current_task();
    void apply_resume(PsxContext& ctx, uint32_t tcb, bool full);
    void reap_dead_tasks();
    static void task_main(void* arg);

    bool pacing_ = true;  ///< DCB_FAST=1 runs unthrottled
    uint64_t sleep_ns_ = 0;

    psx::Fiber* host_ = nullptr;   ///< the OS thread's fiber: runs the host loop
    psx::Fiber* guest_ = nullptr;  ///< game fiber to resume (the one that last yielded)
    bool guest_done_ = false;
    std::string error_;
    uint32_t main_entry_ = 0;

    uint64_t cycles_per_vblank() const { return static_cast<uint64_t>(kCpuHz / kVblankHz); }
    /// From game code at a VBLANK: suspend this fiber and let the host run a frame.
    void yield_to_host();
    /// The game can no longer run: record why and hand control to the host for good.
    [[noreturn]] void guest_finished(std::string error);
    static void main_fiber(void* arg);
    void deliver(PsxContext& ctx);
    void leave_interrupt(PsxContext& ctx, const uint32_t* saved, uint32_t hi, uint32_t lo, uint32_t pc, uint32_t sr,
                         uint32_t cookie);
    uint32_t find_dispatcher(PsxContext& ctx, uint32_t hook);
};

}  // namespace hle
