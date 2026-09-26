#pragma once
// Trainer, part 1: PS1 GameShark / Action Replay cheat codes. Parses a cheat file, keeps it as
// editable text (comments survive toggling, adding and removing cheats) and applies the enabled
// cheats to a 2 MB guest-RAM buffer. Pure: no SDL, no guest runtime, unit-tested on plain buffers.
//
// File format (cheats/<serial>.txt):
//
//   # comment (also ';')
//   [Infinite money] on        <- a cheat: name in brackets, then "on" / "off" (missing = off)
//   800B1234 270F              <- code lines: 8 hex digits, a space, 4 hex digits
//   300B1236 0063  ; comment   <- a trailing ';' or '#' comment is allowed
//
// Supported code types (address = 0x80000000 | the low 24 bits, must be inside the 2 MB RAM):
//   80aaaaaa vvvv  16-bit write           30aaaaaa 00vv  8-bit write
//   10aaaaaa vvvv  16-bit increment       11aaaaaa vvvv  16-bit decrement
//   20aaaaaa 00vv  8-bit increment        21aaaaaa 00vv  8-bit decrement
//   D0/D1/D2/D3aaaaaa vvvv  if 16-bit value ==, !=, <, > vvvv then run the next code
//   E0/E1/E2/E3aaaaaa 00vv  the same with an 8-bit value
//   C0aaaaaa vvvv  if 16-bit value == vvvv run the rest of this cheat, else stop it
//   5000nnss iiii  serial repeater: the next line (80 or 30) is applied nn times, the address
//                  advancing by ss bytes and the value by iiii each time
// A false condition skips the next code; consecutive conditions combine (all must hold).
// Increment / decrement codes run every frame, like on the cartridge: guard them with a condition.
//
// Unsupported (the whole cheat is rejected with a message, never half-applied): C1 (boot delay),
// C2 (memory copy), D4/D5/D6 (pad-button jokers), 1F (scratchpad / hardware writes) and any
// other type.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trainer {

/// Guest main RAM size (PSX_RAM_SIZE).
inline constexpr uint32_t kRamSize = 0x200000;
/// KSEG0 base the trainer uses to show addresses.
inline constexpr uint32_t kRamBase = 0x80000000;

enum class CodeType : uint8_t {
    Write16, Write8, Inc16, Dec16, Inc8, Dec8,
    IfEq16, IfNe16, IfLt16, IfGt16,  ///< D0..D3
    IfEq8, IfNe8, IfLt8, IfGt8,      ///< E0..E3
    IfEqAll16,                       ///< C0: gates the rest of the cheat
    Repeat16, Repeat8,               ///< 50 + 80 / 50 + 30
};

/// One compiled code (a 50 repeater and the line it repeats form a single op).
struct CodeOp {
    CodeType type = CodeType::Write16;
    uint32_t offset = 0;      ///< RAM offset, 0 .. kRamSize-1
    uint16_t value = 0;
    uint8_t count = 0;        ///< Repeat*: number of writes
    uint8_t addr_step = 0;    ///< Repeat*: address increment per write
    uint16_t value_step = 0;  ///< Repeat*: value increment per write
};

/// What applying codes did to RAM (for tracing / tests).
struct ApplyStats {
    size_t writes = 0;         ///< write / increment / decrement operations executed
    size_t bytes_changed = 0;  ///< bytes whose value actually changed
};

/// Parse one code line ("800B1234 270F", optional trailing comment) into its two numbers.
/// Returns false with a message in `error` if the line is malformed.
bool parse_code_line(std::string_view line, uint32_t& code, uint16_t& value, std::string& error);

/// Compile the code lines of one cheat. On failure returns false and describes the first bad
/// line in `error` ("80XXXXXX: ..."); `ops` is then unspecified.
bool compile_codes(const std::vector<std::string>& lines, std::vector<CodeOp>& ops, std::string& error);

/// Run compiled codes against `ram` (kRamSize bytes).
ApplyStats apply_codes(const std::vector<CodeOp>& ops, uint8_t* ram);

/// One named cheat of a CheatSet.
struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<std::string> codes;  ///< code lines as written (comments stripped, trimmed)
    std::vector<CodeOp> ops;         ///< compiled; empty if `error` is set
    std::string error;               ///< why the cheat cannot be used ("" = valid)
    size_t header_line = 0;          ///< line index of "[name]" in the text
    size_t end_line = 0;             ///< one past its last line (codes and comments)
};

/// The contents of a cheat file: its text (the source of truth, so edits keep the user's
/// comments and layout) and the cheats parsed from it.
class CheatSet {
public:
    /// Parse `text`. Never fails: problems become warnings() and per-cheat errors.
    static CheatSet parse(std::string_view text);

    const std::vector<Cheat>& cheats() const { return cheats_; }
    /// Problems found while parsing ("line 12: ..."), including every rejected cheat.
    const std::vector<std::string>& warnings() const { return warnings_; }
    /// The file text, with every edit applied ('\n' line endings, trailing newline).
    std::string text() const;
    size_t enabled_count() const;

    /// Turn cheat `index` on or off (rewrites its header line). False if the index is out of
    /// range or an invalid cheat is being enabled.
    bool set_enabled(size_t index, bool enabled);
    /// Append a cheat. `codes` must compile; false (and nothing added) otherwise, with `error`.
    bool add(std::string_view name, const std::vector<std::string>& codes, bool enabled, std::string* error = nullptr);
    /// Remove cheat `index` with its code and comment lines.
    bool remove(size_t index);

    /// Apply every enabled, valid cheat (in file order) to `ram` (kRamSize bytes).
    ApplyStats apply(uint8_t* ram) const;

private:
    void reparse();

    std::vector<std::string> lines_;
    std::vector<Cheat> cheats_;
    std::vector<std::string> warnings_;
};

/// Code lines that hold `value` at RAM offset `offset`: one 30 code (size 1), one 80 code
/// (size 2) or two 80 codes (size 4, low half first).
std::vector<std::string> freeze_codes(uint32_t offset, uint32_t value, int size_bytes);

/// Where the cheat file lives: `override_path` (DCB_CHEATS) when set; else
/// <cwd>/cheats/<serial>.txt if it exists; else <exe_dir>/cheats/<serial>.txt if it exists; else
/// the current-directory location (where a new file is written on save).
std::filesystem::path resolve_cheat_path(std::string_view serial, const std::filesystem::path& override_path,
                                         const std::filesystem::path& cwd, const std::filesystem::path& exe_dir,
                                         const std::function<bool(const std::filesystem::path&)>& exists);

}  // namespace trainer
