// Names code addresses for guest call chains (psx::backtrace): the recompiled function containing
// an address, from the generated function tables (overlays: the one loaded now, identified by the
// check words the dispatcher uses).

#include <psx/backtrace.hpp>
#include <psx/recomp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/// Entry and symbol of the recompiled function containing `addr` (0 if none).
uint32_t containing_function(PsxContext* ctx, uint32_t addr, const char** symbol) {
    // Overlays first: the window overlaps nothing in the boot EXE, and several overlays share it;
    // the loaded one is the one whose entry's check words match RAM.
    for (uint32_t i = 0; i < recomp_overlay_count; ++i) {
        const RecompOverlay& ov = recomp_overlays[i];
        if (addr < ov.base || addr >= ov.base + ov.size || ov.count == 0) continue;
        const RecompOverlayEntry* end = ov.entries + ov.count;
        const RecompOverlayEntry* it = std::upper_bound(ov.entries, end, addr,
                                                        [](uint32_t a, const RecompOverlayEntry& e) { return a < e.addr; });
        if (it == ov.entries) continue;
        --it;
        const int32_t off = psx_ram_offset(it->addr);
        if (off < 0 || std::memcmp(ctx->ram + off, it->check, sizeof it->check) != 0) continue;
        *symbol = ov.name;
        return it->addr;
    }
    const RecompFunctionEntry* end = recomp_function_table + recomp_function_count;
    const RecompFunctionEntry* it = std::upper_bound(recomp_function_table, end, addr,
                                                     [](uint32_t a, const RecompFunctionEntry& e) { return a < e.addr; });
    if (it == recomp_function_table) return 0;
    --it;
    if (addr - it->addr > 0x10000) return 0;  // past the last function: not code we know
    *symbol = nullptr;
    return it->addr;
}

std::string describe(PsxContext* ctx, uint32_t addr) {
    const char* overlay = nullptr;
    const uint32_t entry = containing_function(ctx, addr, &overlay);
    if (!entry) return {};
    char buf[64];
    if (overlay) std::snprintf(buf, sizeof buf, "o_%s_%08X+0x%X", overlay, entry, addr - entry);
    else std::snprintf(buf, sizeof buf, "f_%08X+0x%X", entry, addr - entry);
    return buf;
}

}  // namespace

namespace dcb {

void register_code_names() { psx::set_describer(&describe); }

}  // namespace dcb
