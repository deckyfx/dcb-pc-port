#include "mips.hpp"

#include <array>
#include <cstdio>

namespace recomp {

namespace {

Op decode_special(uint32_t funct) {
    switch (funct) {
        case 0x00: return Op::Sll;   case 0x02: return Op::Srl;   case 0x03: return Op::Sra;
        case 0x04: return Op::Sllv;  case 0x06: return Op::Srlv;  case 0x07: return Op::Srav;
        case 0x08: return Op::Jr;    case 0x09: return Op::Jalr;
        case 0x0C: return Op::Syscall; case 0x0D: return Op::Break;
        case 0x10: return Op::Mfhi;  case 0x11: return Op::Mthi;  case 0x12: return Op::Mflo;  case 0x13: return Op::Mtlo;
        case 0x18: return Op::Mult;  case 0x19: return Op::Multu; case 0x1A: return Op::Div;   case 0x1B: return Op::Divu;
        case 0x20: return Op::Add;   case 0x21: return Op::Addu;  case 0x22: return Op::Sub;   case 0x23: return Op::Subu;
        case 0x24: return Op::And;   case 0x25: return Op::Or;    case 0x26: return Op::Xor;   case 0x27: return Op::Nor;
        case 0x2A: return Op::Slt;   case 0x2B: return Op::Sltu;
        default:   return Op::Invalid;
    }
}

Op decode_primary(uint32_t opcode) {
    switch (opcode) {
        case 0x02: return Op::J;     case 0x03: return Op::Jal;
        case 0x04: return Op::Beq;   case 0x05: return Op::Bne;   case 0x06: return Op::Blez;  case 0x07: return Op::Bgtz;
        case 0x08: return Op::Addi;  case 0x09: return Op::Addiu; case 0x0A: return Op::Slti;  case 0x0B: return Op::Sltiu;
        case 0x0C: return Op::Andi;  case 0x0D: return Op::Ori;   case 0x0E: return Op::Xori;  case 0x0F: return Op::Lui;
        case 0x20: return Op::Lb;    case 0x21: return Op::Lh;    case 0x22: return Op::Lwl;   case 0x23: return Op::Lw;
        case 0x24: return Op::Lbu;   case 0x25: return Op::Lhu;   case 0x26: return Op::Lwr;
        case 0x28: return Op::Sb;    case 0x29: return Op::Sh;    case 0x2A: return Op::Swl;   case 0x2B: return Op::Sw;
        case 0x2E: return Op::Swr;   case 0x32: return Op::Lwc2;  case 0x3A: return Op::Swc2;
        default:   return Op::Invalid;
    }
}

}  // namespace

Instr decode(uint32_t raw, uint32_t pc) {
    Instr in;
    in.raw = raw;
    in.pc = pc;
    in.rs = static_cast<uint8_t>((raw >> 21) & 31);
    in.rt = static_cast<uint8_t>((raw >> 16) & 31);
    in.rd = static_cast<uint8_t>((raw >> 11) & 31);
    in.sa = static_cast<uint8_t>((raw >> 6) & 31);
    in.imm = static_cast<uint16_t>(raw & 0xFFFF);

    const uint32_t opcode = raw >> 26;
    switch (opcode) {
        case 0x00:
            in.op = decode_special(raw & 0x3F);
            // Reserved fields must be zero for the few forms where the hardware checks nothing,
            // but compilers never emit non-zero ones; treat those as data to reject bad seeds.
            if (in.op == Op::Jr && (raw & 0x001FFFC0u)) in.op = Op::Invalid;
            break;
        case 0x01:
            switch (in.rt) {
                case 0x00: in.op = Op::Bltz; break;
                case 0x01: in.op = Op::Bgez; break;
                case 0x10: in.op = Op::Bltzal; break;
                case 0x11: in.op = Op::Bgezal; break;
                default:   in.op = Op::Invalid; break;
            }
            break;
        case 0x10:  // COP0
            if (in.rs == 0x00) in.op = Op::Mfc0;
            else if (in.rs == 0x04) in.op = Op::Mtc0;
            else if (in.rs == 0x10 && (raw & 0x3F) == 0x10) in.op = Op::Rfe;
            break;
        case 0x12:  // COP2 (GTE)
            if (raw & (1u << 25)) in.op = Op::Cop2Cmd;
            else if (in.rs == 0x00) in.op = Op::Mfc2;
            else if (in.rs == 0x02) in.op = Op::Cfc2;
            else if (in.rs == 0x04) in.op = Op::Mtc2;
            else if (in.rs == 0x06) in.op = Op::Ctc2;
            break;
        default:
            in.op = decode_primary(opcode);
            break;
    }
    return in;
}

bool is_branch(Op op) {
    switch (op) {
        case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz:
        case Op::Bltz: case Op::Bgez: case Op::Bltzal: case Op::Bgezal:
            return true;
        default:
            return false;
    }
}

bool is_jump(Op op) { return op == Op::J || op == Op::Jal || op == Op::Jr || op == Op::Jalr; }

bool is_load(Op op) {
    switch (op) {
        case Op::Lb: case Op::Lh: case Op::Lwl: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Lwr:
            return true;
        default:
            return false;
    }
}

int gpr_written(const Instr& in) {
    switch (in.op) {
        case Op::Sll: case Op::Srl: case Op::Sra: case Op::Sllv: case Op::Srlv: case Op::Srav:
        case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu: case Op::And: case Op::Or:
        case Op::Xor: case Op::Nor: case Op::Slt: case Op::Sltu: case Op::Mfhi: case Op::Mflo: case Op::Jalr:
            return in.rd;
        case Op::Addi: case Op::Addiu: case Op::Slti: case Op::Sltiu: case Op::Andi: case Op::Ori:
        case Op::Xori: case Op::Lui: case Op::Lb: case Op::Lh: case Op::Lwl: case Op::Lw: case Op::Lbu:
        case Op::Lhu: case Op::Lwr: case Op::Mfc0: case Op::Mfc2: case Op::Cfc2:
            return in.rt;
        case Op::Jal: case Op::Bltzal: case Op::Bgezal:
            return 31;
        default:
            return -1;
    }
}

bool reads_gpr(const Instr& in, int r) {
    if (r == 0) return false;
    switch (in.op) {
        case Op::Sll: case Op::Srl: case Op::Sra:
            return in.rt == r;
        case Op::Sllv: case Op::Srlv: case Op::Srav: case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu:
        case Op::And: case Op::Or: case Op::Xor: case Op::Nor: case Op::Slt: case Op::Sltu:
        case Op::Mult: case Op::Multu: case Op::Div: case Op::Divu: case Op::Beq: case Op::Bne:
        case Op::Sb: case Op::Sh: case Op::Swl: case Op::Sw: case Op::Swr:
            return in.rs == r || in.rt == r;
        case Op::Lwl: case Op::Lwr:  // merge into the old value of rt
            return in.rs == r || in.rt == r;
        case Op::Addi: case Op::Addiu: case Op::Slti: case Op::Sltiu: case Op::Andi: case Op::Ori: case Op::Xori:
        case Op::Blez: case Op::Bgtz: case Op::Bltz: case Op::Bgez: case Op::Bltzal: case Op::Bgezal:
        case Op::Jr: case Op::Jalr: case Op::Mthi: case Op::Mtlo:
        case Op::Lb: case Op::Lh: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Lwc2: case Op::Swc2:
            return in.rs == r;
        case Op::Mtc0: case Op::Mtc2: case Op::Ctc2:
            return in.rt == r;
        default:
            return false;
    }
}

const char* reg_name(int r) {
    static constexpr std::array<const char*, 32> names = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return names[static_cast<size_t>(r & 31)];
}

std::string disasm(const Instr& in) {
    char buf[96];
    const char* s = reg_name(in.rs);
    const char* t = reg_name(in.rt);
    const char* d = reg_name(in.rd);
    switch (in.op) {
        case Op::Sll:
            if (in.raw == 0) return "nop";
            std::snprintf(buf, sizeof buf, "sll %s, %s, %u", d, t, in.sa); break;
        case Op::Srl:  std::snprintf(buf, sizeof buf, "srl %s, %s, %u", d, t, in.sa); break;
        case Op::Sra:  std::snprintf(buf, sizeof buf, "sra %s, %s, %u", d, t, in.sa); break;
        case Op::Sllv: std::snprintf(buf, sizeof buf, "sllv %s, %s, %s", d, t, s); break;
        case Op::Srlv: std::snprintf(buf, sizeof buf, "srlv %s, %s, %s", d, t, s); break;
        case Op::Srav: std::snprintf(buf, sizeof buf, "srav %s, %s, %s", d, t, s); break;
        case Op::Jr:   std::snprintf(buf, sizeof buf, "jr %s", s); break;
        case Op::Jalr: std::snprintf(buf, sizeof buf, "jalr %s, %s", d, s); break;
        case Op::Syscall: std::snprintf(buf, sizeof buf, "syscall 0x%X", in.code20()); break;
        case Op::Break:   std::snprintf(buf, sizeof buf, "break 0x%X", in.code20()); break;
        case Op::Mfhi: std::snprintf(buf, sizeof buf, "mfhi %s", d); break;
        case Op::Mthi: std::snprintf(buf, sizeof buf, "mthi %s", s); break;
        case Op::Mflo: std::snprintf(buf, sizeof buf, "mflo %s", d); break;
        case Op::Mtlo: std::snprintf(buf, sizeof buf, "mtlo %s", s); break;
        case Op::Mult:  std::snprintf(buf, sizeof buf, "mult %s, %s", s, t); break;
        case Op::Multu: std::snprintf(buf, sizeof buf, "multu %s, %s", s, t); break;
        case Op::Div:   std::snprintf(buf, sizeof buf, "div %s, %s", s, t); break;
        case Op::Divu:  std::snprintf(buf, sizeof buf, "divu %s, %s", s, t); break;
        case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu: case Op::And: case Op::Or:
        case Op::Xor: case Op::Nor: case Op::Slt: case Op::Sltu: {
            static constexpr const char* n[] = {"add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"};
            std::snprintf(buf, sizeof buf, "%s %s, %s, %s", n[static_cast<int>(in.op) - static_cast<int>(Op::Add)], d, s, t);
            break;
        }
        case Op::Addi: case Op::Addiu: case Op::Slti: case Op::Sltiu: {
            static constexpr const char* n[] = {"addi", "addiu", "slti", "sltiu"};
            std::snprintf(buf, sizeof buf, "%s %s, %s, %d", n[static_cast<int>(in.op) - static_cast<int>(Op::Addi)], t, s, in.simm());
            break;
        }
        case Op::Andi: std::snprintf(buf, sizeof buf, "andi %s, %s, 0x%X", t, s, in.imm); break;
        case Op::Ori:  std::snprintf(buf, sizeof buf, "ori %s, %s, 0x%X", t, s, in.imm); break;
        case Op::Xori: std::snprintf(buf, sizeof buf, "xori %s, %s, 0x%X", t, s, in.imm); break;
        case Op::Lui:  std::snprintf(buf, sizeof buf, "lui %s, 0x%X", t, in.imm); break;
        case Op::J:    std::snprintf(buf, sizeof buf, "j 0x%08X", in.jump_target()); break;
        case Op::Jal:  std::snprintf(buf, sizeof buf, "jal 0x%08X", in.jump_target()); break;
        case Op::Beq:  std::snprintf(buf, sizeof buf, "beq %s, %s, 0x%08X", s, t, in.branch_target()); break;
        case Op::Bne:  std::snprintf(buf, sizeof buf, "bne %s, %s, 0x%08X", s, t, in.branch_target()); break;
        case Op::Blez: std::snprintf(buf, sizeof buf, "blez %s, 0x%08X", s, in.branch_target()); break;
        case Op::Bgtz: std::snprintf(buf, sizeof buf, "bgtz %s, 0x%08X", s, in.branch_target()); break;
        case Op::Bltz: std::snprintf(buf, sizeof buf, "bltz %s, 0x%08X", s, in.branch_target()); break;
        case Op::Bgez: std::snprintf(buf, sizeof buf, "bgez %s, 0x%08X", s, in.branch_target()); break;
        case Op::Bltzal: std::snprintf(buf, sizeof buf, "bltzal %s, 0x%08X", s, in.branch_target()); break;
        case Op::Bgezal: std::snprintf(buf, sizeof buf, "bgezal %s, 0x%08X", s, in.branch_target()); break;
        case Op::Lb: case Op::Lh: case Op::Lwl: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Lwr:
        case Op::Sb: case Op::Sh: case Op::Swl: case Op::Sw: case Op::Swr: {
            static constexpr const char* n[] = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "sb", "sh", "swl", "sw", "swr"};
            std::snprintf(buf, sizeof buf, "%s %s, %d(%s)", n[static_cast<int>(in.op) - static_cast<int>(Op::Lb)], t, in.simm(), s);
            break;
        }
        case Op::Lwc2: std::snprintf(buf, sizeof buf, "lwc2 $%u, %d(%s)", in.rt, in.simm(), s); break;
        case Op::Swc2: std::snprintf(buf, sizeof buf, "swc2 $%u, %d(%s)", in.rt, in.simm(), s); break;
        case Op::Mfc0: std::snprintf(buf, sizeof buf, "mfc0 %s, $%u", t, in.rd); break;
        case Op::Mtc0: std::snprintf(buf, sizeof buf, "mtc0 %s, $%u", t, in.rd); break;
        case Op::Rfe:  return "rfe";
        case Op::Mfc2: std::snprintf(buf, sizeof buf, "mfc2 %s, $%u", t, in.rd); break;
        case Op::Cfc2: std::snprintf(buf, sizeof buf, "cfc2 %s, $%u", t, in.rd); break;
        case Op::Mtc2: std::snprintf(buf, sizeof buf, "mtc2 %s, $%u", t, in.rd); break;
        case Op::Ctc2: std::snprintf(buf, sizeof buf, "ctc2 %s, $%u", t, in.rd); break;
        case Op::Cop2Cmd: std::snprintf(buf, sizeof buf, "cop2 0x%07X", in.raw & 0x1FFFFFF); break;
        case Op::Invalid: std::snprintf(buf, sizeof buf, ".word 0x%08X", in.raw); break;
    }
    return buf;
}

}  // namespace recomp
