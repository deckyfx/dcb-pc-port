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
//   todo   B0:32 open  B0:33 lseek  B0:34 read  B0:36 close  B0:42 firstfile  B0:43 nextfile
//   todo   A0:70 _bu_init  A0:AB _card_info  A0:AC _card_load  B0:4A InitCard  B0:4B StartCard
//          B0:4C StopCard  B0:4E write_card_sector  B0:4F read_card_sector  B0:50 allow_new_card
//          B0:5C get_card_status
//   done   A0:49 GPU_cw (-> GP0 port)
//   done   B0:56 GetC0Table  B0:57 GetB0Table (kernel patchers find nothing to patch)
//   todo   A0:51 LoadExec (switch to PSX2.EXE)

#include "bios/bios.hpp"

#include "system.hpp"

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
    if (fd > 1) {
        std::fprintf(stderr, "[bios] write(fd=%u) to a file is not implemented yet (ra=%08X)\n", fd, ctx.r[kRa]);
        std::abort();
    }
    std::string text(len, '\0');
    for (uint32_t i = 0; i < len; ++i) text[i] = static_cast<char>(psx_read8(&ctx, buf + i));
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    ctx.r[kV0] = len;
}

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
            system_->io_poll();
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
    int_handlers_.push_back({ctx.r[kA0], ctx.r[kA1]});
    ctx.r[kV0] = 0;
}

void Bios::c0_sys_deq_int_rp(PsxContext& ctx) {
    std::erase_if(int_handlers_, [&](const IntHandler& h) { return h.priority == ctx.r[kA0] && h.block == ctx.r[kA1]; });
    ctx.r[kV0] = 0;
}

}  // namespace hle
