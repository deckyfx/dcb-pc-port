// Trainer cheat codes: parsing, the editable cheat file, and the code engine (see trainer_cheats.hpp).

#include "trainer_cheats.hpp"

#include <cctype>
#include <cstdio>

namespace trainer {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

/// Drop a trailing "; comment" / "# comment".
std::string_view strip_comment(std::string_view s) {
    const size_t cut = s.find_first_of(";#");
    return trim(cut == std::string_view::npos ? s : s.substr(0, cut));
}

bool parse_hex(std::string_view s, size_t digits, uint32_t& out) {
    if (s.size() != digits) return false;
    uint32_t v = 0;
    for (const char c : s) {
        const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                      : (c >= 'a' && c <= 'f')                    ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F')                    ? c - 'A' + 10
                                                                  : -1;
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

uint16_t read16(const uint8_t* ram, uint32_t off) {
    return static_cast<uint16_t>(ram[off] | (ram[(off + 1) & (kRamSize - 1)] << 8));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// Unsupported types with a known meaning get a specific message.
const char* unsupported_reason(uint32_t type) {
    switch (type) {
    case 0xC1: return "C1 (boot delay) is not supported";
    case 0xC2: return "C2 (memory copy) is not supported";
    case 0xD4: return "D4 (pad button joker) is not supported";
    case 0xD5: return "D5 (codes on by pad buttons) is not supported";
    case 0xD6: return "D6 (codes off by pad buttons) is not supported";
    case 0x1F: return "1F (scratchpad / hardware write) is not supported";
    default: return nullptr;
    }
}

bool is_line_condition(CodeType t) {
    switch (t) {
    case CodeType::IfEq16: case CodeType::IfNe16: case CodeType::IfLt16: case CodeType::IfGt16:
    case CodeType::IfEq8:  case CodeType::IfNe8:  case CodeType::IfLt8:  case CodeType::IfGt8:
        return true;
    default:
        return false;
    }
}

/// Counts writes and the bytes that actually changed.
class Writer {
public:
    Writer(uint8_t* ram, ApplyStats& stats) : ram_(ram), stats_(stats) {}
    void byte(uint32_t off, uint8_t v) {
        off &= kRamSize - 1;
        if (ram_[off] != v) ++stats_.bytes_changed;
        ram_[off] = v;
    }
    void write8(uint32_t off, uint8_t v) {
        ++stats_.writes;
        byte(off, v);
    }
    void write16(uint32_t off, uint16_t v) {
        ++stats_.writes;
        byte(off, static_cast<uint8_t>(v));
        byte(off + 1, static_cast<uint8_t>(v >> 8));
    }

private:
    uint8_t* ram_;
    ApplyStats& stats_;
};

}  // namespace

bool parse_code_line(std::string_view line, uint32_t& code, uint16_t& value, std::string& error) {
    const std::string_view s = strip_comment(line);
    size_t split = 0;
    while (split < s.size() && !std::isspace(static_cast<unsigned char>(s[split]))) ++split;
    const std::string_view a = s.substr(0, split);
    const std::string_view b = trim(s.substr(split));
    uint32_t v = 0;
    if (!parse_hex(a, 8, code) || !parse_hex(b, 4, v)) {
        error = "'" + std::string(s) + "' is not a code (expected 8 hex digits, a space, 4 hex digits)";
        return false;
    }
    value = static_cast<uint16_t>(v);
    return true;
}

bool compile_codes(const std::vector<std::string>& lines, std::vector<CodeOp>& ops, std::string& error) {
    ops.clear();
    for (size_t i = 0; i < lines.size(); ++i) {
        uint32_t code = 0;
        uint16_t value = 0;
        std::string why;
        if (!parse_code_line(lines[i], code, value, why)) {
            error = why;
            return false;
        }
        char label[16];
        std::snprintf(label, sizeof label, "%08X", code);
        const uint32_t type = code >> 24;
        const uint32_t addr = code & 0xFFFFFF;
        const auto fail = [&](const std::string& msg) {
            error = std::string(label) + ": " + msg;
            return false;
        };
        CodeOp op;
        op.offset = addr;
        op.value = value;
        bool eight_bit = false;
        switch (type) {
        case 0x80: op.type = CodeType::Write16; break;
        case 0x30: op.type = CodeType::Write8; eight_bit = true; break;
        case 0x10: op.type = CodeType::Inc16; break;
        case 0x11: op.type = CodeType::Dec16; break;
        case 0x20: op.type = CodeType::Inc8; eight_bit = true; break;
        case 0x21: op.type = CodeType::Dec8; eight_bit = true; break;
        case 0xD0: op.type = CodeType::IfEq16; break;
        case 0xD1: op.type = CodeType::IfNe16; break;
        case 0xD2: op.type = CodeType::IfLt16; break;
        case 0xD3: op.type = CodeType::IfGt16; break;
        case 0xE0: op.type = CodeType::IfEq8; eight_bit = true; break;
        case 0xE1: op.type = CodeType::IfNe8; eight_bit = true; break;
        case 0xE2: op.type = CodeType::IfLt8; eight_bit = true; break;
        case 0xE3: op.type = CodeType::IfGt8; eight_bit = true; break;
        case 0xC0: op.type = CodeType::IfEqAll16; break;
        case 0x50: {
            if ((addr >> 16) != 0) return fail("serial repeater must be 5000nnss");
            if (i + 1 >= lines.size()) return fail("serial repeater needs a following 80 or 30 code");
            uint32_t next = 0;
            uint16_t next_value = 0;
            if (!parse_code_line(lines[i + 1], next, next_value, why)) {
                error = why;
                return false;
            }
            const uint32_t next_type = next >> 24;
            if (next_type != 0x80 && next_type != 0x30) return fail("serial repeater must be followed by an 80 or 30 code");
            op.type = next_type == 0x80 ? CodeType::Repeat16 : CodeType::Repeat8;
            op.count = static_cast<uint8_t>(addr >> 8);
            op.addr_step = static_cast<uint8_t>(addr);
            op.value_step = value;
            op.offset = next & 0xFFFFFF;
            op.value = next_value;
            std::snprintf(label, sizeof label, "%08X", next);
            if (op.offset + static_cast<uint32_t>(op.count > 0 ? op.count - 1 : 0) * op.addr_step +
                    (op.type == CodeType::Repeat16 ? 1u : 0u) >= kRamSize)
                return fail("repeated writes run past the end of RAM (80000000-801FFFFF)");
            if (op.type == CodeType::Repeat8 && next_value > 0xFF) return fail("8-bit value must be 00vv");
            ops.push_back(op);
            ++i;
            continue;
        }
        default:
            if (const char* reason = unsupported_reason(type)) return fail(reason);
            char msg[64];
            std::snprintf(msg, sizeof msg, "code type %02X is not supported", type);
            return fail(msg);
        }
        if (addr >= kRamSize) return fail("address is outside main RAM (80000000-801FFFFF)");
        if (!eight_bit && addr + 1 >= kRamSize) return fail("16-bit access past the end of RAM");
        if (eight_bit && value > 0xFF) return fail("8-bit value must be 00vv");
        ops.push_back(op);
    }
    if (ops.empty()) {
        error = "no codes";
        return false;
    }
    return true;
}

ApplyStats apply_codes(const std::vector<CodeOp>& ops, uint8_t* ram) {
    ApplyStats stats;
    Writer w(ram, stats);
    size_t i = 0;
    while (i < ops.size()) {
        const CodeOp& op = ops[i];
        const uint16_t cur16 = read16(ram, op.offset);
        const uint8_t cur8 = ram[op.offset];
        bool cond = true;
        switch (op.type) {
        case CodeType::Write16: w.write16(op.offset, op.value); break;
        case CodeType::Write8: w.write8(op.offset, static_cast<uint8_t>(op.value)); break;
        case CodeType::Inc16: w.write16(op.offset, static_cast<uint16_t>(cur16 + op.value)); break;
        case CodeType::Dec16: w.write16(op.offset, static_cast<uint16_t>(cur16 - op.value)); break;
        case CodeType::Inc8: w.write8(op.offset, static_cast<uint8_t>(cur8 + op.value)); break;
        case CodeType::Dec8: w.write8(op.offset, static_cast<uint8_t>(cur8 - op.value)); break;
        case CodeType::IfEq16: cond = cur16 == op.value; break;
        case CodeType::IfNe16: cond = cur16 != op.value; break;
        case CodeType::IfLt16: cond = cur16 < op.value; break;
        case CodeType::IfGt16: cond = cur16 > op.value; break;
        case CodeType::IfEq8: cond = cur8 == op.value; break;
        case CodeType::IfNe8: cond = cur8 != op.value; break;
        case CodeType::IfLt8: cond = cur8 < op.value; break;
        case CodeType::IfGt8: cond = cur8 > op.value; break;
        case CodeType::IfEqAll16:
            if (cur16 != op.value) return stats;  // the rest of the cheat is off
            break;
        case CodeType::Repeat16:
        case CodeType::Repeat8: {
            uint32_t off = op.offset;
            uint16_t v = op.value;
            for (unsigned n = 0; n < op.count; ++n) {
                if (op.type == CodeType::Repeat16) w.write16(off, v); else w.write8(off, static_cast<uint8_t>(v));
                off += op.addr_step;
                v = static_cast<uint16_t>(v + op.value_step);
            }
            break;
        }
        }
        ++i;
        if (!cond) {
            // Skip the rest of a chain of conditions, then the code they guard.
            while (i < ops.size() && is_line_condition(ops[i].type)) ++i;
            ++i;
        }
    }
    return stats;
}

// ---------------------------------------------------------------------------------------------
// CheatSet
// ---------------------------------------------------------------------------------------------

CheatSet CheatSet::parse(std::string_view text) {
    CheatSet set;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        set.lines_.emplace_back(line);
        pos = end + 1;
    }
    set.reparse();
    return set;
}

void CheatSet::reparse() {
    cheats_.clear();
    warnings_.clear();
    Cheat* current = nullptr;
    const auto finish = [&](size_t end) {
        if (current == nullptr) return;
        current->end_line = end;
        if (!compile_codes(current->codes, current->ops, current->error)) {
            current->ops.clear();
            if (current->enabled)
                warnings_.push_back("[" + current->name + "] " + current->error + " (cheat disabled)");
            else
                warnings_.push_back("[" + current->name + "] " + current->error);
            current->enabled = false;
        }
    };
    for (size_t n = 0; n < lines_.size(); ++n) {
        const std::string_view line = trim(lines_[n]);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::string where = "line " + std::to_string(n + 1) + ": ";
        if (line.front() == '[') {
            finish(n);
            const size_t close = line.find(']');
            Cheat c;
            c.header_line = n;
            if (close == std::string_view::npos) {
                c.name = std::string(trim(line.substr(1)));
                warnings_.push_back(where + "missing ']' in cheat name");
            } else {
                c.name = std::string(trim(line.substr(1, close - 1)));
                const std::string state = lower(strip_comment(line.substr(close + 1)));
                if (state == "on") {
                    c.enabled = true;
                } else if (!state.empty() && state != "off") {
                    warnings_.push_back(where + "'" + state + "' after [" + c.name + "]: expected on or off");
                }
            }
            if (c.name.empty()) c.name = "(unnamed)";
            cheats_.push_back(std::move(c));
            current = &cheats_.back();
            continue;
        }
        if (current == nullptr) {
            warnings_.push_back(where + "code before any [cheat name] is ignored");
            continue;
        }
        current->codes.emplace_back(strip_comment(line));
    }
    finish(lines_.size());
}

std::string CheatSet::text() const {
    std::string out;
    for (const std::string& l : lines_) {
        out += l;
        out += '\n';
    }
    return out;
}

size_t CheatSet::enabled_count() const {
    size_t n = 0;
    for (const Cheat& c : cheats_) n += c.enabled ? 1 : 0;
    return n;
}

bool CheatSet::set_enabled(size_t index, bool enabled) {
    if (index >= cheats_.size()) return false;
    Cheat& c = cheats_[index];
    if (enabled && !c.error.empty()) return false;
    std::string& header = lines_[c.header_line];
    const size_t indent = header.find('[');
    header = header.substr(0, indent == std::string::npos ? 0 : indent) + "[" + c.name + "] " + (enabled ? "on" : "off");
    c.enabled = enabled;
    return true;
}

bool CheatSet::add(std::string_view name, const std::vector<std::string>& codes, bool enabled, std::string* error) {
    std::vector<CodeOp> ops;
    std::string why;
    if (!compile_codes(codes, ops, why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    std::string clean(name);
    for (char& ch : clean)
        if (ch == '[' || ch == ']' || ch == '\n' || ch == '\r') ch = ' ';
    if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.emplace_back();
    lines_.push_back("[" + clean + "] " + (enabled ? "on" : "off"));
    for (const std::string& c : codes) lines_.push_back(c);
    reparse();
    return true;
}

bool CheatSet::remove(size_t index) {
    if (index >= cheats_.size()) return false;
    const Cheat& c = cheats_[index];
    // Remove the header and everything up to its last code line; comments after that most
    // likely introduce the next cheat and stay.
    size_t end = c.header_line + 1;
    for (size_t n = c.header_line + 1; n < c.end_line; ++n) {
        const std::string_view l = trim(lines_[n]);
        if (!l.empty() && l.front() != '#' && l.front() != ';') end = n + 1;
    }
    // Also drop one blank separator line so repeated add/remove does not pile up blank lines.
    if (end < lines_.size() && trim(lines_[end]).empty()) ++end;
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(c.header_line),
                 lines_.begin() + static_cast<std::ptrdiff_t>(end));
    reparse();
    return true;
}

ApplyStats CheatSet::apply(uint8_t* ram) const {
    ApplyStats total;
    for (const Cheat& c : cheats_) {
        if (!c.enabled || !c.error.empty()) continue;
        const ApplyStats s = apply_codes(c.ops, ram);
        total.writes += s.writes;
        total.bytes_changed += s.bytes_changed;
    }
    return total;
}

std::vector<std::string> freeze_codes(uint32_t offset, uint32_t value, int size_bytes) {
    char line[32];
    std::vector<std::string> out;
    offset &= kRamSize - 1;
    if (size_bytes == 1) {
        std::snprintf(line, sizeof line, "30%06X 00%02X", offset, value & 0xFF);
        out.emplace_back(line);
    } else {
        std::snprintf(line, sizeof line, "80%06X %04X", offset, value & 0xFFFF);
        out.emplace_back(line);
        if (size_bytes == 4) {
            std::snprintf(line, sizeof line, "80%06X %04X", offset + 2, (value >> 16) & 0xFFFF);
            out.emplace_back(line);
        }
    }
    return out;
}

std::filesystem::path resolve_cheat_path(std::string_view serial, const std::filesystem::path& override_path,
                                         const std::filesystem::path& cwd, const std::filesystem::path& exe_dir,
                                         const std::function<bool(const std::filesystem::path&)>& exists) {
    if (!override_path.empty()) return override_path;
    const std::filesystem::path name = std::filesystem::path("cheats") / (std::string(serial) + ".txt");
    const std::filesystem::path local = cwd / name;
    if (exists && exists(local)) return local;
    if (!exe_dir.empty()) {
        const std::filesystem::path portable = exe_dir / name;
        if (exists && exists(portable)) return portable;
    }
    return local;
}

}  // namespace trainer
