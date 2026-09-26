#pragma once
// The guest program as the recompiler sees it: the boot EXE plus code overlays that share
// one load window, each a Segment with its own bytes and base address.

#include "mips.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

struct Segment {
    std::string name;           ///< "main" for the boot EXE, else the overlay name (KAWSEG, ...)
    bool overlay = false;
    uint32_t base = 0;          ///< guest load address of bytes[0]
    std::vector<uint8_t> bytes;
    uint32_t code_begin = 0;    ///< range where code may live (main: .text; overlays: whole image)
    uint32_t code_end = 0;

    uint32_t end() const { return base + static_cast<uint32_t>(bytes.size()); }
    bool contains(uint32_t addr) const { return addr >= base && addr < end(); }
    bool in_code(uint32_t addr) const { return addr >= code_begin && addr < code_end && (addr & 3) == 0; }
    std::optional<uint32_t> word(uint32_t addr) const;
    Instr instr(uint32_t addr) const;  ///< Op::Invalid outside the segment
};

struct Program {
    std::string game_id;
    uint32_t entry = 0;
    std::vector<Segment> segments;   ///< [0] is the boot EXE
    std::vector<uint32_t> known_functions;  ///< main-EXE entries from Ghidra (functions.json)

    const Segment& main() const { return segments.front(); }
    /// Overlay window shared by all overlays: [lo, hi).
    uint32_t overlay_lo = 0, overlay_hi = 0;
    bool in_overlay_window(uint32_t addr) const { return addr >= overlay_lo && addr < overlay_hi; }
};

/// Load extracted/<id>/exe/boot.exe, config/<id>/overlays.json (+ its container file) and
/// config/<id>/functions.json. `text_begin/text_end` bound code in the boot EXE.
Program load_program(const std::filesystem::path& root, const std::string& game_id,
                     uint32_t text_begin, uint32_t text_end);

}  // namespace recomp
