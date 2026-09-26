// Decoder tests against hand-checked R3000A encodings.

#include "mips.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace recomp;

static int failures = 0;

static void expect(uint32_t raw, uint32_t pc, const std::string& text) {
    const std::string got = disasm(decode(raw, pc));
    if (got != text) {
        std::fprintf(stderr, "%08X @%08X: expected '%s', got '%s'\n", raw, pc, text.c_str(), got.c_str());
        ++failures;
    }
}

int main() {
    expect(0x00000000, 0, "nop");
    expect(0x27BDFFE8, 0, "addiu sp, sp, -24");
    expect(0xAFBF0010, 0, "sw ra, 16(sp)");
    expect(0x03E00008, 0, "jr ra");
    expect(0x0C0163A2, 0x80010000, "jal 0x80058E88");
    expect(0x08004000, 0x80010000, "j 0x80010000");
    expect(0x1440FFFE, 0x80010000, "bne v0, zero, 0x8000FFFC");
    expect(0x04110003, 0x80010000, "bgezal zero, 0x80010010");
    expect(0x3C028007, 0, "lui v0, 0x8007");
    expect(0x8C42A5E8, 0, "lw v0, -23064(v0)");
    expect(0x0043102B, 0, "sltu v0, v0, v1");
    expect(0x00031080, 0, "sll v0, v1, 2");
    expect(0x0000000C, 0, "syscall 0x0");
    expect(0x0007000D, 0, "break 0x1C00");
    expect(0x48020800, 0, "mfc2 v0, $1");
    expect(0x4AE80030, 0, "cop2 0x0E80030");
    expect(0x42000010, 0, "rfe");
    expect(0xC8800000, 0, "lwc2 $0, 0(a0)");
    expect(0x7C000000, 0, ".word 0x7C000000");  // not an R3000A opcode
    expect(0x44000000, 0, ".word 0x44000000");  // COP1 does not exist on the PS1

    const Instr jal = decode(0x0C0163A2, 0x80010000);
    if (gpr_written(jal) != 31 || !has_delay_slot(jal.op)) { std::fprintf(stderr, "jal semantics\n"); ++failures; }
    const Instr lwl = decode(0x88820003, 0);  // lwl v0, 3(a0): reads both a0 and the old v0
    if (!reads_gpr(lwl, 2) || !reads_gpr(lwl, 4)) { std::fprintf(stderr, "lwl reads\n"); ++failures; }

    if (failures) return EXIT_FAILURE;
    std::puts("recomp.decoder: ok");
    return 0;
}
