#pragma once
// PC options: a dependency-free INI document, the typed settings it holds (display, audio,
// keyboard / gamepad bindings), where settings.ini lives, and the pure helpers the backends
// use (output rectangle, volume). Nothing here touches SDL; binding names are resolved
// through injected lookups so the logic is unit-testable without a display or audio device.

#include "platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platform {

// ---------------------------------------------------------------------------------------------
// INI document
// ---------------------------------------------------------------------------------------------

/// Line-preserving INI document. Comments (lines starting with ';' or '#'), blank lines,
/// unknown sections/keys and unparseable lines survive a load/save round trip untouched.
/// Section and key lookups are case-insensitive; the last duplicate key wins. There are no
/// inline comments: everything after '=' (trimmed) is the value, so '#' and ';' can be keys.
class IniDocument {
public:
    /// Parse `text` (LF or CRLF, optional UTF-8 BOM). Never fails.
    static IniDocument parse(std::string_view text);

    /// Serialise back to text ('\n' line endings, trailing newline).
    std::string to_string() const;

    /// Value of `key` in `section` ("" = before any section header), if present.
    std::optional<std::string> get(std::string_view section, std::string_view key) const;

    /// Set `key` in `section`: rewrites the existing line in place, otherwise appends the key
    /// to the end of that section (creating the section at the end of the file if needed).
    void set(std::string_view section, std::string_view key, std::string_view value);

    /// Append a comment line ("; text") at the end of `section` (created if needed).
    void add_comment(std::string_view section, std::string_view text);

    /// Append a blank line at the end of the document.
    void add_blank();

    /// Whether a `[section]` header exists.
    bool has_section(std::string_view section) const;

private:
    enum class Kind : uint8_t { Other, Section, Entry };
    struct Line {
        Kind kind = Kind::Other;
        std::string text;     ///< original text (Other/Section) or rendered "key = value" (Entry)
        std::string section;  ///< section this line belongs to (name for Section lines)
        std::string key;
        std::string value;
    };

    /// Index one past the last line belonging to `section`, or npos if the section is absent.
    size_t section_end(std::string_view section) const;
    /// Insert position for new content in `section`, creating the header if needed.
    size_t insert_point(std::string_view section);

    std::vector<Line> lines_;
};

// ---------------------------------------------------------------------------------------------
// Typed settings
// ---------------------------------------------------------------------------------------------

enum class ScaleMode : uint8_t { Integer, Fit };   ///< whole multiples of 320x240, or fill the window
enum class FilterMode : uint8_t { Nearest, Linear };
enum class AspectMode : uint8_t { Ratio4x3, Stretch };

/// One PS1 pad button as named in the [keyboard] / [gamepad] sections.
struct PadButtonInfo {
    const char* name;
    Button bit;
};

inline constexpr size_t kPadButtonCount = 16;

/// Every PS1 pad button, in settings-file order.
inline constexpr std::array<PadButtonInfo, kPadButtonCount> kPadButtons{{
    {"Up", Up},         {"Down", Down},     {"Left", Left},         {"Right", Right},
    {"Cross", Cross},   {"Circle", Circle}, {"Square", Square},     {"Triangle", Triangle},
    {"Start", Start},   {"Select", Select}, {"L1", L1},             {"R1", R1},
    {"L2", L2},         {"R2", R2},         {"L3", L3},             {"R3", R3},
}};

/// Input codes bound to each pad button (index = position in kPadButtons).
using BindingTable = std::array<std::vector<int>, kPadButtonCount>;

inline constexpr int kScaleMin = 1, kScaleMax = 8, kScaleDefault = 3;
inline constexpr int kVolumeMax = 100;
inline constexpr int kDeadzoneDefault = 50;  ///< percent of full stick travel

struct DisplaySettings {
    int scale = kScaleDefault;            ///< initial window size = 320x240 * scale
    bool fullscreen = false;
    ScaleMode scale_mode = ScaleMode::Fit;
    FilterMode filter = FilterMode::Nearest;
    AspectMode aspect = AspectMode::Ratio4x3;
};

struct Settings {
    DisplaySettings display;
    int volume = kVolumeMax;              ///< master volume, 0..100
    int stick_deadzone = kDeadzoneDefault; ///< percent; stick bindings fire beyond it
    BindingTable keyboard;                ///< SDL scancodes
    BindingTable gamepad;                 ///< gamepad input codes (see encode_gamepad_axis)
    std::vector<int> overlay_keys;        ///< [hotkeys] overlay: toggles the performance overlay
    std::vector<int> pause_keys;          ///< [hotkeys] pause: freezes / resumes the game
    std::vector<int> frame_advance_keys;  ///< [hotkeys] frame_advance: while paused, run one frame
    std::vector<int> fast_forward_keys;   ///< [hotkeys] fast_forward: held, the game runs unthrottled
    std::vector<int> scale_mode_keys;     ///< [hotkeys] scale_mode: switches fit / integer scaling
    std::vector<int> trainer_keys;        ///< [hotkeys] trainer: opens / closes the cheats + memory search panel
    std::vector<int> save_state_keys;     ///< [hotkeys] save_state: save into the selected slot
    std::vector<int> load_state_keys;     ///< [hotkeys] load_state: load the selected slot
    std::vector<int> state_slot_keys;     ///< [hotkeys] state_slot: select the next slot (1-4)
};

/// Maps one binding name to an input code, or nullopt if the name is unknown.
using NameResolver = std::function<std::optional<int>(std::string_view)>;

/// Resolvers for the [keyboard] and [gamepad] sections.
struct BindingResolvers {
    NameResolver keyboard;
    NameResolver gamepad;
};

/// Default binding strings (the values written to a fresh settings.ini).
extern const std::array<const char*, kPadButtonCount> kDefaultKeyboardBindings;
extern const std::array<const char*, kPadButtonCount> kDefaultGamepadBindings;

/// Parse a comma-separated binding list ("Z, Return"). Empty / whitespace-only means
/// "unbound" and yields an empty list. Any unknown name makes the whole list invalid
/// (nullopt) and, if `bad_name` is given, stores the first offending name there.
std::optional<std::vector<int>> parse_binding_list(std::string_view value, const NameResolver& resolve,
                                                   std::string* bad_name = nullptr);

/// The settings.ini written on first run: every option at its default, with comments.
IniDocument default_settings_ini();

/// Read typed settings from `doc`. Missing keys take their default silently; invalid values
/// take their default and add a human-readable message to `warnings`. Never throws.
Settings parse_settings(const IniDocument& doc, const BindingResolvers& resolvers,
                        std::vector<std::string>& warnings);

/// Environment lookup (std::getenv-shaped) so overrides are testable.
using EnvLookup = std::function<const char*(const char*)>;

/// Environment overrides, applied on top of the file:
///   DCB_FILTER=nearest|linear   DCB_SCALE=integer|fit
void apply_env_overrides(Settings& settings, const EnvLookup& getenv_fn);

// ---------------------------------------------------------------------------------------------
// Gamepad input codes: buttons are their SDL_GamepadButton value; axis halves are encoded
// above kGamepadAxisBase so a single int covers both.
// ---------------------------------------------------------------------------------------------

inline constexpr int kGamepadAxisBase = 0x1000;

/// Code for one half of gamepad axis `axis` (SDL_GamepadAxis value).
constexpr int encode_gamepad_axis(int axis, bool negative) {
    return kGamepadAxisBase + axis * 2 + (negative ? 1 : 0);
}
constexpr bool is_gamepad_axis(int code) { return code >= kGamepadAxisBase; }
constexpr int gamepad_axis_of(int code) { return (code - kGamepadAxisBase) / 2; }
constexpr bool gamepad_axis_negative(int code) { return ((code - kGamepadAxisBase) & 1) != 0; }

/// Whether a gamepad axis reading (-32768..32767) activates the bound half. Triggers
/// (`is_trigger`) fire above 50%; stick halves fire beyond `deadzone_percent` in their
/// direction.
bool gamepad_axis_pressed(int16_t value, bool negative, bool is_trigger, int deadzone_percent);

// ---------------------------------------------------------------------------------------------
// Settings file location and persistence
// ---------------------------------------------------------------------------------------------

inline constexpr const char* kSettingsFileName = "settings.ini";
inline constexpr const char* kSettingsAppDir = "dcb-pc-port";

/// Inputs to settings-path resolution (captured from the process, or injected by tests).
struct SettingsLocations {
    std::filesystem::path override_path;    ///< DCB_SETTINGS; wins when set
    std::filesystem::path cwd;              ///< current directory (a project / game folder)
    std::filesystem::path exe_dir;          ///< directory holding the executable
    std::filesystem::path appdata;          ///< %APPDATA% (Windows)
    std::filesystem::path xdg_config_home;  ///< $XDG_CONFIG_HOME
    std::filesystem::path home;             ///< $HOME (or %USERPROFILE%)
    bool windows = false;
};

/// Where settings.ini lives: DCB_SETTINGS if set; else ./settings.ini in the current directory
/// if it exists; else <exe_dir>/settings.ini if it exists (portable mode); else the per-user file
/// in home: %APPDATA%\dcb-pc-port\settings.ini on Windows, or $XDG_CONFIG_HOME (falling back to
/// ~/.config)/dcb-pc-port/settings.ini elsewhere. With no usable per-user directory, falls back
/// to the portable location.
std::filesystem::path resolve_settings_path(const SettingsLocations& where,
                                            const std::function<bool(const std::filesystem::path&)>& exists);

/// The real process locations (environment + executable directory).
SettingsLocations current_settings_locations();

/// A settings file on disk: the parsed document plus where it came from.
class SettingsFile {
public:
    /// Load `path`; if it does not exist, write the defaults there first (warnings on failure,
    /// in which case the in-memory defaults are still used).
    static SettingsFile load_or_create(const std::filesystem::path& path, std::vector<std::string>& warnings);

    const IniDocument& document() const { return doc_; }
    const std::filesystem::path& path() const { return path_; }

    /// Change one value and write the file back (comments / unknown keys preserved).
    bool set_and_save(std::string_view section, std::string_view key, std::string_view value);

private:
    std::filesystem::path path_;
    IniDocument doc_;
};

/// Read a whole file; nullopt if it cannot be opened.
std::optional<std::string> read_text_file(const std::filesystem::path& path);
/// Write `text` via a temporary file + rename, creating parent directories. False on failure.
bool write_text_file(const std::filesystem::path& path, std::string_view text);

// ---------------------------------------------------------------------------------------------
// Pure presentation / audio helpers
// ---------------------------------------------------------------------------------------------

/// Destination rectangle inside the output.
struct OutputRect {
    int x = 0, y = 0, w = 0, h = 0;
};

/// Where the 320x240-based picture goes in an `out_w` x `out_h` output. Integer mode uses
/// whole multiples of 320x240 (per axis when stretching), falling back to Fit when the output
/// is smaller than one unit. Fit fills the output (at 4:3 unless stretching). Always centred.
OutputRect compute_output_rect(int out_w, int out_h, ScaleMode mode, AspectMode aspect);

/// Scale `count` s16 samples by `volume_percent` (0 = silence, 100 = unchanged), saturating
/// to the s16 range. `in` and `out` may alias.
void apply_volume(const int16_t* in, int16_t* out, size_t count, int volume_percent);

#if defined(DCB_HAS_SDL3)
/// SDL name lookups (sdl3_input.cpp). Keyboard: SDL scancode names, case-insensitive, plus
/// "Comma" for ','. Gamepad: SDL gamepad button names plus south/east/west/north, triggers
/// ("lefttrigger", optionally "+lefttrigger") and signed stick halves ("-lefty"). Pure table
/// lookups: no SDL_Init needed.
BindingResolvers sdl3_binding_resolvers();

/// Whether SDL_GamepadAxis `axis` is an analog trigger (0..32767) rather than a stick axis.
bool sdl3_axis_is_trigger(int axis);
#endif

}  // namespace platform
