// Trainer panel logic (see trainer.hpp).

#include "trainer.hpp"

#include "settings.hpp"  // read_text_file / write_text_file

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace trainer {

namespace {

std::string hex_address(uint32_t offset) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", kRamBase | offset);
    return buf;
}

const char* size_name(ValueSize s) {
    switch (s) {
    case ValueSize::U8: return "8-bit";
    case ValueSize::U16: return "16-bit";
    case ValueSize::U32: return "32-bit";
    }
    return "?";
}

bool is_value_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' ||
           c == '$' || c == '-';
}

}  // namespace

std::unique_ptr<Trainer> make_trainer(uint8_t* ram, std::string_view serial) {
    const platform::SettingsLocations where = platform::current_settings_locations();
    const char* override_path = std::getenv("DCB_CHEATS");
    const auto exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    };
    auto t = std::make_unique<Trainer>(
        ram, resolve_cheat_path(serial, override_path != nullptr ? override_path : "", where.cwd, where.exe_dir, exists));
    for (const std::string& line : t->load()) std::printf("[cheats] %s\n", line.c_str());
    t->set_trace(std::getenv("DCB_TRACE_CHEATS") != nullptr);
    return t;
}

Trainer::Trainer(uint8_t* ram, std::filesystem::path cheat_path) : ram_(ram), path_(std::move(cheat_path)) {}

std::vector<std::string> Trainer::load() {
    std::vector<std::string> log;
    std::error_code ec;
    const std::string where = path_.string();
    if (!std::filesystem::exists(path_, ec)) {
        cheats_ = CheatSet::parse("");
        log.push_back("no cheat file at " + where);
    } else if (const std::optional<std::string> text = platform::read_text_file(path_)) {
        // The Battle tab's lines ("!battle ...", "!toggle ...") are its own; the cheat parser gets
        // the rest.
        battle_ = BattleActions();
        toggles_ = GameToggles();
        preset_states_.clear();
        std::string rest;
        size_t pos = 0;
        while (pos < text->size()) {
            size_t end = text->find('\n', pos);
            if (end == std::string::npos) end = text->size();
            const std::string_view line(text->data() + pos, end - pos);
            const size_t lead = line.find_first_not_of(" \t");
            if (lead != std::string_view::npos && line.substr(lead, 7) == "!preset") {
                // "!preset <name> on|off": the name may contain spaces, the state is the last word.
                const std::string_view body = line.substr(lead + 7);
                const size_t cut = body.find_last_of(" \t");
                const std::string_view state = cut == std::string_view::npos ? body : body.substr(cut + 1);
                std::string_view name = cut == std::string_view::npos ? std::string_view() : body.substr(0, cut);
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
                if (name.empty() || (state != "on" && state != "off"))
                    log.push_back(where + ": bad preset line '" + std::string(line) + "'");
                else
                    preset_states_.emplace_back(std::string(name), state == "on");
            } else if (BattleActions::owns_line(line)) {
                if (line.find("#!battle") == std::string_view::npos && !battle_.parse_line(line))
                    log.push_back(where + ": bad battle line '" + std::string(line) + "'");
            } else if (GameToggles::owns_line(line)) {
                if (line.find("#!toggle") == std::string_view::npos && !toggles_.parse_line(line))
                    log.push_back(where + ": bad toggle line '" + std::string(line) + "'");
            } else {
                rest.append(line);
                rest += '\n';
            }
            pos = end + 1;
        }
        cheats_ = CheatSet::parse(rest);
        log.push_back(std::to_string(cheats_.cheats().size()) + " cheats (" + std::to_string(cheats_.enabled_count()) +
                      " on) from " + where);
        for (const std::string& w : cheats_.warnings()) log.push_back(where + ": " + w);
    } else {
        cheats_ = CheatSet::parse("");
        log.push_back("cannot read " + where);
    }
    apply_preset_states();
    dirty_ = false;
    cheat_sel_ = std::clamp(cheat_sel_, 0, std::max(0, static_cast<int>(cheats_.cheats().size()) - 1));
    // The status line names only the file (the log above has the full path).
    std::string summary = log.front();
    if (const size_t at = summary.rfind(where); at != std::string::npos)
        summary.replace(at, where.size(), path_.filename().string());
    set_status(summary, log.size() > 1);
    return log;
}

void Trainer::set_presets(std::string_view text) {
    presets_ = CheatSet::parse(text);
    apply_preset_states();
    tab_ = presets_.cheats().empty() ? Tab::Battle : Tab::Presets;
}

void Trainer::apply_preset_states() {
    for (size_t i = 0; i < presets_.cheats().size(); ++i) {
        presets_.set_enabled(i, false);
        for (const auto& [name, on] : preset_states_)
            if (name == presets_.cheats()[i].name) presets_.set_enabled(i, on);
    }
}

bool Trainer::save() {
    std::string text = cheats_.text();
    if (!text.empty() && text.back() != '\n') text += '\n';
    std::string presets;
    for (const Cheat& c : presets_.cheats())
        presets += "!preset " + c.name + (c.enabled ? " on\n" : " off\n");
    if (!platform::write_text_file(path_, text + presets + battle_.text() + toggles_.text())) {
        set_status("cannot write " + path_.string(), true);
        return false;
    }
    dirty_ = false;
    set_status("saved " + path_.string());
    return true;
}

ApplyStats Trainer::apply_frame() {
    ApplyStats stats = cheats_.apply(ram_);
    const ApplyStats preset = presets_.apply(ram_);
    stats.writes += preset.writes;
    stats.bytes_changed += preset.bytes_changed;
    if (trace_ && stats.writes > 0)
        std::fprintf(stderr, "[cheats] frame %llu: %zu writes, %zu bytes changed\n",
                     static_cast<unsigned long long>(frame_), stats.writes, stats.bytes_changed);
    ++frame_;
    return stats;
}

void Trainer::set_open(bool open) { open_ = open; }

void Trainer::set_status(std::string text, bool error) {
    status_ = std::move(text);
    status_error_ = error;
}

void Trainer::move(int& sel, int count, Key k) const {
    switch (k) {
    case Key::Up: --sel; break;
    case Key::Down: ++sel; break;
    case Key::PageUp: sel -= 10; break;
    case Key::PageDown: sel += 10; break;
    case Key::Home: sel = 0; break;
    case Key::End: sel = count - 1; break;
    default: break;
    }
    sel = std::clamp(sel, 0, std::max(0, count - 1));
}

void Trainer::key(Key k) {
    if (k == Key::Close) {
        open_ = false;
        return;
    }
    if (k == Key::Tab) {  // Presets (when the game has any) -> Battle -> Custom -> Search
        const bool presets = !presets_.cheats().empty();
        tab_ = tab_ == Tab::Presets  ? Tab::Battle
               : tab_ == Tab::Battle ? Tab::Cheats
               : tab_ == Tab::Cheats ? Tab::Search
               : presets             ? Tab::Presets
                                     : Tab::Battle;
        return;
    }
    if (tab_ == Tab::Presets) presets_key(k);
    else if (tab_ == Tab::Cheats) cheats_key(k);
    else if (tab_ == Tab::Battle) battle_key(k);
    else search_key(k);
}

void Trainer::text(std::string_view chars) {
    for (const char c : chars) {
        if (static_cast<unsigned char>(c) >= 0x80) continue;  // ASCII only
        if (tab_ == Tab::Presets) presets_char(c);
        else if (tab_ == Tab::Cheats) cheats_char(c);
        else if (tab_ == Tab::Battle) battle_char(c);
        else search_char(c);
    }
}

// ---------------------------------------------------------------------------------------------
// Cheats tab
// ---------------------------------------------------------------------------------------------

void Trainer::cheats_key(Key k) {
    const int count = static_cast<int>(cheats_.cheats().size());
    if (k == Key::Enter) {
        cheats_char(' ');
    } else if (k == Key::Delete) {
        if (count == 0) return;
        const std::string name = cheats_.cheats()[static_cast<size_t>(cheat_sel_)].name;
        cheats_.remove(static_cast<size_t>(cheat_sel_));
        dirty_ = true;
        cheat_sel_ = std::clamp(cheat_sel_, 0, std::max(0, count - 2));
        set_status("removed '" + name + "' (S saves)");
    } else {
        move(cheat_sel_, count, k);
    }
}

void Trainer::cheats_char(char c) {
    const int count = static_cast<int>(cheats_.cheats().size());
    if (c == ' ') {
        if (count == 0) return;
        const size_t i = static_cast<size_t>(cheat_sel_);
        const Cheat& cheat = cheats_.cheats()[i];
        const bool on = !cheat.enabled;
        if (!cheats_.set_enabled(i, on)) {
            set_status("cannot enable: " + cheat.error, true);
            return;
        }
        dirty_ = true;
        set_status("'" + cheats_.cheats()[i].name + "' " + (on ? "on" : "off"));
    } else if (c == 'r' || c == 'R') {
        const bool had_changes = dirty_;
        load();
        if (had_changes && !status_error_) set_status(status_ + " (unsaved changes dropped)");
    } else if (c == 's' || c == 'S') {
        save();
    }
}

// ---------------------------------------------------------------------------------------------
// Presets tab
// ---------------------------------------------------------------------------------------------

void Trainer::presets_key(Key k) {
    if (k == Key::Enter) presets_char(' ');
    else move(preset_sel_, static_cast<int>(presets_.cheats().size()), k);
}

void Trainer::presets_char(char c) {
    const size_t count = presets_.cheats().size();
    if (c == ' ' && count > 0) {
        const size_t i = static_cast<size_t>(preset_sel_);
        const bool on = !presets_.cheats()[i].enabled;
        presets_.set_enabled(i, on);
        dirty_ = true;
        set_status("'" + presets_.cheats()[i].name + "' " + (on ? "on" : "off") + " (S saves)");
    } else if (c == 's' || c == 'S') {
        save();
    }
}

// ---------------------------------------------------------------------------------------------
// Battle tab
// ---------------------------------------------------------------------------------------------

void Trainer::battle_step(int delta) {
    const size_t i = static_cast<size_t>(battle_sel_);
    if (i >= battle_.list().size()) return;  // a game toggle: no value
    const BattleAction& a = battle_.list()[i];
    if (!a.has_value()) return;
    const int v = battle_.set_value(i, a.value + delta);
    battle_edit_.clear();
    dirty_ = true;
    set_status(a.label() + " -> " + std::to_string(v) + " (S saves)");
}

void Trainer::battle_key(Key k) {
    const int count = static_cast<int>(battle_.list().size() + toggles_.list().size());
    const size_t i = static_cast<size_t>(battle_sel_);
    switch (k) {
    case Key::Left: battle_step(-10); break;
    case Key::Right: battle_step(10); break;
    case Key::PageUp: battle_step(1000); break;
    case Key::PageDown: battle_step(-1000); break;
    case Key::Enter:
        if (!battle_edit_.empty() && i < battle_.list().size()) {  // typed number: set it
            const BattleAction& a = battle_.list()[i];
            const int typed = std::atoi(battle_edit_.c_str());
            const int v = battle_.set_value(i, typed);
            battle_edit_.clear();
            dirty_ = true;
            set_status(a.label() + " -> " + std::to_string(v) +
                       (v != typed ? " (multiples of 10, 0-" + std::to_string(a.max()) + ")" : "") + " (S saves)");
        } else {
            battle_char(' ');
        }
        break;
    case Key::Backspace:
        if (!battle_edit_.empty()) battle_edit_.pop_back();
        break;
    case Key::Delete: battle_edit_.clear(); break;
    default:
        battle_edit_.clear();
        move(battle_sel_, count, k);
        break;
    }
}

void Trainer::battle_char(char c) {
    const size_t i = static_cast<size_t>(battle_sel_);
    const size_t n = battle_.list().size();
    if (i >= n) {  // a game toggle: Space switches it
        if (c == ' ') {
            const ToggleItem& t = toggles_.list()[i - n];
            toggles_.set_enabled(i - n, !t.enabled);
            dirty_ = true;
            set_status(t.label + (t.enabled ? " on" : " off") + " (S saves)");
        } else if (c == 's' || c == 'S') {
            save();
        } else if (c == 'r' || c == 'R') {
            load();
        }
        return;
    }
    if (c >= '0' && c <= '9') {
        if (battle_.list()[i].has_value() && battle_edit_.size() < 4) battle_edit_ += c;
    } else if (c == ' ') {
        const BattleAction& a = battle_.list()[i];
        battle_.set_enabled(i, !a.enabled);
        dirty_ = true;
        set_status(a.label() + (a.enabled ? " on" : " off") + " (S saves)");
    } else if (c == 's' || c == 'S') {
        save();
    } else if (c == 'r' || c == 'R') {
        load();
    }
}

// ---------------------------------------------------------------------------------------------
// Search tab
// ---------------------------------------------------------------------------------------------

int Trainer::search_rows() const {
    return kSearchControls + static_cast<int>(std::min(search_.count(), kMaxListedResults));
}

void Trainer::search_key(Key k) {
    const int row = search_sel_;
    switch (k) {
    case Key::Left:
    case Key::Right: {
        const int dir = k == Key::Left ? -1 : 1;
        if (row == kRowSize) {
            const int next = std::clamp(size_index_ + dir, 0, 2);
            if (next != size_index_) {
                size_index_ = next;
                if (search_.active()) {
                    search_.reset();
                    set_status("size changed: the search starts over");
                }
            }
        } else if (row == kRowSigned) {
            search_.set_signed(!search_.is_signed());
        } else if (row == kRowFilter) {
            filter_index_ = (filter_index_ + dir + static_cast<int>(kSearchFilterCount)) % static_cast<int>(kSearchFilterCount);
        }
        break;
    }
    case Key::Enter:
        if (row == kRowSigned) {
            search_.set_signed(!search_.is_signed());
        } else if (row == kRowValue || row == kRowFilter) {
            apply_filter();
        } else if (row == kRowNew) {
            search_.start(ram_, size());
            result_scroll_ = 0;
            set_status("new " + std::string(size_name(size())) + " search: " + std::to_string(search_.count()) +
                       " addresses, snapshot taken");
        } else if (row >= kSearchControls) {
            set_status("on a result: F freezes it, W writes the value once");
        }
        break;
    case Key::Backspace:
        if (row == kRowValue && !value_text_.empty()) value_text_.pop_back();
        break;
    case Key::Delete:
        if (row == kRowValue) value_text_.clear();
        break;
    default:
        move(search_sel_, search_rows(), k);
        break;
    }
}

void Trainer::search_char(char c) {
    if (search_sel_ >= kSearchControls) {
        if (c == 'f' || c == 'F') freeze_selected();
        else if (c == 'w' || c == 'W') write_selected();
        return;
    }
    const bool starts_number = (c >= '0' && c <= '9') || c == '-' || c == '$';
    if (search_sel_ != kRowValue) {
        if (!starts_number) return;
        search_sel_ = kRowValue;  // typing a number jumps to the value field
    }
    if (is_value_char(c) && value_text_.size() < 12) value_text_ += c;
}

void Trainer::apply_filter() {
    const SearchFilter f = static_cast<SearchFilter>(filter_index_);
    uint32_t value = 0;
    if (filter_needs_value(f)) {
        const std::optional<uint32_t> v = parse_value(value_text_, size());
        if (!v) {
            set_status(value_text_.empty() ? "type a value first"
                                           : "'" + value_text_ + "' is not a " + size_name(size()) + " value",
                       true);
            return;
        }
        value = *v;
    }
    if (!search_.active()) {
        search_.start(ram_, size());
        if (!filter_needs_value(f)) {
            set_status("snapshot taken: let the game run (F4), then filter again");
            return;
        }
    }
    const size_t n = search_.filter(ram_, f, value);
    result_scroll_ = 0;
    search_sel_ = std::min(search_sel_, search_rows() - 1);
    std::string what = filter_name(f);
    if (filter_needs_value(f)) what.replace(what.find("value"), 5, value_text_);
    set_status(what + ": " + std::to_string(n) + (n == 1 ? " result" : " results"));
}

void Trainer::freeze_selected() {
    const size_t i = static_cast<size_t>(search_sel_ - kSearchControls);
    if (i >= search_.count()) return;
    const uint32_t off = search_.offset(i);
    const ValueSize s = search_.size();
    uint32_t value = read_value(ram_, off, s);
    if (!value_text_.empty()) {
        const std::optional<uint32_t> v = parse_value(value_text_, s);
        if (!v) {
            set_status("'" + value_text_ + "' is not a " + size_name(s) + " value", true);
            return;
        }
        value = *v;
    }
    const std::string name = "Freeze " + hex_address(off) + " = " + format_value(value, s, search_.is_signed());
    std::string error;
    if (!cheats_.add(name, freeze_codes(off, value, static_cast<int>(s)), true, &error)) {
        set_status(error, true);
        return;
    }
    dirty_ = true;
    cheat_sel_ = static_cast<int>(cheats_.cheats().size()) - 1;
    set_status("added '" + name + "' (on); S on the Cheats tab saves it");
}

void Trainer::write_selected() {
    const size_t i = static_cast<size_t>(search_sel_ - kSearchControls);
    if (i >= search_.count()) return;
    const ValueSize s = search_.size();
    const std::optional<uint32_t> v = parse_value(value_text_, s);
    if (!v) {
        set_status(value_text_.empty() ? "type the value to write first"
                                       : "'" + value_text_ + "' is not a " + size_name(s) + " value",
                   true);
        return;
    }
    write_value(ram_, search_.offset(i), s, *v);
    set_status("wrote " + format_value(*v, s, search_.is_signed()) + " to " + hex_address(search_.offset(i)));
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

std::vector<Line> Trainer::render(int cols, int rows) const {
    cols = std::max(cols, 0);
    rows = std::max(rows, 0);
    std::vector<Line> out;
    const auto add = [&](std::string s, Style st = Style::Normal) {
        if (static_cast<int>(out.size()) >= rows) return;
        if (static_cast<int>(s.size()) > cols) s.resize(static_cast<size_t>(cols));
        out.push_back({std::move(s), st});
    };
    // Keeps `sel` inside a window of `visible` rows starting at `scroll`.
    const auto follow = [](int sel, int visible, int& scroll) {
        if (visible <= 0) return;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + visible) scroll = sel - visible + 1;
        scroll = std::max(scroll, 0);
    };

    {
        std::string tabs;
        const auto tab = [&](Tab t, const char* name) { tabs += tab_ == t ? std::string("[") + name + "] " : std::string(" ") + name + "  "; };
        if (!presets_.cheats().empty()) tab(Tab::Presets, "Presets");
        tab(Tab::Battle, "Battle");
        tab(Tab::Cheats, "Custom");
        tab(Tab::Search, "Search");
        add("TRAINER " + tabs + (dirty_ ? " *unsaved" : ""), Style::Title);
    }
    add("Tab: switch  F4/Esc: close  (paused)", Style::Dim);
    add(std::string(static_cast<size_t>(cols), '-'), Style::Dim);
    constexpr int kHeader = 3, kFooter = 3;
    const int body = std::max(0, rows - kHeader - kFooter);
    std::vector<Line> lines;  // body

    if (tab_ == Tab::Presets) {
        const std::vector<Cheat>& list = presets_.cheats();
        lines.push_back({"Built into the port for this game.", Style::Dim});
        lines.push_back({});
        const int visible = body - 3;
        for (int i = 0; i < static_cast<int>(list.size()) && i < visible; ++i) {
            const Cheat& c = list[static_cast<size_t>(i)];
            const bool sel = i == preset_sel_;
            lines.push_back({std::string(sel ? "> " : "  ") + (c.enabled ? "[x] " : "[ ] ") + c.name,
                             sel ? Style::Selected : c.enabled ? Style::Good : Style::Normal});
        }
    } else if (tab_ == Tab::Cheats) {
        const std::vector<Cheat>& list = cheats_.cheats();
        const int detail = body >= 8 ? 3 : 0;
        const int visible = body - detail;
        if (list.empty()) {
            lines.push_back({"No cheats. Add codes to the file below,", Style::Dim});
            lines.push_back({"or freeze a result on the Search tab.", Style::Dim});
        } else {
            follow(cheat_sel_, visible, cheat_scroll_);
            for (int i = cheat_scroll_; i < static_cast<int>(list.size()) && i < cheat_scroll_ + visible; ++i) {
                const Cheat& c = list[static_cast<size_t>(i)];
                const bool sel = i == cheat_sel_;
                std::string row = std::string(sel ? "> " : "  ") + (!c.error.empty() ? "[!] " : c.enabled ? "[x] " : "[ ] ") + c.name;
                lines.push_back({row, sel ? Style::Selected : !c.error.empty() ? Style::Error : Style::Normal});
            }
        }
        while (static_cast<int>(lines.size()) < visible) lines.push_back({});
        if (detail > 0) {
            std::string codes, error;
            if (!list.empty()) {
                const Cheat& c = list[static_cast<size_t>(cheat_sel_)];
                for (const std::string& code : c.codes) codes += (codes.empty() ? "" : "  ") + code;
                error = c.error;
            }
            lines.push_back({codes, Style::Dim});
            lines.push_back({error, Style::Error});
            // Long paths keep their end (the file name) visible.
            std::string file = path_.string();
            const size_t room = static_cast<size_t>(std::max(cols - 6, 4));
            if (file.size() > room) file = "..." + file.substr(file.size() - (room - 3));
            lines.push_back({"File: " + file, Style::Dim});
        }
    } else if (tab_ == Tab::Battle) {
        lines.push_back({"In battle: F10/F11 apply P1/P2 lines, F12 resets all.", Style::Dim});
        lines.push_back({});
        const std::vector<BattleAction>& list = battle_.list();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const BattleAction& a = list[static_cast<size_t>(i)];
            const bool sel = i == battle_sel_;
            std::string value = sel && !battle_edit_.empty() ? battle_edit_ + "_" : std::to_string(a.value);
            char buf[96];
            if (a.is_toggle())
                std::snprintf(buf, sizeof buf, "%s%s      %s", sel ? "> " : "  ", a.enabled ? "[x]" : "[ ]",
                              a.label().c_str());
            else if (!a.has_value())
                std::snprintf(buf, sizeof buf, "%s%s %s  %s", sel ? "> " : "  ", a.enabled ? "[x]" : "[ ]",
                              a.player == 0 ? "F10" : "F11", a.label().c_str());
            else
                std::snprintf(buf, sizeof buf, "%s%s %s  %-17s %5s", sel ? "> " : "  ", a.enabled ? "[x]" : "[ ]",
                              a.player == 0 ? "F10" : "F11", a.label().c_str(), value.c_str());
            lines.push_back({buf, sel ? Style::Selected : a.enabled ? Style::Good : Style::Normal});
            if (i == 4 || i == 9 || i == 11) lines.push_back({});
        }
        lines.push_back({});
        lines.push_back({"Outside battle (held while on):", Style::Dim});
        const int first = static_cast<int>(list.size());
        for (int i = 0; i < static_cast<int>(toggles_.list().size()); ++i) {
            const ToggleItem& t = toggles_.list()[static_cast<size_t>(i)];
            const bool sel = first + i == battle_sel_;
            lines.push_back({std::string(sel ? "> " : "  ") + (t.enabled ? "[x]      " : "[ ]      ") + t.label,
                             sel ? Style::Selected : t.enabled ? Style::Good : Style::Normal});
        }
    } else {
        const int sel = search_sel_;
        const auto control = [&](int row, std::string label, std::string value) {
            lines.push_back({std::string(sel == row ? "> " : "  ") + label + value,
                             sel == row ? Style::Selected : Style::Normal});
        };
        control(kRowSize, "Size    ", std::string("< ") + size_name(size()) + " >");
        control(kRowSigned, "Signed  ", search_.is_signed() ? "< yes >" : "< no >");
        control(kRowValue, "Value   ", "[" + value_text_ + (sel == kRowValue ? "_" : "") + "]");
        control(kRowFilter, "Filter  ", std::string("< ") + filter_name(static_cast<SearchFilter>(filter_index_)) + " >");
        control(kRowNew, "", "[ New search ]");
        const size_t n = search_.count();
        if (!search_.active()) {
            lines.push_back({"No search yet: Enter on a filter or New search", Style::Dim});
        } else {
            lines.push_back({"Results: " + std::to_string(n) +
                                 (n > kMaxListedResults ? " (first " + std::to_string(kMaxListedResults) + " listed)" : ""),
                             Style::Dim});
        }
        const int visible = body - static_cast<int>(lines.size());
        const int listed = static_cast<int>(std::min(n, kMaxListedResults));
        if (sel >= kSearchControls) follow(sel - kSearchControls, visible, result_scroll_);
        result_scroll_ = std::clamp(result_scroll_, 0, std::max(0, listed - 1));
        const ValueSize s = search_.size();
        for (int i = result_scroll_; i < listed && i < result_scroll_ + visible; ++i) {
            const size_t idx = static_cast<size_t>(i);
            const uint32_t off = search_.offset(idx);
            const bool selected = sel == kSearchControls + i;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%s  %-11s was %s", selected ? "> " : "  ", hex_address(off).c_str(),
                          format_value(read_value(ram_, off, s), s, search_.is_signed()).c_str(),
                          format_value(search_.previous(idx), s, search_.is_signed()).c_str());
            lines.push_back({buf, selected ? Style::Selected : Style::Normal});
        }
    }
    for (int i = 0; i < body; ++i) {
        if (static_cast<size_t>(i) < lines.size()) add(lines[static_cast<size_t>(i)].text, lines[static_cast<size_t>(i)].style);
        else add("");
    }
    if (tab_ == Tab::Presets) {
        add("Enter/Space: on/off  S: save", Style::Dim);
        add("Your own codes: the Custom tab", Style::Dim);
    } else if (tab_ == Tab::Cheats) {
        add("Enter/Space: on/off  Del: remove", Style::Dim);
        add("R: reload file  S: save file", Style::Dim);
    } else if (tab_ == Tab::Battle) {
        add("Space: on/off  Left/Right: -/+10  PgUp/PgDn: +/-1000", Style::Dim);
        add("Type a number + Enter to set it  S: save", Style::Dim);
    } else {
        add("Up/Down: select  Left/Right: change  Enter: go", Style::Dim);
        add("On a result: F freeze  W write value once", Style::Dim);
    }
    add(status_, status_error_ ? Style::Error : Style::Good);
    return out;
}

}  // namespace trainer
