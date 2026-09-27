#pragma once
// Guest call chains for diagnostics (load and texture logs): which game functions led here.
//
// The recompiled code keeps guest registers and the guest stack like the original, so return
// addresses sit on the guest stack. backtrace() starts from $ra and then scans the stack for words
// that point just after a jal/jalr into known code: a heuristic (a stale return address left in
// a stack slot can show up), but good enough to name the callers of a load.

#include <psx/recomp.h>

#include <cstdint>
#include <string>
#include <vector>

namespace psx {

/// The context the machine runs (set by Machine; for diagnostics from code without one at hand).
PsxContext* active_context();
void set_active_context(PsxContext* ctx);

/// Names a code address ("f_8001B48C+0x40", "o_OPENSEG_801E1A20+0x88"), or "" if it is not in
/// recompiled code. Registered by the game executable, which links the generated function tables
/// (src/game/code_names.cpp); without one, backtraces are empty.
using Describer = std::string (*)(PsxContext* ctx, uint32_t addr);
void set_describer(Describer describer);
std::string describe_address(PsxContext* ctx, uint32_t addr);

/// Up to `max` return addresses, innermost first.
std::vector<uint32_t> backtrace(PsxContext* ctx, size_t max = 6);

/// " <- f_...+0x.. <- o_...+0x.." (empty if nothing was found).
std::string backtrace_string(PsxContext* ctx, size_t max = 6);

}  // namespace psx
