// Native replacements for the three functions of the game's task library that switch stacks
// (config/SLPS-03101/overrides.json). Everything else in the library (task table, priorities,
// picking the next task, the per-frame scheduler) stays recompiled game code.
//
// Each override mirrors its MIPS routine step by step (Ghidra: 80014BC4, 80014BFC, 80014D3C and
// their shared tail at 80014C70), with one change: where the routine would store a code address
// to resume at, it stores the current task's fiber cookie, and the jump into the next task is
// hle::System::switch_context. Resuming a cookie continues right here, after the switch.

#include "system.hpp"

#include <psx/recomp.h>
#include <psx/runtime.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kCurrentTask = 0x8007A5ECu;  // pointer to the running task's block
constexpr uint32_t kPickNext = 0x80014404u;     // (block) -> next block to run
constexpr uint32_t kUnlinkAndPick = 0x8001487Cu;  // unlink running task, free its stack, pick next

constexpr int kA0 = 4, kGp = 28, kSp = 29, kS8 = 30, kRa = 31;

uint32_t rd(PsxContext& ctx, uint32_t a) { return psx_read32(&ctx, a); }
void wr(PsxContext& ctx, uint32_t a, uint32_t v) { psx_write32(&ctx, a, v); }

/// "mfc0 t0,SR; SR = (SR & ~0x3F) | (SR & 0xF) << 2": push the interrupt-enable stack (disable).
uint32_t push_sr(PsxContext& ctx) {
    uint32_t& sr = ctx.cop0[12];
    sr = (sr & ~0x3Fu) | ((sr & 0xFu) << 2);
    return sr;
}

void save_callee_saved(PsxContext& ctx, uint32_t task) {
    for (uint32_t i = 16; i <= 23; ++i) wr(ctx, task + 0x20 + 4 * i, ctx.r[i]);  // s0-s7 at +0x60
    wr(ctx, task + 0x90, ctx.r[kGp]);
    wr(ctx, task + 0x94, ctx.r[kSp]);
    wr(ctx, task + 0x98, ctx.r[kS8]);
}

/// Shared tail (80014C70): run `next`; bit 29 of its status selects the full-register resume.
void run_next(PsxContext& ctx, uint32_t next) {
    const bool full = (rd(ctx, next) & 0x20000000u) != 0;
    hle::System::from(ctx).switch_context(ctx, next, full);
}

}  // namespace

extern "C" {

// 80014BFC: yield to the next runnable task.
void dcb_task_yield(PsxContext* ctx) {
    auto& sys = hle::System::from(*ctx);
    const uint32_t sr = push_sr(*ctx);
    const uint32_t cur = rd(*ctx, kCurrentTask);
    wr(*ctx, cur + 0xAC, sr);
    save_callee_saved(*ctx, cur);
    wr(*ctx, cur + 0xA0, sys.current_cookie());   // original: ra
    psx_write16(ctx, cur + 2, 0x8000);
    run_next(*ctx, psx::call_guest(*ctx, kPickNext, cur));
}

// 80014D3C: sleep for a0 frames (yield a0 times), then return the task's wake-up value (+0x18).
void dcb_task_sleep(PsxContext* ctx) {
    auto& sys = hle::System::from(*ctx);
    uint32_t frames = ctx->r[kA0];
    uint32_t sr = push_sr(*ctx) & ~4u;
    uint32_t cur = rd(*ctx, kCurrentTask);
    wr(*ctx, cur + 0xAC, sr);
    save_callee_saved(*ctx, cur);
    wr(*ctx, cur + 0x9C, ctx->r[kRa]);
    uint32_t wake = rd(*ctx, cur + 0x18);
    if (wake == 0) {
        wr(*ctx, cur + 0xA0, sys.current_cookie());  // original: continuation 80014DD4
        wr(*ctx, cur + 4, frames);
        psx_write16(ctx, cur + 2, 0x8000);
        for (;;) {
            frames -= 1;
            wr(*ctx, cur + 4, frames);
            if (static_cast<int32_t>(frames) <= 0) break;
            run_next(*ctx, psx::call_guest(*ctx, kPickNext, cur));
            // 80014DD4: resumed for the next frame
            cur = rd(*ctx, kCurrentTask);
            frames = rd(*ctx, cur + 4);
        }
        sr = rd(*ctx, cur + 0xAC);
        wake = rd(*ctx, cur + 0x18);
    }
    wr(*ctx, cur + 0xAC, sr | 4u);
    wr(*ctx, cur + 0x28, wake);                   // returned in v0 on resume
    wr(*ctx, cur + 0xA0, sys.current_cookie());   // original: ra (return to the caller)
    wr(*ctx, cur + 0x18, 0);
    run_next(*ctx, psx::call_guest(*ctx, kPickNext, cur));
}

// 80014BC4: every task's return address: unlink the task, then run the next one for good.
void dcb_task_exit(PsxContext* ctx) {
    auto& sys = hle::System::from(*ctx);
    push_sr(*ctx);
    const uint32_t next = psx::call_guest(*ctx, kUnlinkAndPick);
    sys.end_current_task();
    run_next(*ctx, next);
    std::fprintf(stderr, "[task] a finished task was resumed\n");
    std::abort();
}

}  // extern "C"
