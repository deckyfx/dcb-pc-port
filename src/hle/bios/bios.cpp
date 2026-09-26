// BIOS functions used by SLPS-03101 (every A0/B0/C0 call stub in the boot EXE; overlays have none),
// plus calls built without a stub (libgte _patch_gte jumps to 0xB0 directly: B0:56).
//
//   done   A0:39 InitHeap  A0:33 malloc  A0:34 free  A0:37 calloc  A0:38 realloc
//   done   A0:44 FlushCache  A0:71 _96_init  A0:72 CdRemove           (no kernel state to touch)
//   done   B0:07 DeliverEvent  B0:08 OpenEvent  B0:09 CloseEvent  B0:0A WaitEvent
//          B0:0B TestEvent  B0:0C EnableEvent  B0:0D DisableEvent
//   done   B0:18 SetDefaultExitFromException  B0:19 HookEntryInt  B0:5B ChangeClearPad
//   done   C0:02 SysEnqIntRP  C0:03 SysDeqIntRP  C0:0A ChangeClearRCnt
//   done   B0:17 ReturnFromException (-> System, leaves the native interrupt delivery)
//   todo   B0:12 InitPAD  B0:13 StartPAD  B0:15 OutdatedPadInitAndStart
//   part   B0:35 write (TTY fds 0/1 -> host stdout)
//   done   B0:32 open  B0:33 lseek  B0:34 read  B0:36 close  B0:41 format  B0:42 firstfile
//          B0:43 nextfile  B0:44 rename  B0:45 erase  B0:54/55 GetLastError   (hle::CardFs on bu00:/bu10:)
//   done   A0:70 _bu_init  A0:AB _card_info  A0:AC _card_load  B0:4A InitCard  B0:4B StartCard
//          B0:4C StopCard  B0:4E write_card_sector  B0:4F read_card_sector  B0:50 allow_new_card
//          B0:5C get_card_status                   (raw .mcd images in saves/<serial>/)
//   done   A0:49 GPU_cw (-> GP0 port)
//   done   B0:56 GetC0Table  B0:57 GetB0Table (kernel patchers find nothing to patch)
//   todo   A0:51 LoadExec (switch to PSX2.EXE)

#include "bios/bios.hpp"

#include "system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hle {

namespace {

constexpr int kV0 = 2, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kRa = 31;
constexpr uint32_t key(uint32_t table, uint32_t function) { return table << 8 | function; }

constexpr uint32_t kEvDisabled = 0x1000, kEvEnabled = 0x2000, kEvReady = 0x4000;
constexpr uint32_t kEvModeCallback = 0x1000;  // EvMdINTR: run func on delivery, stay enabled

}  // namespace

Bios::Bios() {
    trace_ = std::getenv("DCB_TRACE_BIOS") != nullptr;
    handlers_ = {
        {key(0xA0, 0x33), &Bios::a0_malloc},
        {key(0xA0, 0x34), &Bios::a0_free},
        {key(0xA0, 0x37), &Bios::a0_calloc},
        {key(0xA0, 0x38), &Bios::a0_realloc},
        {key(0xA0, 0x39), &Bios::a0_init_heap},
        {key(0xA0, 0x44), &Bios::a0_flush_cache},
        {key(0xA0, 0x71), &Bios::a0_96_init},
        {key(0xA0, 0x72), &Bios::a0_cd_remove},
        {key(0xA0, 0x49), &Bios::a0_gpu_cw},
        {key(0xA0, 0x70), &Bios::a0_bu_init},
        {key(0xA0, 0xAB), &Bios::a0_card_info},
        {key(0xA0, 0xAC), &Bios::a0_card_load},
        {key(0xB0, 0x4A), &Bios::b0_init_card},
        {key(0xB0, 0x4B), &Bios::b0_start_card},
        {key(0xB0, 0x4C), &Bios::b0_stop_card},
        {key(0xB0, 0x4E), &Bios::b0_write_card_sector},
        {key(0xB0, 0x4F), &Bios::b0_read_card_sector},
        {key(0xB0, 0x50), &Bios::b0_allow_new_card},
        {key(0xB0, 0x5C), &Bios::b0_get_card_status},
        {key(0xB0, 0x07), &Bios::b0_deliver_event},
        {key(0xB0, 0x08), &Bios::b0_open_event},
        {key(0xB0, 0x09), &Bios::b0_close_event},
        {key(0xB0, 0x0A), &Bios::b0_wait_event},
        {key(0xB0, 0x0B), &Bios::b0_test_event},
        {key(0xB0, 0x0C), &Bios::b0_enable_event},
        {key(0xB0, 0x0D), &Bios::b0_disable_event},
        {key(0xB0, 0x18), &Bios::b0_set_default_exit_from_exception},
        {key(0xB0, 0x19), &Bios::b0_hook_entry_int},
        {key(0xB0, 0x17), &Bios::b0_return_from_exception},
        {key(0xB0, 0x5B), &Bios::b0_change_clear_pad},
        {key(0xB0, 0x35), &Bios::b0_write},
        {key(0xB0, 0x32), &Bios::b0_open},
        {key(0xB0, 0x33), &Bios::b0_lseek},
        {key(0xB0, 0x34), &Bios::b0_read},
        {key(0xB0, 0x36), &Bios::b0_close},
        {key(0xB0, 0x41), &Bios::b0_format},
        {key(0xB0, 0x42), &Bios::b0_firstfile},
        {key(0xB0, 0x43), &Bios::b0_nextfile},
        {key(0xB0, 0x44), &Bios::b0_rename},
        {key(0xB0, 0x45), &Bios::b0_erase},
        {key(0xB0, 0x54), &Bios::b0_get_last_error},
        {key(0xB0, 0x55), &Bios::b0_get_last_file_error},
        {key(0xB0, 0x56), &Bios::b0_get_c0_table},
        {key(0xB0, 0x57), &Bios::b0_get_b0_table},
        {key(0xC0, 0x02), &Bios::c0_sys_enq_int_rp},
        {key(0xC0, 0x03), &Bios::c0_sys_deq_int_rp},
        {key(0xC0, 0x0A), &Bios::c0_change_clear_rcnt},
    };
}

void Bios::call(PsxContext& ctx, uint32_t table, uint32_t function) {
    const auto it = handlers_.find(key(table, function));
    if (trace_ && key(table, function) != key(0xB0, 0x17)) {  // ReturnFromException: 60+/s
        std::fprintf(stderr, "[bios] %02X:%02X(%08X, %08X, %08X, %08X) ra=%08X\n", table, function, ctx.r[kA0],
                     ctx.r[kA1], ctx.r[kA2], ctx.r[kA3], ctx.r[kRa]);
    }
    if (it == handlers_.end()) {
        std::fprintf(stderr, "[bios] unimplemented %02X:%02X (ra=%08X)\n", table, function, ctx.r[kRa]);
        std::abort();
    }
    (this->*(it->second))(ctx);
}

// ---------------------------------------------------------------------------------------------
// Heap. First-fit over [base, base+size); 4-byte aligned like the kernel's allocator.

uint32_t Bios::Heap::alloc(uint32_t bytes) {
    bytes = (bytes + 3u) & ~3u;
    if (bytes == 0 || size == 0) return 0;
    uint32_t cursor = base;
    for (const auto& [addr, len] : used) {
        if (addr - cursor >= bytes) break;
        cursor = addr + len;
    }
    if (cursor + bytes > base + size) return 0;
    used.emplace(cursor, bytes);
    return cursor;
}

void Bios::Heap::release(uint32_t addr) { used.erase(addr); }

void Bios::a0_init_heap(PsxContext& ctx) { heap_ = Heap{ctx.r[kA0], ctx.r[kA1], {}}; }

void Bios::a0_malloc(PsxContext& ctx) { ctx.r[kV0] = heap_.alloc(ctx.r[kA0]); }

void Bios::a0_free(PsxContext& ctx) { heap_.release(ctx.r[kA0]); }

void Bios::a0_calloc(PsxContext& ctx) {
    const uint64_t bytes = static_cast<uint64_t>(ctx.r[kA0]) * ctx.r[kA1];
    const uint32_t p = bytes > 0xFFFFFFFFu ? 0 : heap_.alloc(static_cast<uint32_t>(bytes));
    if (p) {
        for (uint32_t i = 0; i < bytes; ++i) psx_write8(&ctx, p + i, 0);
    }
    ctx.r[kV0] = p;
}

void Bios::a0_realloc(PsxContext& ctx) {
    const uint32_t old = ctx.r[kA0], bytes = ctx.r[kA1];
    if (old == 0) { ctx.r[kV0] = heap_.alloc(bytes); return; }
    if (bytes == 0) { heap_.release(old); ctx.r[kV0] = 0; return; }
    const auto it = heap_.used.find(old);
    const uint32_t old_len = it == heap_.used.end() ? 0 : it->second;
    const uint32_t p = heap_.alloc(bytes);
    if (p) {
        for (uint32_t i = 0; i < old_len && i < bytes; ++i) psx_write8(&ctx, p + i, psx_read8(&ctx, old + i));
        heap_.release(old);
    }
    ctx.r[kV0] = p;
}

// ---------------------------------------------------------------------------------------------
// Kernel setup that has no native state: there is no instruction cache, CD kernel driver or
// kernel pad acknowledge to configure.

void Bios::a0_flush_cache(PsxContext&) {}
void Bios::a0_96_init(PsxContext&) {}
void Bios::a0_cd_remove(PsxContext&) {}

void Bios::b0_change_clear_pad(PsxContext& ctx) { clear_pad_ = ctx.r[kA0]; }

// GPU_cw(word): the kernel waits for the GPU to accept a command, then writes it to GP0.
// libgpu's ResetGraph sends a 24-bit address here, which the GPU decodes as GP0(00h) NOP.
void Bios::a0_gpu_cw(PsxContext& ctx) { psx_write32(&ctx, 0x1F801810u, ctx.r[kA0]); }

void Bios::c0_change_clear_rcnt(PsxContext& ctx) {
    const uint32_t t = ctx.r[kA0] & 3u;
    ctx.r[kV0] = clear_rcnt_[t];
    clear_rcnt_[t] = ctx.r[kA1];
}

// Kernel function tables. Libraries use these to patch kernel code (libgte _patch_gte checks
// the exception handler for known code and rewrites it). The real addresses are returned; kernel
// RAM is empty here, so the patchers' "is this the original code?" checks fail and they skip.
void Bios::b0_get_c0_table(PsxContext& ctx) { ctx.r[kV0] = 0x00000674u; }
void Bios::b0_get_b0_table(PsxContext& ctx) { ctx.r[kV0] = 0x00000874u; }

// ---------------------------------------------------------------------------------------------
// Files. fds 0/1 are the kernel TTY: the game's printf output. Card/CD files come later.

void Bios::b0_write(PsxContext& ctx) {
    const uint32_t fd = ctx.r[kA0], buf = ctx.r[kA1], len = ctx.r[kA2];
    if (fd > 1) {  // a memory-card file
        {
            // Scoped: the completion event below runs guest callbacks, which can reach a frame
            // boundary; no heap memory may be owned by this frame then (save states).
            std::vector<uint8_t> data(len);
            for (uint32_t i = 0; i < len; ++i) data[i] = psx_read8(&ctx, buf + i);
            const int n = card_fs_.write(static_cast<int>(fd), data.data(), len);
            ctx.r[kV0] = static_cast<uint32_t>(n);
        }
        file_async_event(ctx);
        return;
    }
    std::string text(len, '\0');
    for (uint32_t i = 0; i < len; ++i) text[i] = static_cast<char>(psx_read8(&ctx, buf + i));
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    ctx.r[kV0] = len;
}

// ---------------------------------------------------------------------------------------------
// Memory cards. Operations complete immediately and report through the kernel's card events:
// SwCARD (F4000001h) for _card_info/_card_load, HwCARD (F0000011h) for sector transfers, with
// spec 0004h done, 8000h error, 0100h timeout (no card), 2000h new card.

namespace {
constexpr uint32_t kSwCard = 0xF4000001u, kHwCard = 0xF0000011u;
constexpr uint32_t kCardDone = 0x0004u, kCardTimeout = 0x0100u, kCardError = 0x8000u;
}  // namespace

void Bios::insert_cards(const std::filesystem::path& save_dir) {
    cards_[0] = std::make_unique<MemoryCard>(save_dir / "card1.mcd");
    cards_[1].reset();
    card_fs_.set_slot(0, cards_[0].get());
    card_fs_.set_slot(1, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Memory-card files (bu00:/bu10:) through hle::CardFs. Guest strings and buffers are copied in and
// out; FASYNC operations also deliver completion events.

namespace {
std::string guest_string(PsxContext& ctx, uint32_t addr, size_t max = 128) {
    std::string out;
    for (size_t i = 0; i < max; ++i) {
        const char ch = static_cast<char>(psx_read8(&ctx, addr + static_cast<uint32_t>(i)));
        if (!ch) break;
        out.push_back(ch);
    }
    return out;
}
void put_dirent(PsxContext& ctx, uint32_t addr, const CardFs::DirEntry& e) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&e);
    for (uint32_t i = 0; i < sizeof e; ++i) psx_write8(&ctx, addr + i, bytes[i]);
}
}  // namespace

void Bios::file_async_event(PsxContext& ctx) {
    // The sector transfer (HwCARD) and the file operation (SwCARD) both complete. Games wait on
    // SwCARD: DCB's save loop polls it after every write and gives up ("card not detected").
    if (!card_fs_.last_was_async()) return;
    deliver_event(ctx, kHwCard, card_fs_.async_spec());
    deliver_event(ctx, kSwCard, card_fs_.async_spec());
}

void Bios::b0_open(PsxContext& ctx) {
    ctx.r[kV0] = static_cast<uint32_t>(card_fs_.open(guest_string(ctx, ctx.r[kA0]), ctx.r[kA1]));
}

void Bios::b0_lseek(PsxContext& ctx) {
    ctx.r[kV0] = static_cast<uint32_t>(
        card_fs_.lseek(static_cast<int>(ctx.r[kA0]), static_cast<int32_t>(ctx.r[kA1]), static_cast<int>(ctx.r[kA2])));
}

void Bios::b0_read(PsxContext& ctx) {
    {
        // Scoped like b0_write: nothing heap-owned may be live when the completion event runs.
        const uint32_t len = ctx.r[kA2];
        std::vector<uint8_t> data(len);
        const int n = card_fs_.read(static_cast<int>(ctx.r[kA0]), data.data(), len);
        for (int i = 0; i < n; ++i) psx_write8(&ctx, ctx.r[kA1] + static_cast<uint32_t>(i), data[static_cast<size_t>(i)]);
        ctx.r[kV0] = static_cast<uint32_t>(n);
    }
    file_async_event(ctx);
}

void Bios::b0_close(PsxContext& ctx) { ctx.r[kV0] = static_cast<uint32_t>(card_fs_.close(static_cast<int>(ctx.r[kA0]))); }

void Bios::b0_format(PsxContext& ctx) { ctx.r[kV0] = card_fs_.format(guest_string(ctx, ctx.r[kA0])) ? 1 : 0; }

void Bios::b0_firstfile(PsxContext& ctx) {
    CardFs::DirEntry e{};
    const bool ok = card_fs_.firstfile(guest_string(ctx, ctx.r[kA0]), e);
    if (ok) put_dirent(ctx, ctx.r[kA1], e);
    ctx.r[kV0] = ok ? ctx.r[kA1] : 0;
}

void Bios::b0_nextfile(PsxContext& ctx) {
    CardFs::DirEntry e{};
    const bool ok = card_fs_.nextfile(e);
    if (ok) put_dirent(ctx, ctx.r[kA0], e);
    ctx.r[kV0] = ok ? ctx.r[kA0] : 0;
}

void Bios::b0_rename(PsxContext& ctx) {
    ctx.r[kV0] = card_fs_.rename(guest_string(ctx, ctx.r[kA0]), guest_string(ctx, ctx.r[kA1])) ? 1 : 0;
}

void Bios::b0_erase(PsxContext& ctx) { ctx.r[kV0] = card_fs_.erase(guest_string(ctx, ctx.r[kA0])) ? 1 : 0; }

void Bios::b0_get_last_error(PsxContext& ctx) { ctx.r[kV0] = card_fs_.last_error(); }

void Bios::b0_get_last_file_error(PsxContext& ctx) { ctx.r[kV0] = card_fs_.file_error(static_cast<int>(ctx.r[kA0])); }

MemoryCard* Bios::card(uint32_t port) { return cards_[(port >> 4) & 1u].get(); }

void Bios::card_result(PsxContext& ctx, uint32_t ev_class, MemoryCard* c) {
    deliver_event(ctx, ev_class, c ? kCardDone : kCardTimeout);
}

void Bios::a0_bu_init(PsxContext&) {}
void Bios::b0_init_card(PsxContext&) {}
void Bios::b0_start_card(PsxContext&) {}
void Bios::b0_stop_card(PsxContext&) {}
void Bios::b0_allow_new_card(PsxContext&) {}

void Bios::a0_card_info(PsxContext& ctx) {
    card_result(ctx, kSwCard, card(ctx.r[kA0]));
    ctx.r[kV0] = 1;
}

void Bios::a0_card_load(PsxContext& ctx) {
    card_result(ctx, kSwCard, card(ctx.r[kA0]));
    ctx.r[kV0] = 1;
}

void Bios::b0_read_card_sector(PsxContext& ctx) {
    MemoryCard* c = card(ctx.r[kA0]);
    uint8_t frame[MemoryCard::kFrameSize];
    const bool ok = c && c->read_frame(ctx.r[kA1], frame);
    if (ok) {
        for (uint32_t i = 0; i < MemoryCard::kFrameSize; ++i) psx_write8(&ctx, ctx.r[kA2] + i, frame[i]);
    }
    deliver_event(ctx, kHwCard, !c ? kCardTimeout : ok ? kCardDone : kCardError);
    ctx.r[kV0] = 1;
}

void Bios::b0_write_card_sector(PsxContext& ctx) {
    MemoryCard* c = card(ctx.r[kA0]);
    uint8_t frame[MemoryCard::kFrameSize];
    for (uint32_t i = 0; i < MemoryCard::kFrameSize; ++i) frame[i] = psx_read8(&ctx, ctx.r[kA2] + i);
    const bool ok = c && c->write_frame(ctx.r[kA1], frame);
    deliver_event(ctx, kHwCard, !c ? kCardTimeout : ok ? kCardDone : kCardError);
    ctx.r[kV0] = 1;
}

void Bios::b0_get_card_status(PsxContext& ctx) { ctx.r[kV0] = 0x01; }  // ready (never busy)

// ---------------------------------------------------------------------------------------------
// Events.

Bios::Event* Bios::event(uint32_t handle) {
    const uint32_t index = handle - kEventHandleBase;
    if ((handle & 0xFFFF0000u) != kEventHandleBase || index >= kMaxEvents || events_[index].status == 0) return nullptr;
    return &events_[index];
}

void Bios::b0_open_event(PsxContext& ctx) {
    for (size_t i = 0; i < kMaxEvents; ++i) {
        if (events_[i].status == 0) {
            events_[i] = {ctx.r[kA0], ctx.r[kA1], ctx.r[kA2], ctx.r[kA3], kEvDisabled};
            ctx.r[kV0] = kEventHandleBase | static_cast<uint32_t>(i);
            return;
        }
    }
    ctx.r[kV0] = 0xFFFFFFFFu;
}

void Bios::b0_close_event(PsxContext& ctx) {
    Event* ev = event(ctx.r[kA0]);
    if (ev) *ev = Event{};
    ctx.r[kV0] = ev ? 1 : 0;
}

void Bios::b0_enable_event(PsxContext& ctx) {
    Event* ev = event(ctx.r[kA0]);
    if (ev) ev->status = kEvEnabled;
    ctx.r[kV0] = ev ? 1 : 0;
}

void Bios::b0_disable_event(PsxContext& ctx) {
    Event* ev = event(ctx.r[kA0]);
    if (ev) ev->status = kEvDisabled;
    ctx.r[kV0] = ev ? 1 : 0;
}

void Bios::b0_test_event(PsxContext& ctx) {
    Event* ev = event(ctx.r[kA0]);
    const bool ready = ev && ev->status == kEvReady;
    if (ready) ev->status = kEvEnabled;
    ctx.r[kV0] = ready ? 1 : 0;
}

void Bios::b0_wait_event(PsxContext& ctx) {
    Event* ev = event(ctx.r[kA0]);
    if (ev && ev->status == kEvReady) {
        ev->status = kEvEnabled;
        ctx.r[kV0] = 1;
        return;
    }
    if (ev && ev->status == kEvEnabled && system_) {
        // The kernel spins until an interrupt handler delivers the event: let time pass and
        // interrupts run until it does. A few seconds without it means nothing will deliver it.
        const uint64_t give_up = system_->cpu_cycles() + static_cast<uint64_t>(5 * System::kCpuHz);
        while (ev->status == kEvEnabled) {
            system_->idle(ctx);  // no guest code runs here: let guest time reach the next event
            if (system_->cpu_cycles() > give_up) {
                std::fprintf(stderr, "[bios] WaitEvent(%08X class=%08X spec=%X): never delivered (ra=%08X)\n",
                             ctx.r[kA0], ev->ev_class, ev->spec, ctx.r[kRa]);
                std::abort();
            }
        }
        if (ev->status == kEvReady) {
            ev->status = kEvEnabled;
            ctx.r[kV0] = 1;
            return;
        }
    }
    ctx.r[kV0] = 0;
}

void Bios::deliver_event(PsxContext& ctx, uint32_t ev_class, uint32_t spec) {
    for (Event& ev : events_) {
        if (ev.status != kEvEnabled || ev.ev_class != ev_class || ev.spec != spec) continue;
        if (ev.mode == kEvModeCallback) {
            if (ev.func) psx::call_guest(ctx, ev.func);
        } else {
            ev.status = kEvReady;
        }
    }
}

void Bios::b0_deliver_event(PsxContext& ctx) { deliver_event(ctx, ctx.r[kA0], ctx.r[kA1]); }

// ---------------------------------------------------------------------------------------------
// Exceptions / interrupts.

// HookEntryInt(jmp_buf): on every interrupt the kernel longjmps here. Natively the interrupt
// source calls libetc's dispatcher instead; the buffer is kept for reference (sp/gp values).
void Bios::b0_hook_entry_int(PsxContext& ctx) { hook_entry_int_ = ctx.r[kA0]; }

void Bios::b0_set_default_exit_from_exception(PsxContext&) { hook_entry_int_ = 0; }

void Bios::b0_return_from_exception(PsxContext& ctx) {
    if (!system_) {
        std::fprintf(stderr, "[bios] ReturnFromException without an interrupt system (ra=%08X)\n", ctx.r[kRa]);
        std::abort();
    }
    system_->return_from_exception();
}

void Bios::c0_sys_enq_int_rp(PsxContext& ctx) {
    // The kernel links new blocks at the head of their priority's chain.
    if (int_handler_count_ == kMaxIntHandlers) {
        std::fprintf(stderr, "[bios] SysEnqIntRP: more than %zu interrupt handlers\n", kMaxIntHandlers);
        std::abort();
    }
    std::copy_backward(int_handlers_.begin(), int_handlers_.begin() + static_cast<std::ptrdiff_t>(int_handler_count_),
                       int_handlers_.begin() + static_cast<std::ptrdiff_t>(int_handler_count_ + 1));
    int_handlers_[0] = {ctx.r[kA0], ctx.r[kA1]};
    ++int_handler_count_;
    ctx.r[kV0] = 0;
}

void Bios::run_interrupt_chains(PsxContext& ctx, uint32_t kernel_sp) {
    for (uint32_t priority = 0; priority < 4; ++priority) {
        // Walk a copy on the stack: handlers may (de)register, and the guest calls can reach a
        // frame boundary (see the member's comment).
        const IntHandlers handlers = int_handlers_;
        const size_t count = int_handler_count_;
        for (size_t i = 0; i < count; ++i) {
            const IntHandler h = handlers[i];
            if (h.priority != priority) continue;
            const uint32_t second = psx_read32(&ctx, h.block + 4);
            const uint32_t first = psx_read32(&ctx, h.block + 8);
            const uint32_t v = first ? psx::call_guest_on_stack(ctx, kernel_sp, first) : 1u;
            if (v && second) psx::call_guest_on_stack(ctx, kernel_sp, second, v);
        }
    }
}

void Bios::c0_sys_deq_int_rp(PsxContext& ctx) {
    size_t kept = 0;
    for (size_t i = 0; i < int_handler_count_; ++i) {
        const IntHandler h = int_handlers_[i];
        if (h.priority == ctx.r[kA0] && h.block == ctx.r[kA1]) continue;
        int_handlers_[kept++] = h;
    }
    for (size_t i = kept; i < int_handler_count_; ++i) int_handlers_[i] = {};
    int_handler_count_ = kept;
    ctx.r[kV0] = 0;
}

// ---------------------------------------------------------------------------------------------
// Save state

void Bios::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("BIOS"), 1);
    w.u32(heap_.base);
    w.u32(heap_.size);
    w.map(heap_.used);
    for (const Event& e : events_) {
        w.u32(e.ev_class);
        w.u32(e.spec);
        w.u32(e.mode);
        w.u32(e.func);
        w.u32(e.status);
    }
    w.size(int_handler_count_);
    for (size_t i = 0; i < int_handler_count_; ++i) {
        w.u32(int_handlers_[i].priority);
        w.u32(int_handlers_[i].block);
    }
    w.u32(hook_entry_int_);
    w.u32(clear_pad_);
    w.pod(clear_rcnt_);
    card_fs_.save_state(w);
    w.end();
}

void Bios::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("BIOS"), 1);
    heap_.base = r.u32();
    heap_.size = r.u32();
    r.map(heap_.used, 1u << 20);
    for (Event& e : events_) {
        e.ev_class = r.u32();
        e.spec = r.u32();
        e.mode = r.u32();
        e.func = r.u32();
        e.status = r.u32();
    }
    int_handlers_ = {};
    int_handler_count_ = r.size(kMaxIntHandlers);
    for (size_t i = 0; i < int_handler_count_; ++i) {
        int_handlers_[i].priority = r.u32();
        int_handlers_[i].block = r.u32();
    }
    hook_entry_int_ = r.u32();
    clear_pad_ = r.u32();
    r.pod(clear_rcnt_);
    card_fs_.load_state(r);
    r.end();
}

}  // namespace hle
