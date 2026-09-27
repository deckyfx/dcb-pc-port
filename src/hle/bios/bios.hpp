#pragma once
// Native replacement for the PS1 kernel's A0/B0/C0 function tables.
// No BIOS image is loaded at runtime: each call the game makes is implemented here.
// Reference: psx-spx "BIOS Function Summary". Arguments arrive in $a0-$a3, results go to $v0.
// The game's full BIOS surface (40 functions, found by scanning its call stubs) is listed at the
// top of bios.cpp with its implementation status.

#include "mcrd/card_fs.hpp"
#include "mcrd/memcard.hpp"

#include <psx/runtime.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace hle {

class System;

class Bios final : public psx::BiosHandler {
public:
    Bios();
    void call(PsxContext& ctx, uint32_t table, uint32_t function) override;

    /// Kernel event classes/specs used when hardware sources fire (psx-spx "BIOS Events").
    void deliver_event(PsxContext& ctx, uint32_t ev_class, uint32_t spec);

    /// Kernel interrupt chains (C0:02 SysEnqIntRP): run every registered handler, priority 0 first.
    /// Each block is {next, second, first, 0}: `first` checks whether its interrupt is pending and
    /// returns non-zero if so, then `second` is called with that value.
    void run_interrupt_chains(PsxContext& ctx, uint32_t kernel_sp);

    /// Guest jmp_buf the game registered for interrupt entry (B0:19), or 0.
    uint32_t interrupt_hook() const { return hook_entry_int_; }

    void attach(System* system) { system_ = system; }

    /// Slot 1 is a card image in `save_dir` (created formatted when missing); slot 2 is empty.
    void insert_cards(const std::filesystem::path& save_dir);
    /// Re-read card1.mcd from `save_dir` after the menu restored a backup over
    /// it: swaps in a fresh card object (closing the game's open fds, like a
    /// physical swap). Only call while the game is frozen at a frame boundary.
    void reload_card(const std::filesystem::path& save_dir) { insert_cards(save_dir); }

    /// Save state: heap bookkeeping, events, interrupt chains and hooks, card file system
    /// (chunk "BIOS"). Memory-card images are not included (they are files on disk).
    void save_state(psx::StateWriter& w) const;
    void load_state(psx::StateReader& r);

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
    /// Registered chain entries, most recent first. A fixed array, not a vector: the chains run
    /// guest code that can reach a frame boundary, and the running copy of this list sits on
    /// the game's stack then (save states restore stack bytes; heap memory would dangle).
    static constexpr size_t kMaxIntHandlers = 32;
    using IntHandlers = std::array<IntHandler, kMaxIntHandlers>;
    IntHandlers int_handlers_{};
    size_t int_handler_count_ = 0;
    uint32_t hook_entry_int_ = 0;
    uint32_t clear_pad_ = 1;
    std::array<uint32_t, 4> clear_rcnt_{1, 1, 1, 1};
    bool trace_ = false;
    System* system_ = nullptr;
    std::array<std::unique_ptr<MemoryCard>, 2> cards_;
    CardFs card_fs_{{nullptr, nullptr}};  ///< BIOS file API on bu00:/bu10:

    Event* event(uint32_t handle);
    MemoryCard* card(uint32_t port);  ///< port: 0x00 slot 1, 0x10 slot 2
    void card_result(PsxContext& ctx, uint32_t ev_class, MemoryCard* card);

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
    void a0_bu_init(PsxContext& ctx);
    void a0_card_info(PsxContext& ctx);
    void a0_card_load(PsxContext& ctx);

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
    void b0_init_card(PsxContext& ctx);
    void b0_start_card(PsxContext& ctx);
    void b0_stop_card(PsxContext& ctx);
    void b0_write_card_sector(PsxContext& ctx);
    void b0_open(PsxContext& ctx);
    void b0_lseek(PsxContext& ctx);
    void b0_read(PsxContext& ctx);
    void b0_close(PsxContext& ctx);
    void b0_format(PsxContext& ctx);
    void b0_firstfile(PsxContext& ctx);
    void b0_nextfile(PsxContext& ctx);
    void b0_rename(PsxContext& ctx);
    void b0_erase(PsxContext& ctx);
    void b0_get_last_error(PsxContext& ctx);
    void b0_get_last_file_error(PsxContext& ctx);
    void file_async_event(PsxContext& ctx);
    void b0_read_card_sector(PsxContext& ctx);
    void b0_allow_new_card(PsxContext& ctx);
    void b0_get_card_status(PsxContext& ctx);

    // C0 table
    void c0_sys_enq_int_rp(PsxContext& ctx);
    void c0_sys_deq_int_rp(PsxContext& ctx);
    void c0_change_clear_rcnt(PsxContext& ctx);
};

}  // namespace hle
