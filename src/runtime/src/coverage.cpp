// Opt-in function coverage: counters, JSON report, live call trace.
// Guest-neutral by construction: when disarmed only a single predictable
// branch runs (PSX_COVER in psx/recomp.h); arming only appends to host-side
// counters and files, never to guest RAM, registers, or cycle counts.

#include <psx/coverage.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {

PsxCoverageEntry* psx_coverage_entries = nullptr;
uint32_t psx_coverage_count = 0;
int psx_coverage_armed = 0;
uint64_t psx_coverage_frame = 0;

// The id->(address, overlay, name) mapping lives in generated code (the
// recompiler emits it into function_table.c); tests link the empty stub
// (src/runtime/stub/empty_function_table.c) instead.

static uint64_t trace_left = 0;
static int trace_init = 0;

void psx_cover(uint32_t id) {
    if (id < psx_coverage_count) {
        PsxCoverageEntry* e = &psx_coverage_entries[id];
        if (e->calls == 0) e->first_frame = psx_coverage_frame;
        e->calls++;
    }
    if (!trace_init) {
        trace_init = 1;
        if (const char* s = std::getenv("DCB_TRACE_CALLS")) trace_left = std::strtoull(s, nullptr, 10);
    }
    if (trace_left > 0) {
        --trace_left;
        const char* sym = "";
        char addrbuf[16] = "";
        if (id < psx_coverage_name_count) {
            sym = psx_coverage_names[id].symbol;
            std::snprintf(addrbuf, sizeof addrbuf, "%08X", psx_coverage_names[id].addr);
        } else {
            std::snprintf(addrbuf, sizeof addrbuf, "id%-7u", id);
        }
        // Ghidra names are resolved by the diff/import tooling (symbols file);
        // the live log shows the C symbol, which maps 1:1 to the address.
        std::fprintf(stderr, "[call] frame %llu %s %s\n", static_cast<unsigned long long>(psx_coverage_frame),
                     addrbuf, sym);
    }
}

void psx_coverage_init(uint32_t count) {
    delete[] psx_coverage_entries;
    psx_coverage_entries = nullptr;
    psx_coverage_count = 0;
    if (count == 0) return;
    psx_coverage_entries = new (std::nothrow) PsxCoverageEntry[count];
    if (!psx_coverage_entries) return;
    for (uint32_t i = 0; i < count; ++i) {
        psx_coverage_entries[i].calls = 0;
        psx_coverage_entries[i].first_frame = UINT64_MAX;
    }
    psx_coverage_count = count;
}

int psx_coverage_write_json(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return -1;
    std::fprintf(f, "{\"version\":1,\"count\":%u,\"entries\":[", psx_coverage_count);
    const uint32_t names = psx_coverage_name_count;
    for (uint32_t i = 0; i < psx_coverage_count; ++i) {
        const PsxCoverageEntry& e = psx_coverage_entries[i];
        const uint64_t first = e.first_frame == UINT64_MAX ? 0 : e.first_frame;
        if (i) std::fprintf(f, ",");
        if (i < names) {
            std::fprintf(f, "{\"id\":%u,\"addr\":\"0x%08X\",\"overlay\":\"%s\",\"symbol\":\"%s\",\"calls\":%llu,\"first_frame\":%llu}",
                         i, psx_coverage_names[i].addr, psx_coverage_names[i].overlay, psx_coverage_names[i].symbol,
                         static_cast<unsigned long long>(e.calls), static_cast<unsigned long long>(first));
        } else {
            std::fprintf(f, "{\"id\":%u,\"calls\":%llu,\"first_frame\":%llu}", i,
                         static_cast<unsigned long long>(e.calls), static_cast<unsigned long long>(first));
        }
    }
    std::fprintf(f, "]}\n");
    return std::fclose(f) == 0 ? 0 : -1;
}

}  // extern "C"
