#include "emit.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace recomp {

namespace fs = std::filesystem;

namespace {

constexpr size_t kFunctionsPerFile = 150;

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08Xu", v);
    return b;
}

std::string reg(int r) { return "ctx->r[" + std::to_string(r) + "]"; }

class FunctionEmitter {
public:
    FunctionEmitter(const Program& prog, const Analysis& an, size_t seg_idx, const Function& fn, std::ostream& out)
        : prog_(prog), an_(an), seg_idx_(seg_idx), fn_(fn), seg_(*fn.seg), out_(out) {}

    size_t emit() {
        collect_labels();
        out_ << "void " << function_symbol(seg_, fn_.entry) << "(PsxContext* ctx) {\n";
        out_ << "    PSX_FUNCTION_PROLOGUE(ctx);\n";
        // `jr <reg>` other than ra may be a return through a saved copy of ra (libgcc's soft-float
        // helpers do `move t8,ra; jal ...; jr t8`): remember where this function was called from.
        for (uint32_t pc : fn_.instrs) {
            const Instr in = seg_.instr(pc);
            if (in.op == Op::Jr && in.rs != 31) {
                out_ << "    const uint32_t psx_entry_ra = ctx->r[31];\n";
                break;
            }
        }
        size_t count = 0;
        uint32_t skip = 0;       // delay slot already emitted inside its branch
        bool owner_falls = false;  // that branch can continue past its delay slot
        for (uint32_t pc : fn_.instrs) {
            if (pc == skip) {
                if (labels_.count(pc)) {
                    // Something jumps straight into a delay slot: give it a standalone copy.
                    if (owner_falls) {
                        if (fn_.instrs.count(pc + 4)) out_ << "    goto L_" << hexlabel(pc + 4) << ";\n";
                        else out_ << "    psx_dispatch(ctx, " << hex32(pc + 4) << "); return;\n";
                    }
                    label(pc);
                    body(seg_.instr(pc));
                    if (!fn_.instrs.count(pc + 4)) out_ << "    psx_dispatch(ctx, " << hex32(pc + 4) << "); return;\n";
                }
                continue;
            }
            if (labels_.count(pc)) label(pc);
            const Instr in = seg_.instr(pc);
            if (has_delay_slot(in.op)) {
                control(in);
                skip = pc + 4;
                owner_falls = is_branch(in.op) || in.op == Op::Jal || in.op == Op::Jalr;
            } else {
                body(in);
            }
            ++count;
        }
        // Labels whose code could not be decoded: trap loudly instead of running garbage.
        for (uint32_t target : labels_) {
            if (!fn_.instrs.count(target)) {
                label(target);
                out_ << "    psx_invalid(ctx, " << hex32(target) << "); return;\n";
            }
        }
        out_ << "}\n\n";
        return count;
    }

private:
    const Program& prog_;
    const Analysis& an_;
    size_t seg_idx_;
    const Function& fn_;
    const Segment& seg_;
    std::ostream& out_;
    std::set<uint32_t> labels_;

    static std::string hexlabel(uint32_t a) {
        char b[12];
        std::snprintf(b, sizeof b, "%08X", a);
        return b;
    }

    void label(uint32_t pc) { out_ << "L_" << hexlabel(pc) << ":;\n"; }

    /// Loop back-edges charge the loop body's cycles (~2 per instruction, a typical R3000A CPI
    /// with load/cache stalls) and poll for interrupts: spin-waits must let VBLANK etc. happen.
    static std::string back_edge(const Instr& in, uint32_t target) {
        if (target > in.pc) return "";
        const uint32_t instructions = (in.pc - target) / 4 + 2;  // body + branch + delay slot
        return "PSX_POLL(ctx, " + std::to_string(2 * instructions) + "); ";
    }

    void collect_labels() {
        for (uint32_t pc : fn_.instrs) {
            const Instr in = seg_.instr(pc);
            if (is_branch(in.op) && in.op != Op::Bltzal && in.op != Op::Bgezal) labels_.insert(in.branch_target());
            if (in.op == Op::J && fn_.instrs.count(in.jump_target())) labels_.insert(in.jump_target());
        }
        for (const auto& [pc, jt] : fn_.tables) labels_.insert(jt.targets.begin(), jt.targets.end());
        // A delay slot that is also a jump target needs its fall-through successor labelled.
        for (uint32_t pc : fn_.instrs) {
            const Instr in = seg_.instr(pc);
            if (has_delay_slot(in.op) && labels_.count(pc + 4) && fn_.instrs.count(pc + 8)) labels_.insert(pc + 8);
        }
    }

    void line(const Instr& in, const std::string& code) {
        out_ << "    " << code << "  /* " << hexlabel(in.pc) << ": " << disasm(in) << " */\n";
    }

    std::string imm_s(const Instr& in) const { return hex32(static_cast<uint32_t>(in.simm())); }
    std::string addr(const Instr& in) const { return reg(in.rs) + " + " + imm_s(in); }

    /// Guest call to a statically known target.
    std::string call(uint32_t target) const {
        if (seg_.overlay) {
            if (an_.find(seg_idx_, target)) return function_symbol(seg_, target) + "(ctx);";
        }
        if (const auto it = prog_.overrides.find(target); it != prog_.overrides.end()) return it->second + "(ctx);";
        if (prog_.main().in_code(target) && an_.find(0, target)) return function_symbol(prog_.main(), target) + "(ctx);";
        return "psx_dispatch(ctx, " + hex32(target) + ");";
    }

    void assign(const Instr& in, int rd, const std::string& expr) {
        if (rd == 0) line(in, "/* write to $zero discarded */");
        else line(in, reg(rd) + " = " + expr + ";");
    }

    void body(const Instr& in) {
        const std::string s = reg(in.rs), t = reg(in.rt);
        switch (in.op) {
            case Op::Sll:  if (in.raw == 0) { line(in, ";"); break; }
                           assign(in, in.rd, t + " << " + std::to_string(in.sa)); break;
            case Op::Srl:  assign(in, in.rd, t + " >> " + std::to_string(in.sa)); break;
            case Op::Sra:  assign(in, in.rd, "(uint32_t)((int32_t)" + t + " >> " + std::to_string(in.sa) + ")"); break;
            case Op::Sllv: assign(in, in.rd, t + " << (" + s + " & 31)"); break;
            case Op::Srlv: assign(in, in.rd, t + " >> (" + s + " & 31)"); break;
            case Op::Srav: assign(in, in.rd, "(uint32_t)((int32_t)" + t + " >> (" + s + " & 31))"); break;
            // add/addi overflow traps are not modelled: Psy-Q code does not rely on them.
            case Op::Add: case Op::Addu: assign(in, in.rd, s + " + " + t); break;
            case Op::Sub: case Op::Subu: assign(in, in.rd, s + " - " + t); break;
            case Op::And:  assign(in, in.rd, s + " & " + t); break;
            case Op::Or:   assign(in, in.rd, s + " | " + t); break;
            case Op::Xor:  assign(in, in.rd, s + " ^ " + t); break;
            case Op::Nor:  assign(in, in.rd, "~(" + s + " | " + t + ")"); break;
            case Op::Slt:  assign(in, in.rd, "(int32_t)" + s + " < (int32_t)" + t); break;
            case Op::Sltu: assign(in, in.rd, s + " < " + t); break;
            case Op::Addi: case Op::Addiu: assign(in, in.rt, s + " + " + imm_s(in)); break;
            case Op::Slti:  assign(in, in.rt, "(int32_t)" + s + " < " + std::to_string(in.simm())); break;
            case Op::Sltiu: assign(in, in.rt, s + " < " + imm_s(in)); break;
            case Op::Andi: assign(in, in.rt, s + " & " + hex32(in.imm)); break;
            case Op::Ori:  assign(in, in.rt, s + " | " + hex32(in.imm)); break;
            case Op::Xori: assign(in, in.rt, s + " ^ " + hex32(in.imm)); break;
            case Op::Lui:  assign(in, in.rt, hex32(static_cast<uint32_t>(in.imm) << 16)); break;
            case Op::Mult:  line(in, "psx_mult(ctx, " + s + ", " + t + ");"); break;
            case Op::Multu: line(in, "psx_multu(ctx, " + s + ", " + t + ");"); break;
            case Op::Div:   line(in, "psx_div(ctx, " + s + ", " + t + ");"); break;
            case Op::Divu:  line(in, "psx_divu(ctx, " + s + ", " + t + ");"); break;
            case Op::Mfhi: assign(in, in.rd, "ctx->hi"); break;
            case Op::Mflo: assign(in, in.rd, "ctx->lo"); break;
            case Op::Mthi: line(in, "ctx->hi = " + s + ";"); break;
            case Op::Mtlo: line(in, "ctx->lo = " + s + ";"); break;
            case Op::Lb:  load(in, "(uint32_t)(int32_t)(int8_t)psx_read8"); break;
            case Op::Lbu: load(in, "(uint32_t)psx_read8"); break;
            case Op::Lh:  load(in, "(uint32_t)(int32_t)(int16_t)psx_read16"); break;
            case Op::Lhu: load(in, "(uint32_t)psx_read16"); break;
            case Op::Lw:  load(in, "psx_read32"); break;
            case Op::Lwl: assign(in, in.rt, "psx_lwl(ctx, " + addr(in) + ", " + t + ")"); break;
            case Op::Lwr: assign(in, in.rt, "psx_lwr(ctx, " + addr(in) + ", " + t + ")"); break;
            case Op::Sb:  line(in, "psx_write8(ctx, " + addr(in) + ", (uint8_t)" + t + ");"); break;
            case Op::Sh:  line(in, "psx_write16(ctx, " + addr(in) + ", (uint16_t)" + t + ");"); break;
            case Op::Sw:  line(in, "psx_write32(ctx, " + addr(in) + ", " + t + ");"); break;
            case Op::Swl: line(in, "psx_swl(ctx, " + addr(in) + ", " + t + ");"); break;
            case Op::Swr: line(in, "psx_swr(ctx, " + addr(in) + ", " + t + ");"); break;
            case Op::Lwc2: line(in, "psx_gte_write_data(ctx, " + std::to_string(in.rt) + ", psx_read32(ctx, " + addr(in) + "));"); break;
            case Op::Swc2: line(in, "psx_write32(ctx, " + addr(in) + ", psx_gte_read_data(ctx, " + std::to_string(in.rt) + "));"); break;
            case Op::Mfc0: assign(in, in.rt, "ctx->cop0[" + std::to_string(in.rd) + "]"); break;
            case Op::Mtc0: line(in, "ctx->cop0[" + std::to_string(in.rd) + "] = " + t + ";"); break;
            case Op::Rfe:  line(in, "psx_rfe(ctx);"); break;
            case Op::Mfc2: assign(in, in.rt, "psx_gte_read_data(ctx, " + std::to_string(in.rd) + ")"); break;
            case Op::Cfc2: assign(in, in.rt, "psx_gte_read_ctrl(ctx, " + std::to_string(in.rd) + ")"); break;
            case Op::Mtc2: line(in, "psx_gte_write_data(ctx, " + std::to_string(in.rd) + ", " + t + ");"); break;
            case Op::Ctc2: line(in, "psx_gte_write_ctrl(ctx, " + std::to_string(in.rd) + ", " + t + ");"); break;
            case Op::Cop2Cmd: line(in, "psx_gte_command(ctx, " + hex32(in.raw & 0x1FFFFFFu) + ");"); break;
            case Op::Syscall: line(in, "ctx->pc = " + hex32(in.pc) + "; psx_syscall(ctx, " + hex32(in.code20()) + ");"); break;
            case Op::Break:   line(in, "ctx->pc = " + hex32(in.pc) + "; psx_break(ctx, " + hex32(in.code20()) + ");"); break;
            default:
                line(in, "psx_invalid(ctx, " + hex32(in.pc) + ");");
                break;
        }
    }

    void load(const Instr& in, const std::string& fn) {
        const std::string e = fn + "(ctx, " + addr(in) + ")";
        if (in.rt == 0) line(in, "(void)" + e + ";");
        else line(in, reg(in.rt) + " = " + e + ";");
    }

    void control(const Instr& in) {
        const Instr slot = seg_.instr(in.pc + 4);
        const std::string s = reg(in.rs), t = reg(in.rt);
        auto delay = [&] { body(slot); };
        std::string cond;
        switch (in.op) {
            case Op::Beq:  cond = s + " == " + t; break;
            case Op::Bne:  cond = s + " != " + t; break;
            case Op::Blez: cond = "(int32_t)" + s + " <= 0"; break;
            case Op::Bgtz: cond = "(int32_t)" + s + " > 0"; break;
            case Op::Bltz: case Op::Bltzal: cond = "(int32_t)" + s + " < 0"; break;
            case Op::Bgez: case Op::Bgezal: cond = "(int32_t)" + s + " >= 0"; break;
            default: break;
        }
        switch (in.op) {
            case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz: case Op::Bltz: case Op::Bgez:
                out_ << "    { const int c = " << cond << ";  /* " << hexlabel(in.pc) << ": " << disasm(in) << " */\n";
                delay();
                out_ << "    if (c) { " << back_edge(in, in.branch_target()) << "goto L_" << hexlabel(in.branch_target()) << "; } }\n";
                break;
            case Op::Bltzal: case Op::Bgezal:
                out_ << "    { const int c = " << cond << "; " << reg(31) << " = " << hex32(in.pc + 8)
                     << ";  /* " << hexlabel(in.pc) << ": " << disasm(in) << " */\n";
                delay();
                out_ << "    if (c) " << call(in.branch_target()) << " }\n";
                break;
            case Op::Jal:
                line(in, reg(31) + " = " + hex32(in.pc + 8) + ";");
                delay();
                out_ << "    " << call(in.jump_target()) << "\n";
                break;
            case Op::J:
                line(in, "/* jump */");
                delay();
                if (fn_.instrs.count(in.jump_target()))
                    out_ << "    { " << back_edge(in, in.jump_target()) << "goto L_" << hexlabel(in.jump_target()) << "; }\n";
                else out_ << "    " << call(in.jump_target()) << " return;  /* tail call */\n";
                break;
            case Op::Jr:
                if (in.rs == 31) {
                    line(in, "/* return */");
                    delay();
                    out_ << "    PSX_FUNCTION_RETURN(ctx); return;\n";
                } else {
                    out_ << "    { const uint32_t target = " << s << ";  /* " << hexlabel(in.pc) << ": " << disasm(in) << " */\n";
                    delay();
                    const auto it = fn_.tables.find(in.pc);
                    if (it != fn_.tables.end()) {
                        out_ << "    switch (target) {\n";
                        std::set<uint32_t> seen;
                        for (uint32_t tgt : it->second.targets) {
                            if (seen.insert(tgt).second)
                                out_ << "        case " << hex32(tgt) << ": goto L_" << hexlabel(tgt) << ";\n";
                        }
                        out_ << "        default: break;\n    }\n";
                    }
                    out_ << "    if (target == psx_entry_ra) return;  /* a return through a copy of ra */\n";
                    out_ << "    psx_dispatch(ctx, target); return; }\n";
                }
                break;
            case Op::Jalr:
                out_ << "    { const uint32_t target = " << s << "; ";
                if (in.rd != 0) out_ << reg(in.rd) << " = " << hex32(in.pc + 8) << ";";
                out_ << "  /* " << hexlabel(in.pc) << ": " << disasm(in) << " */\n";
                delay();
                out_ << "    psx_dispatch(ctx, target); }\n";
                break;
            default:
                throw std::logic_error("control(): not a control-flow instruction");
        }
    }
};

void write_if_changed(const fs::path& path, const std::string& text) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream old;
            old << in.rdbuf();
            if (old.str() == text) return;  // keep mtime: avoids needless rebuilds
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << text;
}

std::string file_header() {
    return "/* Generated by dcb_recompiler. Do not edit: override functions in src/game/overrides. */\n"
           "#include <psx/recomp.h>\n#include \"recomp_funcs.h\"\n\n";
}

}  // namespace

std::string function_symbol(const Segment& seg, uint32_t entry) {
    char b[64];
    if (seg.overlay) std::snprintf(b, sizeof b, "o_%s_%08X", seg.name.c_str(), entry);
    else std::snprintf(b, sizeof b, "f_%08X", entry);
    return b;
}

EmitStats emit_program(const Program& prog, const Analysis& analysis, const fs::path& out_dir) {
    fs::create_directories(out_dir);
    EmitStats stats;
    std::set<fs::path> written;

    std::ostringstream decls;
    decls << "/* Generated by dcb_recompiler. */\n#pragma once\n#include <psx/recomp.h>\n\n";

    for (size_t s = 0; s < prog.segments.size(); ++s) {
        const Segment& seg = prog.segments[s];
        size_t chunk = 0, in_chunk = 0;
        std::ostringstream body;
        auto flush = [&] {
            if (!in_chunk) return;
            char name[64];
            std::snprintf(name, sizeof name, "%s_%03zu.c", seg.overlay ? seg.name.c_str() : "main", chunk++);
            write_if_changed(out_dir / name, file_header() + body.str());
            written.insert(out_dir / name);
            body.str({});
            in_chunk = 0;
            ++stats.files;
        };
        for (const auto& [entry, fn] : analysis.functions[s]) {
            decls << "void " << function_symbol(seg, entry) << "(PsxContext* ctx);\n";
            stats.instructions += FunctionEmitter(prog, analysis, s, fn, body).emit();
            ++stats.functions;
            if (++in_chunk == kFunctionsPerFile) flush();
        }
        flush();
    }
    if (!prog.overrides.empty()) {
        decls << "\n/* Native overrides (src/game/overrides): calls to these guest addresses go here. */\n";
        for (const auto& [addr, sym] : prog.overrides) decls << "void " << sym << "(PsxContext* ctx);  /* " << hex32(addr) << " */\n";
    }
    write_if_changed(out_dir / "recomp_funcs.h", decls.str());
    written.insert(out_dir / "recomp_funcs.h");

    // Dispatch tables: boot EXE by address; each overlay with a code fingerprint per entry so
    // the runtime can tell which overlay currently occupies the shared window.
    std::ostringstream table;
    table << file_header();
    table << "const RecompFunctionEntry recomp_function_table[] = {\n";
    for (const auto& [entry, fn] : analysis.functions[0]) {
        const auto ov = prog.overrides.find(entry);
        table << "    {" << hex32(entry) << ", " << (ov != prog.overrides.end() ? ov->second : function_symbol(prog.main(), entry))
              << "},\n";
    }
    table << "};\nconst uint32_t recomp_function_count = " << analysis.functions[0].size() << "u;\n\n";

    std::ostringstream overlays;
    size_t overlay_count = 0;
    for (size_t s = 1; s < prog.segments.size(); ++s) {
        const Segment& seg = prog.segments[s];
        table << "static const RecompOverlayEntry overlay_" << seg.name << "[] = {\n";
        for (const auto& [entry, fn] : analysis.functions[s]) {
            table << "    {" << hex32(entry) << ", " << function_symbol(seg, entry) << ", {";
            for (uint32_t k = 0; k < 4; ++k) table << (k ? ", " : "") << hex32(seg.word(entry + 4 * k).value_or(0));
            table << "}},\n";
        }
        if (analysis.functions[s].empty()) table << "    {0u, 0, {0u, 0u, 0u, 0u}},\n";
        table << "};\n";
        overlays << "    {\"" << seg.name << "\", " << hex32(seg.base) << ", " << hex32(static_cast<uint32_t>(seg.bytes.size()))
                 << ", overlay_" << seg.name << ", " << analysis.functions[s].size() << "u},\n";
        ++overlay_count;
    }
    table << "\nconst RecompOverlay recomp_overlays[] = {\n" << overlays.str();
    if (!overlay_count) table << "    {\"\", 0u, 0u, 0, 0u},\n";
    table << "};\nconst uint32_t recomp_overlay_count = " << overlay_count << "u;\n";
    write_if_changed(out_dir / "function_table.c", table.str());
    written.insert(out_dir / "function_table.c");
    ++stats.files;

    // Remove chunks left over from a previous, larger run.
    for (const auto& entry : fs::directory_iterator(out_dir)) {
        const auto ext = entry.path().extension();
        if ((ext == ".c" || ext == ".h") && !written.count(entry.path())) fs::remove(entry.path());
    }
    return stats;
}

}  // namespace recomp
