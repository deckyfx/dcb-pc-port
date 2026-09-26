// dcb_recompiler — MIPS R3000A -> C static recompiler (host tool).
//
//   dcb_recompiler <repo root> <serial>
//
// Inputs:  extracted/<serial>/exe/boot.exe, config/<serial>/{recomp,functions,overlays}.json
// Outputs: generated/<serial>/*.c + recomp_funcs.h + function_table.c, and
//          generated/<serial>/discovered.json (every function found, for Ghidra and review).

#include "analysis.hpp"
#include "emit.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

void write_report(const recomp::Program& prog, const recomp::Analysis& an, const fs::path& path) {
    json out = {{"game_id", prog.game_id}, {"segments", json::array()}};
    for (size_t s = 0; s < prog.segments.size(); ++s) {
        const auto& seg = prog.segments[s];
        json fns = json::array();
        for (const auto& [entry, f] : an.functions[s]) {
            json j = {{"entry", hex(entry)}, {"end", hex(f.hi())}, {"instructions", f.instrs.size()},
                      {"origin", recomp::origin_name(f.origin)}, {"symbol", recomp::function_symbol(seg, entry)}};
            if (!f.tables.empty()) j["jump_tables"] = f.tables.size();
            if (!f.invalid_at.empty()) j["invalid_at"] = hex(f.invalid_at.front());
            if (!f.load_delay_hazards.empty()) j["load_delay_hazards"] = f.load_delay_hazards.size();
            fns.push_back(std::move(j));
        }
        out["segments"].push_back({{"name", seg.name}, {"base", hex(seg.base)}, {"functions", std::move(fns)}});
    }
    std::ofstream(path) << out.dump(1) << "\n";
}

/// Print how much of each segment's non-zero code range the analysis reached.
void print_coverage(const recomp::Program& prog, const recomp::Analysis& an) {
    for (size_t s = 0; s < prog.segments.size(); ++s) {
        const auto& seg = prog.segments[s];
        std::vector<uint8_t> hit(seg.bytes.size() / 4, 0);
        size_t tables = 0, invalid = 0, hazards = 0;
        std::map<recomp::Origin, size_t> origins;
        for (const auto& [entry, f] : an.functions[s]) {
            for (uint32_t pc : f.instrs) hit[(pc - seg.base) / 4] = 1;
            tables += f.tables.size();
            invalid += !f.invalid_at.empty();
            hazards += f.load_delay_hazards.size();
            ++origins[f.origin];
        }
        size_t nonzero = 0, covered = 0;
        for (uint32_t a = seg.code_begin; a < seg.code_end; a += 4) {
            if (*seg.word(a) == 0) continue;
            ++nonzero;
            covered += hit[(a - seg.base) / 4];
        }
        std::printf("%-8s %5zu functions  code %6zu/%6zu words (%5.1f%%)  tables %3zu  invalid %zu  load-delay %zu  [",
                    seg.name.c_str(), an.functions[s].size(), covered, nonzero,
                    nonzero ? 100.0 * static_cast<double>(covered) / static_cast<double>(nonzero) : 0.0, tables, invalid,
                    hazards);
        for (const auto& [o, n] : origins) std::printf(" %s=%zu", recomp::origin_name(o), n);
        std::printf(" ]\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <repo root> <serial>\n", argv[0]);
        return 2;
    }
    try {
        const fs::path root = argv[1];
        const std::string id = argv[2];
        json cfg;
        std::ifstream(root / "config" / id / "recomp.json") >> cfg;
        const uint32_t text_begin = static_cast<uint32_t>(std::stoul(cfg.at("text").at(0).get<std::string>(), nullptr, 16));
        const uint32_t text_end = static_cast<uint32_t>(std::stoul(cfg.at("text").at(1).get<std::string>(), nullptr, 16));

        const recomp::Program prog = recomp::load_program(root, id, text_begin, text_end);
        const recomp::Analysis an = recomp::analyze(prog);
        print_coverage(prog, an);

        const fs::path out = root / "generated" / id;
        const auto stats = recomp::emit_program(prog, an, out);
        write_report(prog, an, out / "discovered.json");
        std::printf("emitted %zu functions (%zu instructions) into %zu files under %s\n", stats.functions,
                    stats.instructions, stats.files, out.string().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dcb_recompiler: %s\n", e.what());
        return 1;
    }
    return 0;
}
