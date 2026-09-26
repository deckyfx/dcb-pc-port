#include "analysis.hpp"

#include <algorithm>
#include <deque>
#include <optional>

namespace recomp {

namespace {

constexpr size_t kMaxFunctionInstrs = 1u << 16;
constexpr unsigned kJumpTableWindow = 16;   // instructions scanned back from a jr
constexpr unsigned kMaxTableEntries = 1024;

/// Abstract value used to recognise compiler-generated jump tables.
struct AbsVal {
    enum Kind : uint8_t { Unknown, Const, Scaled, Indexed, TableLoad } kind = Unknown;
    uint32_t k = 0;  // Const: value; Indexed: table base; TableLoad: table address
};

/// Evaluate the straight-line window before `jr` and return the table address, if any.
std::optional<uint32_t> match_jump_table(const Segment& seg, uint32_t jr_pc, int reg, uint32_t& bound) {
    AbsVal regs[32] = {};
    bound = 0;
    const uint32_t start = jr_pc >= seg.code_begin + 4 * kJumpTableWindow ? jr_pc - 4 * kJumpTableWindow : seg.code_begin;
    for (uint32_t pc = start; pc < jr_pc; pc += 4) {
        const Instr in = seg.instr(pc);
        const int w = gpr_written(in);
        AbsVal v;
        switch (in.op) {
            case Op::Lui:
                v = {AbsVal::Const, static_cast<uint32_t>(in.imm) << 16};
                break;
            case Op::Addiu:
                if (regs[in.rs].kind == AbsVal::Const) v = {AbsVal::Const, regs[in.rs].k + static_cast<uint32_t>(in.simm())};
                else if (regs[in.rs].kind == AbsVal::Indexed) v = {AbsVal::Indexed, regs[in.rs].k + static_cast<uint32_t>(in.simm())};
                break;
            case Op::Ori:
                if (regs[in.rs].kind == AbsVal::Const) v = {AbsVal::Const, regs[in.rs].k | in.imm};
                break;
            case Op::Sll:
                if (in.sa == 2) v = {AbsVal::Scaled, 0};
                break;
            case Op::Addu: {
                const AbsVal& a = regs[in.rs];
                const AbsVal& b = regs[in.rt];
                if (a.kind == AbsVal::Const && b.kind == AbsVal::Scaled) v = {AbsVal::Indexed, a.k};
                else if (b.kind == AbsVal::Const && a.kind == AbsVal::Scaled) v = {AbsVal::Indexed, b.k};
                break;
            }
            case Op::Lw:
                if (regs[in.rs].kind == AbsVal::Indexed) v = {AbsVal::TableLoad, regs[in.rs].k + static_cast<uint32_t>(in.simm())};
                break;
            case Op::Sltiu:
                bound = in.imm;  // the bounds check that guards the table
                break;
            default:
                break;
        }
        if (w > 0) regs[w] = v;
        // The index register itself is unknown to us; `sll idx, idx, 2` keeps Scaled.
    }
    if (regs[reg].kind == AbsVal::TableLoad) return regs[reg].k;
    return std::nullopt;
}

class Explorer {
public:
    explicit Explorer(const Program& prog) : prog_(prog), analysis_{} {
        analysis_.functions.resize(prog.segments.size());
        for (const Segment& seg : prog.segments) coverage_.emplace_back(seg.bytes.size() / 4, 0);
    }

    Analysis run() {
        // Strong seeds.
        add_seed(0, prog_.entry, Origin::Entry);
        for (uint32_t e : prog_.known_functions) add_seed(0, e, Origin::Ghidra);
        drain();
        reclassify();

        // Weak seeds: accepted only outside known code, and only if they explore cleanly.
        for (size_t s = 0; s < prog_.segments.size(); ++s) {
            for (auto [addr, origin] : weak_seeds(s)) try_weak_seed(s, addr, origin);
        }
        drain();
        reclassify();
        return std::move(analysis_);
    }

private:
    const Program& prog_;
    Analysis analysis_;
    std::deque<std::pair<size_t, uint32_t>> pending_;
    std::map<std::pair<size_t, uint32_t>, Origin> seeds_;
    std::vector<std::vector<uint8_t>> coverage_;  ///< per segment, per word: reached by some function

    void mark(size_t s, const Function& f) {
        const Segment& seg = prog_.segments[s];
        for (uint32_t pc : f.instrs) coverage_[s][(pc - seg.base) / 4] = 1;
    }

    bool is_entry(size_t seg, uint32_t addr) const { return seeds_.count({seg, addr}) != 0; }

    void add_seed(size_t seg, uint32_t addr, Origin origin) {
        if (!prog_.segments[seg].in_code(addr)) return;
        auto [it, inserted] = seeds_.emplace(std::make_pair(seg, addr), origin);
        if (!inserted) {
            if (origin < it->second) it->second = origin;
            return;
        }
        pending_.emplace_back(seg, addr);
    }

    /// Which segments can hold a call target: overlay addresses are ambiguous, so every overlay
    /// that has a plausible function start there qualifies.
    void add_call_target(size_t from_seg, uint32_t target) {
        const Segment& from = prog_.segments[from_seg];
        if (prog_.main().in_code(target)) {
            add_seed(0, target, Origin::Call);
        } else if (from.overlay && from.in_code(target)) {
            add_seed(from_seg, target, Origin::Call);
        } else if (prog_.in_overlay_window(target)) {
            for (size_t s = 1; s < prog_.segments.size(); ++s) {
                if (plausible_start(prog_.segments[s], target) && explore(s, target, true).invalid_at.empty())
                    add_seed(s, target, Origin::Call);
            }
        }
    }

    static bool plausible_start(const Segment& seg, uint32_t addr) {
        if (!seg.in_code(addr)) return false;
        const Instr first = seg.instr(addr);
        if (first.op == Op::Invalid) return false;
        if (first.op == Op::Addiu && first.rs == 29 && first.rt == 29 && first.simm() < 0) return true;
        if (addr == seg.code_begin) return true;
        // Previous function ended: `jr ra` / `j` two instructions back (its delay slot is between).
        const Instr prev = seg.instr(addr - 8);
        return (prev.op == Op::Jr && prev.rs == 31) || prev.op == Op::J;
    }

    /// Explore every pending seed; newly found calls add more seeds.
    void drain() {
        while (!pending_.empty()) {
            auto [seg, addr] = pending_.front();
            pending_.pop_front();
            Function f = explore(seg, addr, false);
            f.origin = seeds_.at({seg, addr});
            collect_calls(seg, f);
            mark(seg, f);
            analysis_.functions[seg][addr] = std::move(f);
        }
    }

    /// Tail-jump vs internal-jump classification depends on the final entry set: re-explore all.
    void reclassify() {
        for (size_t s = 0; s < analysis_.functions.size(); ++s) {
            std::fill(coverage_[s].begin(), coverage_[s].end(), 0);
            for (auto& [addr, f] : analysis_.functions[s]) {
                const Origin o = f.origin;
                f = explore(s, addr, false);
                f.origin = o;
                mark(s, f);
            }
        }
    }

    void collect_calls(size_t seg, const Function& f) {
        for (uint32_t pc : f.instrs) {
            const Instr in = f.seg->instr(pc);
            if (in.op == Op::Jal) add_call_target(seg, in.jump_target());
            else if (in.op == Op::J && !f.instrs.count(in.jump_target())) add_call_target(seg, in.jump_target());
        }
    }

    Function explore(size_t seg_idx, uint32_t entry, bool probe) const {
        const Segment& seg = prog_.segments[seg_idx];
        Function f;
        f.seg = &seg;
        f.entry = entry;
        std::deque<uint32_t> work{entry};
        auto visit = [&](uint32_t a) { if (!f.instrs.count(a)) work.push_back(a); };

        while (!work.empty() && f.instrs.size() < kMaxFunctionInstrs) {
            const uint32_t pc = work.front();
            work.pop_front();
            if (f.instrs.count(pc)) continue;
            const Instr in = seg.in_code(pc) ? seg.instr(pc) : Instr{0, pc, Op::Invalid, 0, 0, 0, 0, 0};
            if (in.op == Op::Invalid) {
                f.invalid_at.push_back(pc);
                if (probe) return f;
                continue;
            }
            f.instrs.insert(pc);

            if (has_delay_slot(in.op)) {
                const Instr slot = seg.instr(pc + 4);
                if (slot.op == Op::Invalid || has_delay_slot(slot.op)) {
                    f.invalid_at.push_back(pc + 4);  // branch in a delay slot: unsupported
                    if (probe) return f;
                    continue;
                }
                f.instrs.insert(pc + 4);
            }

            switch (in.op) {
                case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz: case Op::Bltz: case Op::Bgez: {
                    visit(in.branch_target());
                    // `b` is encoded as beq zero,zero (or bgez/blez zero): nothing falls through,
                    // and what follows is often a literal pool.
                    const bool always = (in.op == Op::Beq && in.rs == in.rt) ||
                                        ((in.op == Op::Bgez || in.op == Op::Blez) && in.rs == 0);
                    if (!always) visit(pc + 8);
                    break;
                }
                case Op::Bltzal: case Op::Bgezal: case Op::Jal: case Op::Jalr:
                    visit(pc + 8);  // calls return
                    break;
                case Op::J: {
                    const uint32_t t = in.jump_target();
                    if (!is_entry(seg_idx, t) && seg.in_code(t)) visit(t);  // otherwise a tail call
                    break;
                }
                case Op::Jr:
                    if (in.rs != 31) {
                        uint32_t bound = 0;
                        if (auto table = match_jump_table(seg, pc, in.rs, bound)) {
                            JumpTable jt{*table, {}};
                            const Segment* holder = seg.contains(*table) ? &seg : &prog_.main();
                            const unsigned n = bound ? bound : kMaxTableEntries;
                            for (unsigned i = 0; i < n; ++i) {
                                const auto target = holder->word(*table + 4 * i);
                                if (!target || !seg.in_code(*target)) break;
                                jt.targets.push_back(*target);
                            }
                            for (uint32_t t : jt.targets) visit(t);
                            f.tables.emplace(pc, std::move(jt));
                        }
                    }
                    break;
                default:
                    visit(pc + 4);
                    break;
            }
        }

        // Load-delay lint: R3000A loads land one instruction late; compilers avoid relying on it.
        for (uint32_t pc : f.instrs) {
            const Instr in = seg.instr(pc);
            if (is_load(in.op) && in.rt != 0 && f.instrs.count(pc + 4) && reads_gpr(seg.instr(pc + 4), in.rt) &&
                in.op != Op::Lwl && in.op != Op::Lwr)
                f.load_delay_hazards.push_back(pc);
        }
        return f;
    }

    std::vector<std::pair<uint32_t, Origin>> weak_seeds(size_t s) const {
        const Segment& seg = prog_.segments[s];
        std::vector<std::pair<uint32_t, Origin>> out;
        // Words in data that point at code.
        for (uint32_t a = seg.base; a + 4 <= seg.end(); a += 4) {
            const bool is_code_word = seg.overlay ? covered(s, a) : seg.in_code(a);
            if (is_code_word) continue;
            const uint32_t v = *seg.word(a);
            if (seg.in_code(v)) out.emplace_back(v, Origin::DataPointer);
            else if (seg.overlay && prog_.main().in_code(v)) out.emplace_back(v, Origin::DataPointer);
        }
        // lui/addiu pairs in reached code that build a code address (callbacks).
        for (const auto& [entry, f] : analysis_.functions[s]) {
            for (uint32_t pc : f.instrs) {
                const Instr hi = seg.instr(pc);
                if (hi.op != Op::Lui) continue;
                for (uint32_t k = 1; k <= 6; ++k) {
                    const Instr lo = seg.instr(pc + 4 * k);
                    if ((lo.op == Op::Addiu || lo.op == Op::Ori) && lo.rs == hi.rt) {
                        const uint32_t v = (static_cast<uint32_t>(hi.imm) << 16) +
                                           (lo.op == Op::Addiu ? static_cast<uint32_t>(lo.simm()) : lo.imm);
                        if (seg.in_code(v) || prog_.main().in_code(v)) out.emplace_back(v, Origin::CodeConstant);
                        break;
                    }
                    if (gpr_written(lo) == hi.rt) break;
                }
            }
        }
        return out;
    }

    bool covered(size_t s, uint32_t addr) const {
        const Segment& seg = prog_.segments[s];
        return seg.contains(addr) && coverage_[s][(addr - seg.base) / 4] != 0;
    }

    void try_weak_seed(size_t from_seg, uint32_t addr, Origin origin) {
        const size_t s = prog_.segments[from_seg].in_code(addr) ? from_seg : 0;
        if (is_entry(s, addr) || covered(s, addr)) return;
        const Function probe = explore(s, addr, true);
        if (!probe.invalid_at.empty() || probe.instrs.empty()) return;
        // A real function never runs into code we already know from the middle.
        for (uint32_t pc : probe.instrs) {
            if (covered(s, pc)) return;
        }
        add_seed(s, addr, origin);
        drain();
    }
};

}  // namespace

const char* origin_name(Origin o) {
    switch (o) {
        case Origin::Entry: return "entry";
        case Origin::Ghidra: return "ghidra";
        case Origin::Call: return "call";
        case Origin::DataPointer: return "data_pointer";
        case Origin::CodeConstant: return "code_constant";
    }
    return "?";
}

const Function* Analysis::find(size_t seg, uint32_t entry) const {
    const auto& m = functions[seg];
    const auto it = m.find(entry);
    return it == m.end() ? nullptr : &it->second;
}

Analysis analyze(const Program& prog) { return Explorer(prog).run(); }

}  // namespace recomp
