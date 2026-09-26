#pragma once
// Native replacement for the PS1 kernel's A0/B0/C0 function tables.
// No BIOS image is loaded at runtime: each call the game makes is implemented here.
// Reference: psx-spx "BIOS Function Summary". Arguments arrive in $a0-$a3, results go to $v0.
// The game's full BIOS surface (40 functions, found by scanning its call stubs) is listed at the
// top of bios.cpp with its implementation status.

#include <psx/runtime.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace hle {

class System;

class Bios final : public psx::BiosHandler {
public:
    Bios();
    void call(PsxContext& ctx, uint32_t table, uint32_t function) override;

    /// Kernel event classes/specs used when hardware sources fire (psx-spx "BIOS Events").
    void deliver_event(PsxContext& ctx, uint32_t ev_class, uint32_t spec);

    /// Guest jmp_buf the game registered for interrupt entry (B0:19), or 0.
    uint32_t interrupt_hook() const { return hook_entry_int_; }

    void attach(System* system) { system_ = system; }

private:
    using Handler = void (Bios::*)(PsxContext& ctx);

    /// Guest heap managed natively (A0:33-39). Blocks live in guest RAM; bookkeeping lives here.
    struct Heap {
        uint32_t base = 0, size = 0;
        std::map<uint32_t, uint32_t> used;  ///< guest address -> size
        uint32_t alloc(uint32_t bytes);
        void release(uint32_t addr);
    };

    /// Event control block (kernel EvCB).
    struct Event {
        uint32_t ev_class = 0, spec = 0, mode = 0, func = 0;
        uint32_t status = 0;  ///< 0 free, 0x1000 disabled, 0x2000 enabled, 0x4000 ready
    };
    static constexpr uint32_t kEventHandleBase = 0xF1000000u;
    static constexpr size_t kMaxEvents = 16;  ///< SYSTEM.CNF EVENT = 16

    struct IntHandler {
        uint32_t priority = 0, block = 0;  ///< SysEnqIntRP(priority, {next, func2, func1, ...})
    };

    std::map<uint32_t, Handler> handlers_;  ///< key: table << 8 | function
    Heap heap_;
    std::array<Event, kMaxEvents> events_{};
    std::vector<IntHandler> int_handlers_;
    uint32_t hook_entry_int_ = 0;
    uint32_t clear_pad_ = 1;
    std::array<uint32_t, 4> clear_rcnt_{1, 1, 1, 1};
    bool trace_ = false;
    System* system_ = nullptr;

    Event* event(uint32_t handle);

    // A0 table
    void a0_malloc(PsxContext& ctx);
    void a0_free(PsxContext& ctx);
    void a0_calloc(PsxContext& ctx);
    void a0_realloc(PsxContext& ctx);
    void a0_init_heap(PsxContext& ctx);
    void a0_flush_cache(PsxContext& ctx);
    void a0_96_init(PsxContext& ctx);
    void a0_cd_remove(PsxContext& ctx);
    void a0_gpu_cw(PsxContext& ctx);

    // B0 table
    void b0_deliver_event(PsxContext& ctx);
    void b0_open_event(PsxContext& ctx);
    void b0_close_event(PsxContext& ctx);
    void b0_wait_event(PsxContext& ctx);
    void b0_test_event(PsxContext& ctx);
    void b0_enable_event(PsxContext& ctx);
    void b0_disable_event(PsxContext& ctx);
    void b0_set_default_exit_from_exception(PsxContext& ctx);
    void b0_hook_entry_int(PsxContext& ctx);
    void b0_return_from_exception(PsxContext& ctx);
    void b0_change_clear_pad(PsxContext& ctx);
    void b0_write(PsxContext& ctx);
    void b0_get_c0_table(PsxContext& ctx);
    void b0_get_b0_table(PsxContext& ctx);

    // C0 table
    void c0_sys_enq_int_rp(PsxContext& ctx);
    void c0_sys_deq_int_rp(PsxContext& ctx);
    void c0_change_clear_rcnt(PsxContext& ctx);
};

}  // namespace hle
