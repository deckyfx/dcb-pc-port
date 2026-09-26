#pragma once
// MIPS R3000A instruction decoding (the PS1 CPU: MIPS I + COP0 + GTE as COP2).

#include <cstdint>
#include <string>

namespace recomp {

enum class Op : uint8_t {
    Invalid,
    // ALU
    Sll, Srl, Sra, Sllv, Srlv, Srav,
    Add, Addu, Sub, Subu, And, Or, Xor, Nor, Slt, Sltu,
    Addi, Addiu, Slti, Sltiu, Andi, Ori, Xori, Lui,
    Mult, Multu, Div, Divu, Mfhi, Mthi, Mflo, Mtlo,
    // Control flow
    J, Jal, Jr, Jalr,
    Beq, Bne, Blez, Bgtz, Bltz, Bgez, Bltzal, Bgezal,
    Syscall, Break,
    // Memory
    Lb, Lh, Lwl, Lw, Lbu, Lhu, Lwr, Sb, Sh, Swl, Sw, Swr, Lwc2, Swc2,
    // Coprocessors
    Mfc0, Mtc0, Rfe, Mfc2, Cfc2, Mtc2, Ctc2, Cop2Cmd,
};

/// One decoded instruction. Field meanings follow the MIPS encoding.
struct Instr {
    uint32_t raw = 0;
    uint32_t pc = 0;
    Op op = Op::Invalid;
    uint8_t rs = 0, rt = 0, rd = 0, sa = 0;
    uint16_t imm = 0;

    int32_t simm() const { return static_cast<int16_t>(imm); }
    /// Target of a PC-relative branch (relative to the delay slot).
    uint32_t branch_target() const { return pc + 4 + (static_cast<uint32_t>(simm()) << 2); }
    /// Target of j/jal (region of the delay slot).
    uint32_t jump_target() const { return ((pc + 4) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2); }
    uint32_t code20() const { return (raw >> 6) & 0xFFFFFu; }
};

Instr decode(uint32_t raw, uint32_t pc);

bool is_branch(Op op);           ///< conditional PC-relative branch (has delay slot)
bool is_jump(Op op);             ///< j/jal/jr/jalr (has delay slot)
inline bool has_delay_slot(Op op) { return is_branch(op) || is_jump(op); }
bool is_load(Op op);

/// Register number written by the instruction as a GPR destination, or -1.
int gpr_written(const Instr& in);
/// True if the instruction reads GPR `r`.
bool reads_gpr(const Instr& in, int r);

std::string disasm(const Instr& in);
const char* reg_name(int r);

}  // namespace recomp
