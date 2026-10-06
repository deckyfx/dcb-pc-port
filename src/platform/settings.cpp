// PC options: INI document, typed settings, settings.ini location and pure helpers.
// Deliberately SDL-free (see settings.hpp); sdl3_input.cpp supplies the SDL name lookups.

#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace platform {

namespace {

constexpr int kBaseWidth = 320;   ///< one 4:3 output unit
constexpr int kBaseHeight = 240;
constexpr int kDeadzoneMin = 5, kDeadzoneMax = 95;
constexpr int16_t kTriggerThreshold = 16384;  ///< 50% of trigger travel

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

/// "key = value", or "key =" for an empty value (no trailing whitespace).
std::string render_entry(const std::string& key, const std::string& value) {
    return value.empty() ? key + " =" : key + " = " + value;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

std::optional<int> parse_int(std::string_view s) {
    s = trim(s);
    int value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || s.empty()) return std::nullopt;
    return value;
}

std::optional<bool> parse_bool(std::string_view s) {
    s = trim(s);
    for (const char* t : {"true", "yes", "on", "1"})
        if (iequals(s, t)) return true;
    for (const char* f : {"false", "no", "off", "0"})
        if (iequals(s, f)) return false;
    return std::nullopt;
}

/// Reads keys from one document, recording a warning for every invalid value.
class Reader {
public:
    Reader(const IniDocument& doc, std::vector<std::string>& warnings) : doc_(doc), warnings_(warnings) {}

    std::optional<std::string> raw(const char* section, const char* key) const { return doc_.get(section, key); }

    void warn(const char* section, const char* key, std::string_view value, const std::string& expected,
              const std::string& fallback) {
        std::string msg = "settings: [";
        msg.append(section).append("] ").append(key).append(" = \"").append(value).append("\" is invalid (expected ");
        msg.append(expected).append("); using ").append(fallback.empty() ? "(unbound)" : fallback);
        warnings_.push_back(std::move(msg));
    }

    int integer(const char* section, const char* key, int fallback, int lo, int hi) {
        const auto v = raw(section, key);
        if (!v) return fallback;
        const auto n = parse_int(*v);
        if (n && *n >= lo && *n <= hi) return *n;
        warn(section, key, *v, std::to_string(lo) + "-" + std::to_string(hi), std::to_string(fallback));
        return fallback;
    }

    bool boolean(const char* section, const char* key, bool fallback) {
        const auto v = raw(section, key);
        if (!v) return fallback;
        if (const auto b = parse_bool(*v)) return *b;
        warn(section, key, *v, "true or false", fallback ? "true" : "false");
        return fallback;
    }

    /// One of two spellings; returns true for `second`.
    template <typename E>
    E choice(const char* section, const char* key, E fallback, const char* first, E first_value, const char* second,
             E second_value) {
        const auto v = raw(section, key);
        if (!v) return fallback;
        const std::string_view t = trim(*v);
        if (iequals(t, first)) return first_value;
        if (iequals(t, second)) return second_value;
        warn(section, key, *v, std::string(first) + " or " + second, fallback == first_value ? first : second);
        return fallback;
    }

    BindingTable bindings(const char* section, const std::array<const char*, kPadButtonCount>& defaults,
                          const NameResolver& resolve) {
        BindingTable table;
        for (size_t i = 0; i < kPadButtonCount; ++i) {
            const char* key = kPadButtons[i].name;
            const auto v = raw(section, key);
            std::string bad;
            std::optional<std::vector<int>> parsed;
            if (v) parsed = parse_binding_list(*v, resolve, &bad);
            if (v && !parsed) warn(section, key, *v, "known input names, unknown \"" + bad + "\"", defaults[i]);
            if (!parsed) parsed = parse_binding_list(defaults[i], resolve);
            if (parsed) table[i] = std::move(*parsed);  // a default that cannot resolve stays unbound
        }
        return table;
    }

private:
    const IniDocument& doc_;
    std::vector<std::string>& warnings_;
};

std::filesystem::path env_path(const char* name) {
#if defined(_WIN32)
    // Wide lookup so non-ASCII profile directories survive.
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const wchar_t* v = _wgetenv(wname.c_str());
    return (v != nullptr && *v != L'\0') ? std::filesystem::path(v) : std::filesystem::path();
#else
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::filesystem::path(v) : std::filesystem::path();
#endif
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// IniDocument
// ---------------------------------------------------------------------------------------------

IniDocument IniDocument::parse(std::string_view text) {
    IniDocument doc;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // UTF-8 BOM
    std::string current;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);

        Line line;
        line.text = std::string(raw);
        const std::string_view t = trim(raw);
        if (t.empty() || t.front() == ';' || t.front() == '#') {
            line.kind = Kind::Other;
        } else if (t.front() == '[' && t.find(']') != std::string_view::npos) {
            line.kind = Kind::Section;
            current = std::string(trim(t.substr(1, t.find(']') - 1)));
        } else if (const size_t eq = t.find('='); eq != std::string_view::npos && !trim(t.substr(0, eq)).empty()) {
            line.kind = Kind::Entry;
            line.key = std::string(trim(t.substr(0, eq)));
            line.value = std::string(trim(t.substr(eq + 1)));
        }
        line.section = current;
        doc.lines_.push_back(std::move(line));
    }
    return doc;
}

std::string IniDocument::to_string() const {
    std::string out;
    for (const Line& line : lines_) out.append(line.text).push_back('\n');
    return out;
}

std::optional<std::string> IniDocument::get(std::string_view section, std::string_view key) const {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it)
        if (it->kind == Kind::Entry && iequals(it->section, section) && iequals(it->key, key)) return it->value;
    return std::nullopt;
}

bool IniDocument::has_section(std::string_view section) const {
    return std::any_of(lines_.begin(), lines_.end(),
                       [&](const Line& l) { return l.kind == Kind::Section && iequals(l.section, section); });
}

size_t IniDocument::section_end(std::string_view section) const {
    size_t i = 0;
    if (!section.empty()) {
        while (i < lines_.size() && !(lines_[i].kind == Kind::Section && iequals(lines_[i].section, section))) ++i;
        if (i == lines_.size()) return std::string_view::npos;
        ++i;  // past the header
    }
    while (i < lines_.size() && lines_[i].kind != Kind::Section) ++i;
    return i;
}

size_t IniDocument::insert_point(std::string_view section) {
    size_t end = section_end(section);
    if (end == std::string_view::npos) {
        if (!lines_.empty() && !trim(lines_.back().text).empty()) add_blank();
        Line header;
        header.kind = Kind::Section;
        header.text = "[" + std::string(section) + "]";
        header.section = std::string(section);
        lines_.push_back(std::move(header));
        return lines_.size();
    }
    // New keys go after the section's last content, before the blank separator lines.
    while (end > 0 && lines_[end - 1].kind == Kind::Other && trim(lines_[end - 1].text).empty()) --end;
    return end;
}

void IniDocument::set(std::string_view section, std::string_view key, std::string_view value) {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
        if (it->kind == Kind::Entry && iequals(it->section, section) && iequals(it->key, key)) {
            it->value = std::string(trim(value));
            it->text = render_entry(it->key, it->value);
            return;
        }
    }
    Line line;
    line.kind = Kind::Entry;
    line.section = std::string(section);
    line.key = std::string(trim(key));
    line.value = std::string(trim(value));
    line.text = render_entry(line.key, line.value);
    const size_t at = insert_point(section);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
}

void IniDocument::add_comment(std::string_view section, std::string_view text) {
    Line line;
    line.text = "; " + std::string(text);
    line.section = std::string(section);
    const size_t at = insert_point(section);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
}

void IniDocument::add_blank() {
    Line line;
    line.section = lines_.empty() ? std::string() : lines_.back().section;
    lines_.push_back(std::move(line));
}

// ---------------------------------------------------------------------------------------------
// Typed settings
// ---------------------------------------------------------------------------------------------

const std::array<const char*, kPadButtonCount> kDefaultKeyboardBindings{{
    "Up", "Down", "Left", "Right",          // D-pad
    "Z", "X", "A", "S",                     // Cross Circle Square Triangle
    "Return", "Backspace, Right Shift",     // Start Select
    "Q", "W", "1", "2",                     // L1 R1 L2 R2
    "", "",                                 // L3 R3
}};

const std::array<const char*, kPadButtonCount> kDefaultGamepadBindings{{
    "dpup, -lefty", "dpdown, +lefty", "dpleft, -leftx", "dpright, +leftx",
    "south", "east", "west", "north",
    "start", "back",
    "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
    "leftstick", "rightstick",
}};

std::optional<std::vector<int>> parse_binding_list(std::string_view value, const NameResolver& resolve,
                                                   std::string* bad_name) {
    std::vector<int> codes;
    value = trim(value);
    while (!value.empty()) {
        const size_t comma = value.find(',');
        const std::string_view name = trim(value.substr(0, comma));
        value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        if (name.empty()) continue;  // tolerate "A,,B" and trailing commas
        const std::optional<int> code = resolve ? resolve(name) : std::nullopt;
        if (!code) {
            if (bad_name != nullptr) *bad_name = std::string(name);
            return std::nullopt;
        }
        if (std::find(codes.begin(), codes.end(), *code) == codes.end()) codes.push_back(*code);
    }
    return codes;
}

IniDocument default_settings_ini() {
    IniDocument doc;
    doc.add_comment("", "dcb PC port settings. Lines starting with ';' or '#' are comments.");
    doc.add_comment("", "Invalid values fall back to their default (a warning is logged). Delete this file");
    doc.add_comment("", "to restore every default. DCB_FILTER / DCB_SCALE environment variables override it.");

    doc.add_comment("display", "Initial window size as a multiple of 320x240 (1-8).");
    doc.set("display", "scale", std::to_string(kScaleDefault));
    doc.add_comment("display", "Start fullscreen (Alt+Enter toggles it and is remembered here).");
    doc.set("display", "fullscreen", "false");
    doc.add_comment("display", "fit = fill the window (follows resizes), integer = whole multiples of 320x240");
    doc.add_comment("display", "(sharpest, black borders in between). The scale_mode hotkey switches it.");
    doc.set("display", "scale_mode", "fit");
    doc.add_comment("display", "nearest = crisp pixels, linear = smoothed.");
    doc.set("display", "filter", "nearest");
    doc.add_comment("display", "4:3 = original shape, stretch = fill the window.");
    doc.set("display", "aspect", "4:3");

    doc.add_comment("audio", "Master volume, 0-100.");
    doc.set("audio", "volume", std::to_string(kVolumeMax));

    doc.add_comment("text", "Character names: jp = the Japanese names (Daisuke, Miyako, Iori, Takeru, Hikari,");
    doc.add_comment("text", "Taichi, Yamato, Koushiro, Jou), us = the US ones (Davis, Keely, Cody, T.K., ...).");
    doc.set("text", "names", "jp");

    doc.add_comment("mods", "Gameplay mods. boss_rematch: beaten Battle Arena bosses (Wormmon, Stingmon,");
    doc.add_comment("mods", "Shadramon, the Digimon Emperor, A) can be fought again in their city's Battle Cafe.");
    doc.set("mods", "boss_rematch", "true");
    doc.add_comment("mods", "arena_save: Save in every Battle Arena battle menu, not only at the 4th and 7th.");
    doc.set("mods", "arena_save", "true");

    doc.add_comment("keyboard", "SDL scancode names (e.g. Z, Return, Space, Left Shift, Keypad 8, Comma);");
    doc.add_comment("keyboard", "separate several keys with commas; leave empty to unbind.");
    doc.add_comment("keyboard", "Escape always quits and Alt+Enter always toggles fullscreen.");
    for (size_t i = 0; i < kPadButtonCount; ++i) doc.set("keyboard", kPadButtons[i].name, kDefaultKeyboardBindings[i]);

    doc.add_comment("gamepad", "Buttons: south east west north, dpup dpdown dpleft dpright, start back guide,");
    doc.add_comment("gamepad", "leftshoulder rightshoulder leftstick rightstick, misc1, paddle1-4, touchpad.");
    doc.add_comment("gamepad", "Axes: lefttrigger righttrigger (pressed beyond 50%), and stick halves");
    doc.add_comment("gamepad", "+leftx -leftx +lefty -lefty +rightx -rightx +righty -righty (-lefty = up).");
    doc.add_comment("gamepad", "deadzone: percent of stick travel before a stick half counts as pressed (5-95).");
    doc.set("gamepad", "deadzone", std::to_string(kDeadzoneDefault));
    for (size_t i = 0; i < kPadButtonCount; ++i) doc.set("gamepad", kPadButtons[i].name, kDefaultGamepadBindings[i]);

    doc.add_comment("hotkeys", "Keyboard keys for port features (SDL scancode names, comma-separated).");
    doc.add_comment("hotkeys", "overlay: show/hide FPS, game FPS, CPU/GPU load and audio queue (top right).");
    doc.set("hotkeys", "overlay", "F3");
    doc.add_comment("hotkeys", "pause: freeze / resume the game. frame_advance: while paused, run one frame.");
    doc.set("hotkeys", "pause", "P, Pause");
    doc.set("hotkeys", "frame_advance", "N");
    doc.add_comment("hotkeys", "fast_forward: hold to run the game unthrottled. scale_mode: switch fit / integer.");
    doc.set("hotkeys", "fast_forward", "Tab");
    doc.set("hotkeys", "scale_mode", "F8");
    doc.add_comment("hotkeys", "trainer: cheat codes (cheats/<serial>.txt) and memory search; pauses the game.");
    doc.set("hotkeys", "trainer", "F4");
    doc.add_comment("hotkeys", "menu: the native pause menu (Esc always works too).");
    doc.set("hotkeys", "menu", "F1");
    doc.add_comment("hotkeys", "Save states (kept in memory for this run): save_state / load_state use the selected");
    doc.add_comment("hotkeys", "slot, state_slot selects the next one (1-4).");
    doc.set("hotkeys", "save_state", "F5");
    doc.set("hotkeys", "load_state", "F7");
    doc.set("hotkeys", "state_slot", "F6");
    doc.add_comment("hotkeys", "Battle cheats (during a card battle): battle_p1 / battle_p2 apply the actions that are");
    doc.add_comment("hotkeys", "on in the trainer's Battle tab (HP, attacks, DP, with their values); battle_reset puts");
    doc.add_comment("hotkeys", "every stat they changed back.");
    doc.set("hotkeys", "battle_p1", "F10");
    doc.set("hotkeys", "battle_p2", "F11");
    doc.set("hotkeys", "battle_reset", "F12");
    return doc;
}

Settings parse_settings(const IniDocument& doc, const BindingResolvers& resolvers, std::vector<std::string>& warnings) {
    Reader r(doc, warnings);
    Settings s;
    s.display.scale = r.integer("display", "scale", kScaleDefault, kScaleMin, kScaleMax);
    s.display.fullscreen = r.boolean("display", "fullscreen", false);
    s.display.scale_mode =
        r.choice("display", "scale_mode", ScaleMode::Fit, "integer", ScaleMode::Integer, "fit", ScaleMode::Fit);
    s.display.filter =
        r.choice("display", "filter", FilterMode::Nearest, "nearest", FilterMode::Nearest, "linear", FilterMode::Linear);
    s.display.aspect =
        r.choice("display", "aspect", AspectMode::Ratio4x3, "4:3", AspectMode::Ratio4x3, "stretch", AspectMode::Stretch);
    s.volume = r.integer("audio", "volume", kVolumeMax, 0, kVolumeMax);
    s.stick_deadzone = r.integer("gamepad", "deadzone", kDeadzoneDefault, kDeadzoneMin, kDeadzoneMax);
    s.keyboard = r.bindings("keyboard", kDefaultKeyboardBindings, resolvers.keyboard);
    s.gamepad = r.bindings("gamepad", kDefaultGamepadBindings, resolvers.gamepad);
    // [hotkeys]. Like the pad bindings, defaults resolve silently; only an invalid value from the
    // file warns (files written before a key existed simply get its default).
    const auto hotkey = [&](const char* key, const char* fallback) {
        std::vector<int> keys = parse_binding_list(fallback, resolvers.keyboard, nullptr).value_or(std::vector<int>{});
        if (const std::optional<std::string> value = doc.get("hotkeys", key)) {
            std::string bad;
            if (auto parsed = parse_binding_list(*value, resolvers.keyboard, &bad)) {
                keys = *parsed;
            } else {
                warnings.push_back(std::string("[hotkeys] ") + key + ": unknown key '" + bad + "', using " + fallback);
            }
        }
        return keys;
    };
    s.overlay_keys = hotkey("overlay", "F3");
    s.pause_keys = hotkey("pause", "P, Pause");
    s.frame_advance_keys = hotkey("frame_advance", "N");
    s.fast_forward_keys = hotkey("fast_forward", "Tab");
    s.scale_mode_keys = hotkey("scale_mode", "F8");
    s.trainer_keys = hotkey("trainer", "F4");
    s.menu_keys = hotkey("menu", "F1");
    s.save_state_keys = hotkey("save_state", "F5");
    s.load_state_keys = hotkey("load_state", "F7");
    s.state_slot_keys = hotkey("state_slot", "F6");
    s.battle_p1_keys = hotkey("battle_p1", "F10");
    s.battle_p2_keys = hotkey("battle_p2", "F11");
    s.battle_reset_keys = hotkey("battle_reset", "F12");
    return s;
}

void apply_env_overrides(Settings& settings, const EnvLookup& getenv_fn) {
    if (!getenv_fn) return;
    if (const char* f = getenv_fn("DCB_FILTER")) {
        if (iequals(f, "linear")) settings.display.filter = FilterMode::Linear;
        if (iequals(f, "nearest")) settings.display.filter = FilterMode::Nearest;
    }
    if (const char* s = getenv_fn("DCB_SCALE")) {
        if (iequals(s, "fit")) settings.display.scale_mode = ScaleMode::Fit;
        if (iequals(s, "integer")) settings.display.scale_mode = ScaleMode::Integer;
    }
}

bool gamepad_axis_pressed(int16_t value, bool negative, bool is_trigger, int deadzone_percent) {
    if (is_trigger) return !negative && value > kTriggerThreshold;
    const int dz = std::clamp(deadzone_percent, 0, 100);
    const int threshold = dz * std::numeric_limits<int16_t>::max() / 100;
    return negative ? value < -threshold : value > threshold;
}

// ---------------------------------------------------------------------------------------------
// Location and persistence
// ---------------------------------------------------------------------------------------------

std::filesystem::path resolve_settings_path(const SettingsLocations& where,
                                            const std::function<bool(const std::filesystem::path&)>& exists) {
    if (!where.override_path.empty()) return where.override_path;
    if (!where.cwd.empty() && exists && exists(where.cwd / kSettingsFileName)) return where.cwd / kSettingsFileName;
    const std::filesystem::path portable = where.exe_dir / kSettingsFileName;
    if (!where.exe_dir.empty() && exists && exists(portable)) return portable;

    std::filesystem::path base;
    if (where.windows) {
        base = where.appdata;
    } else if (!where.xdg_config_home.empty() && where.xdg_config_home.is_absolute()) {
        base = where.xdg_config_home;  // the XDG spec says relative values are to be ignored
    } else if (!where.home.empty()) {
        base = where.home / ".config";
    }
    if (base.empty()) return where.exe_dir.empty() ? std::filesystem::path(kSettingsFileName) : portable;
    return base / kSettingsAppDir / kSettingsFileName;
}

std::filesystem::path executable_path() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) break;
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return self;
#endif
    return {};
}

SettingsLocations current_settings_locations() {
    SettingsLocations where;
    where.override_path = env_path("DCB_SETTINGS");
    std::error_code ec;
    where.cwd = std::filesystem::current_path(ec);
    const std::filesystem::path self = executable_path();
    where.exe_dir = self.empty() ? where.cwd : self.parent_path();
#if defined(_WIN32)
    where.windows = true;
    where.appdata = env_path("APPDATA");
    where.home = env_path("USERPROFILE");
#else
    where.xdg_config_home = env_path("XDG_CONFIG_HOME");
    where.home = env_path("HOME");
#endif
    return where;
}

std::optional<bool> parse_setting_bool(std::string_view value) { return parse_bool(value); }

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return std::move(ss).str();
}

bool write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);

    const auto write_to = [&](const std::filesystem::path& target) {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        return !out.fail();
    };

    // Temp file + rename so a crash mid-write never leaves a truncated settings.ini.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    if (write_to(tmp)) {
        std::filesystem::rename(tmp, path, ec);
        if (!ec) return true;
        std::filesystem::remove(tmp, ec);
    }
    return write_to(path);
}

SettingsFile SettingsFile::load_or_create(const std::filesystem::path& path, std::vector<std::string>& warnings) {
    SettingsFile file;
    file.path_ = path;
    if (const auto text = read_text_file(path)) {
        file.doc_ = IniDocument::parse(*text);
        return file;
    }
    file.doc_ = default_settings_ini();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        warnings.push_back("settings: cannot read " + path.string() + "; using defaults");
    } else if (!write_text_file(path, file.doc_.to_string())) {
        warnings.push_back("settings: cannot write defaults to " + path.string());
    }
    return file;
}

bool SettingsFile::set_and_save(std::string_view section, std::string_view key, std::string_view value) {
    doc_.set(section, key, value);
    return write_text_file(path_, doc_.to_string());
}

// ---------------------------------------------------------------------------------------------
// Presentation / audio helpers
// ---------------------------------------------------------------------------------------------

OutputRect compute_output_rect(int out_w, int out_h, ScaleMode mode, AspectMode aspect) {
    if (out_w <= 0 || out_h <= 0) return {};
    int w = 0, h = 0;
    const int sx = out_w / kBaseWidth, sy = out_h / kBaseHeight;
    if (mode == ScaleMode::Integer && aspect == AspectMode::Ratio4x3 && std::min(sx, sy) >= 1) {
        w = kBaseWidth * std::min(sx, sy);
        h = kBaseHeight * std::min(sx, sy);
    } else if (mode == ScaleMode::Integer && aspect == AspectMode::Stretch && sx >= 1 && sy >= 1) {
        w = kBaseWidth * sx;
        h = kBaseHeight * sy;
    } else if (aspect == AspectMode::Stretch) {
        w = out_w;
        h = out_h;
    } else if (out_w * 3 > out_h * 4) {  // wider than 4:3 -> pillarbox
        h = out_h;
        w = out_h * 4 / 3;
    } else {                             // taller -> letterbox
        w = out_w;
        h = out_w * 3 / 4;
    }
    return OutputRect{(out_w - w) / 2, (out_h - h) / 2, w, h};
}

void apply_volume(const int16_t* in, int16_t* out, size_t count, int volume_percent) {
    const int32_t gain = std::max(volume_percent, 0);
    for (size_t i = 0; i < count; ++i) {
        const int32_t v = static_cast<int32_t>(in[i]) * gain / 100;
        out[i] = static_cast<int16_t>(std::clamp<int32_t>(v, std::numeric_limits<int16_t>::min(),
                                                          std::numeric_limits<int16_t>::max()));
    }
}

}  // namespace platform
