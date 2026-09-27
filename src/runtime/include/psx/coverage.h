/*
 * psx/coverage.h — opt-in per-function call coverage for reverse engineering.
 *
 * Every generated function calls PSX_COVER(id) from its prologue (the recompiler
 * emits the id; see tools/recomp/src/emit.cpp). When coverage is off — the
 * default — that is a single well-predicted branch on a global flag, and guest
 * state and guest time stay bit-identical to a run without it.
 *
 * Plain C so generated C17 code can call in. The game binary links the
 * implementation from the runtime (runtime/src/coverage.cpp).
 */
#ifndef PSX_COVERAGE_H
#define PSX_COVERAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One counter pair per recompiled function, indexed by the coverage id the
 * recompiler assigned (0..psx_coverage_count-1). ids are dense per image:
 * the boot EXE's table comes first, then each overlay's table. */
typedef struct PsxCoverageEntry {
    uint64_t calls;        /* entries through the function prologue */
    uint64_t first_frame;  /* host frame of the first call, or UINT64_MAX */
} PsxCoverageEntry;

extern PsxCoverageEntry* psx_coverage_entries;
extern uint32_t psx_coverage_count;

/* id -> guest address mapping, emitted by the recompiler into function_table.c
 * (dense in emit order: boot EXE first, then overlays). Overlays share one
 * address window, so (overlay, addr) jointly identify a function. */
typedef struct PsxCoverageName {
    uint32_t addr;
    const char* overlay;  // "" for the boot EXE
    const char* symbol;   // C symbol (f_80013F80, o_KAWSEG_801E2A6C, ...)
} PsxCoverageName;

extern const PsxCoverageName psx_coverage_names[];
extern const uint32_t psx_coverage_name_count;

/* Non-zero while any coverage consumer is active (DCB_COVERAGE or
 * DCB_TRACE_CALLS). Checked first; the counters below are cold when off. */
extern int psx_coverage_armed;

/* Current host frame, maintained by the host loop for first_frame stamps. */
extern uint64_t psx_coverage_frame;

/* Record one entry. `id` must be < psx_coverage_count. Also emits the live
 * log while the DCB_TRACE_CALLS budget lasts. The PSX_COVER macro lives in
 * psx/recomp.h so generated C code gets it without a second include. */
void psx_cover(uint32_t id);

/* Size the table before generated code runs (called once at startup with the
 * total function count across the boot EXE and all overlays). Frees any
 * previous table. */
void psx_coverage_init(uint32_t count);
/* Write the JSON report (see docs/RE_WORKFLOW.md for the schema). */
int psx_coverage_write_json(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PSX_COVERAGE_H */
