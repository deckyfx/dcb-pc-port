/*
 * psx/recomp.h — the contract between generated MIPS->C code and the runtime.
 *
 * Plain C so tools/recomp output compiles as C17. Every recompiled function has the
 * signature `void fn(PsxContext*)` and is named after its guest address (func_80012345).
 */
#ifndef PSX_RECOMP_H
#define PSX_RECOMP_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_RAM_SIZE     0x200000u   /* 2 MiB main RAM, mirrored 4x below 0x00800000 */
#define PSX_SCRATCH_BASE 0x1F800000u
#define PSX_SCRATCH_SIZE 0x400u      /* 1 KiB data cache used as fast RAM */

/** R3000A architectural state plus the guest memory the generated code touches directly. */
typedef struct PsxContext {
    uint32_t r[32];        /* GPRs; r[0] is re-zeroed by generated code after writes */
    uint32_t hi, lo;
    uint32_t pc;           /* valid only at dispatch / HLE boundaries */
    uint32_t cop0[32];     /* SR = 12, CAUSE = 13, EPC = 14 */
    uint32_t gte_data[32]; /* COP2 data registers */
    uint32_t gte_ctrl[32]; /* COP2 control registers */
    uint8_t* ram;          /* PSX_RAM_SIZE bytes */
    uint8_t* scratch;      /* PSX_SCRATCH_SIZE bytes */
    void*    host;         /* owning psx::Machine; opaque to generated code */
    int32_t  poll_budget;  /* loop back-edges left before the next interrupt/timing poll */
    uint64_t cycles;       /* guest time: estimated R3000A cycles executed (drives VBLANK/timers) */
} PsxContext;

typedef void (*RecompFunc)(PsxContext* ctx);

/** One entry per recompiled function; emitted by the recompiler, sorted by address. */
typedef struct RecompFunctionEntry {
    uint32_t   addr;
    RecompFunc fn;
} RecompFunctionEntry;

extern const RecompFunctionEntry recomp_function_table[];
extern const uint32_t            recomp_function_count;

/* ---- slow path: scratchpad, MMIO, BIOS ROM, unmapped (runtime/src/memory.cpp) ---- */
uint8_t  psx_slow_read8(PsxContext* ctx, uint32_t addr);
uint16_t psx_slow_read16(PsxContext* ctx, uint32_t addr);
uint32_t psx_slow_read32(PsxContext* ctx, uint32_t addr);
void     psx_slow_write8(PsxContext* ctx, uint32_t addr, uint8_t value);
void     psx_slow_write16(PsxContext* ctx, uint32_t addr, uint16_t value);
void     psx_slow_write32(PsxContext* ctx, uint32_t addr, uint32_t value);

/* ---- control flow the recompiler cannot resolve statically (runtime/src/dispatch.cpp) ---- */
void psx_dispatch(PsxContext* ctx, uint32_t target);           /* jr / jalr / BIOS A0,B0,C0 */
void psx_syscall(PsxContext* ctx, uint32_t code);
void psx_break(PsxContext* ctx, uint32_t code);

/** Main-RAM offset for KUSEG/KSEG0/KSEG1 addresses below 8 MiB, or -1. */
static inline int32_t psx_ram_offset(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    return phys < 0x00800000u ? (int32_t)(phys & (PSX_RAM_SIZE - 1u)) : -1;
}

/* Fast path: RAM directly, everything else through the slow path.
 * memcpy keeps unaligned host access and aliasing well-defined; compilers emit single moves. */
static inline uint32_t psx_read32(PsxContext* ctx, uint32_t addr) {
    int32_t off = psx_ram_offset(addr);
    if (off >= 0) { uint32_t v; memcpy(&v, ctx->ram + off, 4); return v; }
    return psx_slow_read32(ctx, addr);
}
static inline uint16_t psx_read16(PsxContext* ctx, uint32_t addr) {
    int32_t off = psx_ram_offset(addr);
    if (off >= 0) { uint16_t v; memcpy(&v, ctx->ram + off, 2); return v; }
    return psx_slow_read16(ctx, addr);
}
static inline uint8_t psx_read8(PsxContext* ctx, uint32_t addr) {
    int32_t off = psx_ram_offset(addr);
    return off >= 0 ? ctx->ram[off] : psx_slow_read8(ctx, addr);
}
static inline void psx_write32(PsxContext* ctx, uint32_t addr, uint32_t v) {
    int32_t off = psx_ram_offset(addr);
    if (off >= 0) memcpy(ctx->ram + off, &v, 4); else psx_slow_write32(ctx, addr, v);
}
static inline void psx_write16(PsxContext* ctx, uint32_t addr, uint16_t v) {
    int32_t off = psx_ram_offset(addr);
    if (off >= 0) memcpy(ctx->ram + off, &v, 2); else psx_slow_write16(ctx, addr, v);
}
static inline void psx_write8(PsxContext* ctx, uint32_t addr, uint8_t v) {
    int32_t off = psx_ram_offset(addr);
    if (off >= 0) ctx->ram[off] = v; else psx_slow_write8(ctx, addr, v);
}

/* ---- overlays: several code images share one load window; the runtime picks the resident one ---- */
typedef struct RecompOverlayEntry {
    uint32_t   addr;
    RecompFunc fn;
    uint32_t   check[4];   /* first 4 code words: identifies which overlay is loaded */
} RecompOverlayEntry;

typedef struct RecompOverlay {
    const char*               name;
    uint32_t                  base;
    uint32_t                  size;
    const RecompOverlayEntry* entries;  /* sorted by addr */
    uint32_t                  count;
} RecompOverlay;

extern const RecompOverlay recomp_overlays[];
extern const uint32_t      recomp_overlay_count;

/* ---- guest time + interrupts. Every loop back-edge charges the loop body's estimated cycles,
 * so cycle-counted waits (Psy-Q timeouts, delay loops) take as long in guest time as on the
 * console, and periodically polls so spin-waits on RAM flags let VBLANK etc. through. ---- */
void psx_poll(PsxContext* ctx);
#define PSX_POLL(ctx, cyc) do { (ctx)->cycles += (cyc); if (--(ctx)->poll_budget < 0) psx_poll(ctx); } while (0)

/* ---- traps (runtime/src/dispatch.cpp) ---- */
void psx_invalid(PsxContext* ctx, uint32_t pc);
void psx_bad_return(PsxContext* ctx, uint32_t expected_ra);

/* ---- reverse-engineering coverage (runtime/src/coverage.cpp) ----
 * Every generated function calls PSX_COVER(id) from its prologue when the
 * recompiler emits coverage ids (tools/recomp always does now). Off by
 * default: a single predictable branch on psx_coverage_armed, so guest state
 * and timing are bit-identical with it off. */
extern int psx_coverage_armed;
void psx_cover(uint32_t id);
static inline void psx_cover_checked(uint32_t id) {
    if (psx_coverage_armed) psx_cover(id);
}
#define PSX_COVER(id) psx_cover_checked(id)

/* Debug builds verify every `jr ra` returns to the address its caller set, which catches
 * longjmp/context-switch tricks the C call stack cannot follow. */
#ifdef PSX_CHECK_RA
#define PSX_FUNCTION_PROLOGUE(ctx) const uint32_t psx_ra_in = (ctx)->r[31]
#define PSX_FUNCTION_RETURN(ctx) \
    do { if ((ctx)->r[31] != psx_ra_in) psx_bad_return((ctx), psx_ra_in); } while (0)
#else
#define PSX_FUNCTION_PROLOGUE(ctx) ((void)0)
#define PSX_FUNCTION_RETURN(ctx) ((void)0)
#endif

/* ---- multiply / divide with R3000A results for the edge cases ---- */
static inline void psx_mult(PsxContext* ctx, uint32_t a, uint32_t b) {
    const int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    ctx->lo = (uint32_t)p;
    ctx->hi = (uint32_t)((uint64_t)p >> 32);
}
static inline void psx_multu(PsxContext* ctx, uint32_t a, uint32_t b) {
    const uint64_t p = (uint64_t)a * (uint64_t)b;
    ctx->lo = (uint32_t)p;
    ctx->hi = (uint32_t)(p >> 32);
}
static inline void psx_div(PsxContext* ctx, uint32_t n, uint32_t d) {
    if (d == 0) {                                   /* no exception: fixed garbage results */
        ctx->hi = n;
        ctx->lo = (int32_t)n >= 0 ? 0xFFFFFFFFu : 1u;
    } else if (n == 0x80000000u && d == 0xFFFFFFFFu) {
        ctx->hi = 0;
        ctx->lo = 0x80000000u;
    } else {
        ctx->lo = (uint32_t)((int32_t)n / (int32_t)d);
        ctx->hi = (uint32_t)((int32_t)n % (int32_t)d);
    }
}
static inline void psx_divu(PsxContext* ctx, uint32_t n, uint32_t d) {
    if (d == 0) {
        ctx->hi = n;
        ctx->lo = 0xFFFFFFFFu;
    } else {
        ctx->lo = n / d;
        ctx->hi = n % d;
    }
}

/* ---- unaligned word access (little-endian lwl/lwr/swl/swr) ---- */
static inline uint32_t psx_lwl(PsxContext* ctx, uint32_t addr, uint32_t old) {
    const uint32_t w = psx_read32(ctx, addr & ~3u);
    switch (addr & 3u) {
        case 0:  return (old & 0x00FFFFFFu) | (w << 24);
        case 1:  return (old & 0x0000FFFFu) | (w << 16);
        case 2:  return (old & 0x000000FFu) | (w << 8);
        default: return w;
    }
}
static inline uint32_t psx_lwr(PsxContext* ctx, uint32_t addr, uint32_t old) {
    const uint32_t w = psx_read32(ctx, addr & ~3u);
    switch (addr & 3u) {
        case 0:  return w;
        case 1:  return (old & 0xFF000000u) | (w >> 8);
        case 2:  return (old & 0xFFFF0000u) | (w >> 16);
        default: return (old & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void psx_swl(PsxContext* ctx, uint32_t addr, uint32_t v) {
    const uint32_t a = addr & ~3u, m = psx_read32(ctx, a);
    switch (addr & 3u) {
        case 0:  psx_write32(ctx, a, (m & 0xFFFFFF00u) | (v >> 24)); break;
        case 1:  psx_write32(ctx, a, (m & 0xFFFF0000u) | (v >> 16)); break;
        case 2:  psx_write32(ctx, a, (m & 0xFF000000u) | (v >> 8)); break;
        default: psx_write32(ctx, a, v); break;
    }
}
static inline void psx_swr(PsxContext* ctx, uint32_t addr, uint32_t v) {
    const uint32_t a = addr & ~3u, m = psx_read32(ctx, a);
    switch (addr & 3u) {
        case 0:  psx_write32(ctx, a, v); break;
        case 1:  psx_write32(ctx, a, (m & 0x000000FFu) | (v << 8)); break;
        case 2:  psx_write32(ctx, a, (m & 0x0000FFFFu) | (v << 16)); break;
        default: psx_write32(ctx, a, (m & 0x00FFFFFFu) | (v << 24)); break;
    }
}

/* ---- COP0 ---- */
static inline void psx_rfe(PsxContext* ctx) {
    ctx->cop0[12] = (ctx->cop0[12] & ~0xFu) | ((ctx->cop0[12] >> 2) & 0xFu);
}

/* ---- GTE / COP2 (runtime/src/gte.cpp) ---- */
uint32_t psx_gte_read_data(PsxContext* ctx, uint32_t reg);
void     psx_gte_write_data(PsxContext* ctx, uint32_t reg, uint32_t value);
uint32_t psx_gte_read_ctrl(PsxContext* ctx, uint32_t reg);
void     psx_gte_write_ctrl(PsxContext* ctx, uint32_t reg, uint32_t value);
void     psx_gte_command(PsxContext* ctx, uint32_t command);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RECOMP_H */
