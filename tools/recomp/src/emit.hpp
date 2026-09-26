#pragma once
// C code generation for analysed functions.

#include "analysis.hpp"

#include <filesystem>

namespace recomp {

struct EmitStats {
    size_t functions = 0;
    size_t files = 0;
    size_t instructions = 0;
};

/// Write generated/<id>/: recomp_funcs.h, per-segment C chunks, and function_table.c.
EmitStats emit_program(const Program& prog, const Analysis& analysis, const std::filesystem::path& out_dir);

/// C identifier for a function (f_80012345 / o_KAWSEG_801E2A6C).
std::string function_symbol(const Segment& seg, uint32_t entry);

}  // namespace recomp
