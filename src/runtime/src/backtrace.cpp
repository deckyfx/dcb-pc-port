// Guest call chains; see backtrace.hpp.

#include <psx/backtrace.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psx {

namespace {

PsxContext* g_active = nullptr;

uint32_t read_word(PsxContext* ctx, uint32_t addr) {
    const int32_t off = psx_ram_offset(addr);
    if (off < 0 || static_cast<uint32_t>(off) + 4 > PSX_RAM_SIZE || (addr & 3)) return 0;
    uint32_t w = 0;
    std::memcpy(&w, ctx->ram + off, 4);
    return w;
}

Describer g_describer = nullptr;

bool after_call(PsxContext* ctx, uint32_t ret) {
    const uint32_t insn = read_word(ctx, ret - 8);
    const uint32_t op = insn >> 26;
    return op == 3 /* jal */ || (op == 0 && (insn & 0x3F) == 9) /* jalr */;
}

}  // namespace

PsxContext* active_context() { return g_active; }
void set_active_context(PsxContext* ctx) { g_active = ctx; }

void set_describer(Describer describer) { g_describer = describer; }

std::string describe_address(PsxContext* ctx, uint32_t addr) { return g_describer ? g_describer(ctx, addr) : std::string(); }

std::vector<uint32_t> backtrace(PsxContext* ctx, size_t max) {
    std::vector<uint32_t> out;
    if (!ctx) return out;
    const auto add = [&](uint32_t ret) {
        if (out.size() >= max || !after_call(ctx, ret) || describe_address(ctx, ret).empty()) return;
        if (std::find(out.begin(), out.end(), ret) == out.end()) out.push_back(ret);
    };
    add(ctx->r[31]);
    const uint32_t sp = ctx->r[29];
    for (uint32_t off = 0; off < 16384 && out.size() < max; off += 4) add(read_word(ctx, sp + off));
    return out;
}

std::string backtrace_string(PsxContext* ctx, size_t max) {
    std::string s;
    for (const uint32_t ret : backtrace(ctx, max)) s += " <- " + describe_address(ctx, ret - 8);
    return s;
}

}  // namespace psx
