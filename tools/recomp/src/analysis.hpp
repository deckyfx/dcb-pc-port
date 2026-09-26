#pragma once
// Function discovery: control-flow exploration from seeds across the boot EXE and overlays.

#include "program.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

/// Why a function was considered an entry point, strongest first.
enum class Origin : uint8_t { Entry, Ghidra, Call, DataPointer, CodeConstant, PrologueSweep };
const char* origin_name(Origin o);

struct JumpTable {
    uint32_t table_addr = 0;
    std::vector<uint32_t> targets;
};

struct Function {
    const Segment* seg = nullptr;
    uint32_t entry = 0;
    Origin origin = Origin::Call;
    std::set<uint32_t> instrs;                ///< every reached instruction, incl. delay slots
    std::map<uint32_t, JumpTable> tables;     ///< keyed by the jr's address
    std::vector<uint32_t> invalid_at;         ///< addresses that did not decode (strong seeds only)
    std::vector<uint32_t> load_delay_hazards; ///< loads whose next instruction reads the loaded register

    uint32_t lo() const { return *instrs.begin(); }
    uint32_t hi() const { return *instrs.rbegin() + 4; }
};

struct Analysis {
    /// Functions per segment (same index as Program::segments), keyed by entry address.
    std::vector<std::map<uint32_t, Function>> functions;

    const Function* find(size_t seg, uint32_t entry) const;
};

Analysis analyze(const Program& prog);

}  // namespace recomp
