// RAM write watch (DCB_WATCH): log the game's writes into chosen RAM ranges, with the value
// before and after, the function that wrote it and the guest call chain. The tool for "where
// does the game keep X": watch a region, do X in the game, read which function changed what.
//
//   DCB_WATCH=800E0000-800E1800          one range (inclusive start, exclusive end)
//   DCB_WATCH=800E036A+2,801DAF40+0x200  address+length, several ranges
//
// Writes that store the value already there are skipped, and each address logs at most
// kMaxPerAddress changes (per-frame counters would flood the log otherwise). The writer is the
// recompiled function the store is in (f_8003C0B0 / o_KAWSEG_801ED334), found from the host
// return address; the chain after it is psx::backtrace (callers). Only guest stores are seen:
// data the host copies into RAM (file reads, DMA) is not.

#include <psx/backtrace.hpp>
#include <psx/recomp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

extern "C" {
uint32_t psx_watch_lo = 0, psx_watch_size = 0;
uint64_t psx_watch_frame = 0;
}

namespace {

constexpr int kMaxPerAddress = 8;

struct Range {
    uint32_t lo, hi;  // RAM offsets, [lo, hi)
};
std::vector<Range> g_ranges;
std::unordered_map<uint32_t, int> g_counts;  // RAM offset -> changes logged

/// Name of the host function containing `pc` if it is a recompiled one (f_XXXXXXXX,
/// o_OVERLAY_XXXXXXXX), else "".
std::string recompiled_name(void* pc) {
#if defined(__linux__) || defined(__APPLE__)
    Dl_info info{};
    if (pc && dladdr(pc, &info) && info.dli_sname &&
        (std::strncmp(info.dli_sname, "f_", 2) == 0 || std::strncmp(info.dli_sname, "o_", 2) == 0))
        return info.dli_sname;
#else
    (void)pc;
#endif
    return {};
}

bool parse(const char* spec) {
    std::string text(spec);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        if (item.empty()) continue;
        char* rest = nullptr;
        const unsigned long a = std::strtoul(item.c_str(), &rest, 16);
        unsigned long b = a + 1;
        if (*rest == '-') b = std::strtoul(rest + 1, &rest, 16);
        else if (*rest == '+') b = a + std::strtoul(rest + 1, &rest, 0);
        const int32_t lo = psx_ram_offset(static_cast<uint32_t>(a));
        if (*rest != 0 || lo < 0 || b <= a || b - a > PSX_RAM_SIZE) {
            std::fprintf(stderr, "[watch] bad range \"%s\" (use 800E0000-800E1800 or 800E036A+2)\n", item.c_str());
            return false;
        }
        g_ranges.push_back({static_cast<uint32_t>(lo), static_cast<uint32_t>(lo) + static_cast<uint32_t>(b - a)});
    }
    return !g_ranges.empty();
}

struct Setup {
    Setup() {
        const char* spec = std::getenv("DCB_WATCH");
        if (spec && *spec && psx_watch_configure(spec))
            std::fprintf(stderr, "[watch] %zu range(s), RAM %06X-%06X\n", g_ranges.size(), psx_watch_lo,
                         psx_watch_lo + psx_watch_size);
    }
} g_setup;

}  // namespace

extern "C" int psx_watch_configure(const char* spec) {
    g_ranges.clear();
    g_counts.clear();
    psx_watch_lo = psx_watch_size = 0;
    if (!spec || !parse(spec)) return 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const Range& r : g_ranges) {
        if (r.lo < lo) lo = r.lo;
        if (r.hi > hi) hi = r.hi;
    }
    psx_watch_lo = lo;
    psx_watch_size = hi - lo;
    return 1;
}

extern "C" void psx_watch_hit(PsxContext* ctx, uint32_t off, uint32_t value, int bytes) {
    // The recompiled function doing the store: the caller of this function once the write helper
    // is inlined (optimized builds), one frame further up when it is not (-O0 debug builds,
    // which keep frame pointers, so walking one more frame is safe).
    std::string writer = recompiled_name(__builtin_return_address(0));
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
    if (writer.empty()) writer = recompiled_name(__builtin_return_address(1));
#pragma GCC diagnostic pop
#endif
    if (writer.empty()) writer = "?";
    for (int i = 0; i < bytes; ++i) {        // log per watched byte span, once per store
        bool inside = false;
        for (const Range& r : g_ranges)
            if (off + static_cast<uint32_t>(i) >= r.lo && off + static_cast<uint32_t>(i) < r.hi) inside = true;
        if (!inside) continue;
        uint32_t old = 0;
        std::memcpy(&old, ctx->ram + off, static_cast<size_t>(bytes));
        if (old == value) return;
        int& n = g_counts[off];
        if (n > kMaxPerAddress) return;
        if (n++ == kMaxPerAddress) {
            std::fprintf(stderr, "[watch] %08X: more changes, not logged\n", 0x80000000u | off);
            return;
        }
        const int digits = bytes * 2;
        std::fprintf(stderr, "[watch] frame %llu write%d %08X: %0*X -> %0*X  by %s%s\n",
                     static_cast<unsigned long long>(psx_watch_frame), bytes * 8, 0x80000000u | off, digits, old,
                     digits, value, writer.c_str(), psx::backtrace_string(ctx).c_str());
        return;
    }
}
