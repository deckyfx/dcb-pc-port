#pragma once
// Time and interrupts. A host clock drives the PS1 timers and raises VBLANK at the NTSC rate;
// pending interrupts are delivered to the game's own dispatcher (libetc, registered through
// HookEntryInt) whenever generated code polls (loop back-edges) or reads a hardware register.

#include <psx/runtime.hpp>

#include <chrono>
#include <csetjmp>
#include <cstdint>

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

    uint64_t cpu_cycles() const;
    uint64_t hblanks() const;

    /// B0:17: leave the interrupt handler and resume the interrupted code.
    [[noreturn]] void return_from_exception();

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

    double seconds() const;
    void deliver(PsxContext& ctx);
    uint32_t find_dispatcher(PsxContext& ctx, uint32_t hook);
};

}  // namespace hle
