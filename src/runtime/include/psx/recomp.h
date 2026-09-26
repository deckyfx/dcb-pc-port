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

#ifdef __cplusplus
}
#endif

#endif /* PSX_RECOMP_H */
