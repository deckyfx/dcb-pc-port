// PC options: INI round trip, defaults, typed parsing with fallbacks, binding lists, SDL name
// lookups, settings-path resolution, env overrides, output rectangle and volume scaling.
// Needs no display or audio device (SDL name lookups are static tables).

#include "settings.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

#if defined(DCB_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace platform;

/// Fake resolver: single upper-case letters and a few words map to small integers.
std::optional<int> fake_resolve(std::string_view name) {
    static const std::map<std::string, int> kNames = {{"up", 100}, {"down", 101}, {"return", 102}, {"right shift", 103}};
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0] - 'A';
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto it = kNames.find(lower);
    if (it == kNames.end()) return std::nullopt;
    return it->second;
}

/// Accepts every name (any non-empty string resolves to its length).
std::optional<int> accept_all(std::string_view name) { return static_cast<int>(name.size()); }

BindingResolvers accepting_resolvers() { return BindingResolvers{accept_all, accept_all}; }

size_t index_of(const char* button) {
    for (size_t i = 0; i < kPadButtonCount; ++i)
        if (std::strcmp(kPadButtons[i].name, button) == 0) return i;
    std::fprintf(stderr, "no button %s\n", button);
    std::exit(1);
}

void test_ini_parse_and_round_trip() {
    const std::string text =
        "\xEF\xBB\xBF; leading comment\r\n"
        "top = level\r\n"
        "\n"
        "[Display]\n"
        "# hash comment\n"
        "Scale = 4\n"
        "  unknown_key =  some value  \n"
        "this line is garbage\n"
        "empty =\n"
        "dup = 1\n"
        "dup = 2\n"
        "\n"
        "[mystery]\n"
        "x=y=z\n"
        "semicolon ; not a comment = v\n";
    IniDocument doc = IniDocument::parse(text);

    CHECK(doc.get("", "top") == "level");
    CHECK(doc.get("display", "scale") == "4");     // case-insensitive section and key
    CHECK(doc.get("DISPLAY", "Unknown_Key") == "some value");
    CHECK(doc.get("display", "empty") == "");
    CHECK(doc.get("display", "dup") == "2");       // last duplicate wins
    CHECK(doc.get("mystery", "x") == "y=z");       // only the first '=' splits
    CHECK(doc.get("mystery", "semicolon ; not a comment") == "v");
    CHECK(!doc.get("display", "missing").has_value());
    CHECK(!doc.get("nosuch", "scale").has_value());
    CHECK(doc.has_section("mystery") && !doc.has_section("nosuch"));

    // Unmodified round trip reproduces every line (minus BOM / CR), comments and garbage included.
    const std::string out = doc.to_string();
    CHECK(out.find("; leading comment\n") == 0);
    CHECK(out.find("# hash comment\n") != std::string::npos);
    CHECK(out.find("  unknown_key =  some value  \n") != std::string::npos);
    CHECK(out.find("this line is garbage\n") != std::string::npos);
    CHECK(out.find('\r') == std::string::npos);
    CHECK(IniDocument::parse(out).to_string() == out);

    // set(): in-place rewrite of an existing key, keeping neighbours intact.
    doc.set("display", "scale", "5");
    const std::string after = doc.to_string();
    CHECK(after.find("Scale = 5\n") != std::string::npos);
    CHECK(after.find("# hash comment\n") != std::string::npos);
    CHECK(after.find("this line is garbage\n") != std::string::npos);

    // New key in an existing section lands inside it (before the blank separator).
    doc.set("display", "filter", "linear");
    const IniDocument re = IniDocument::parse(doc.to_string());
    CHECK(re.get("display", "filter") == "linear");
    CHECK(re.get("display", "scale") == "5");
    CHECK(re.get("mystery", "x") == "y=z");
    const std::string s2 = doc.to_string();
    CHECK(s2.find("filter = linear") < s2.find("[mystery]"));

    // New section is appended.
    doc.set("audio", "volume", "40");
    CHECK(IniDocument::parse(doc.to_string()).get("audio", "volume") == "40");
    CHECK(doc.to_string().find("\n\n[audio]\nvolume = 40\n") != std::string::npos);
}

void test_ini_edge_cases() {
    CHECK(IniDocument::parse("").to_string().empty());
    const IniDocument d = IniDocument::parse("[unterminated\nkey=v\n= novalue\n[ spaced ]\nk = 1");
    CHECK(d.get("", "key") == "v");       // "[unterminated" is not a header
    CHECK(d.get("spaced", "k") == "1");   // header names are trimmed; no trailing newline needed
    CHECK(d.to_string().find("= novalue\n") != std::string::npos);  // kept as an opaque line
}

void test_defaults() {
    const IniDocument doc = default_settings_ini();
    const std::string text = doc.to_string();
    CHECK(text.front() == ';');
    CHECK(doc.get("display", "scale") == "3");
    CHECK(doc.get("display", "fullscreen") == "false");
    CHECK(doc.get("display", "scale_mode") == "integer");
    CHECK(doc.get("display", "filter") == "nearest");
    CHECK(doc.get("display", "aspect") == "4:3");
    CHECK(doc.get("audio", "volume") == "100");
    CHECK(doc.get("gamepad", "deadzone") == "50");
    for (size_t i = 0; i < kPadButtonCount; ++i) {
        CHECK(doc.get("keyboard", kPadButtons[i].name) == std::string(kDefaultKeyboardBindings[i]));
        CHECK(doc.get("gamepad", kPadButtons[i].name) == std::string(kDefaultGamepadBindings[i]));
    }
    // Defaults survive a text round trip and parse without warnings.
    CHECK(IniDocument::parse(text).to_string() == text);
    std::vector<std::string> warnings;
    const Settings s = parse_settings(IniDocument::parse(text), accepting_resolvers(), warnings);
    CHECK(warnings.empty());
    CHECK(s.display.scale == 3 && !s.display.fullscreen);
    CHECK(s.display.scale_mode == ScaleMode::Integer && s.display.filter == FilterMode::Nearest);
    CHECK(s.display.aspect == AspectMode::Ratio4x3);
    CHECK(s.volume == 100 && s.stick_deadzone == 50);
    CHECK(s.keyboard[index_of("Select")].size() == 2);  // "Backspace, Right Shift"
    CHECK(s.keyboard[index_of("L3")].empty());          // unbound by default
    CHECK(s.gamepad[index_of("Up")].size() == 2);       // "dpup, -lefty"

    // An empty document yields the same settings, silently.
    const Settings e = parse_settings(IniDocument{}, accepting_resolvers(), warnings);
    CHECK(warnings.empty());
    CHECK(e.display.scale == 3 && e.volume == 100 && e.keyboard == s.keyboard && e.gamepad == s.gamepad);
}

void test_invalid_values_fall_back() {
    const IniDocument doc = IniDocument::parse(
        "[display]\nscale = 9\nfullscreen = maybe\nscale_mode = FIT\nfilter = cubic\naspect = Stretch\n"
        "[audio]\nvolume = -5\n"
        "[gamepad]\ndeadzone = 1x\n"
        "[keyboard]\nCross = Z, NotAKey\nCircle = Up, Return\nL3 =\n");
    std::vector<std::string> warnings;
    const BindingResolvers r{fake_resolve, fake_resolve};
    const Settings s = parse_settings(doc, r, warnings);
    CHECK(s.display.scale == 3);
    CHECK(!s.display.fullscreen);
    CHECK(s.display.scale_mode == ScaleMode::Fit);       // valid (case-insensitive)
    CHECK(s.display.filter == FilterMode::Nearest);
    CHECK(s.display.aspect == AspectMode::Stretch);
    CHECK(s.volume == 100);
    CHECK(s.stick_deadzone == 50);
    CHECK(s.keyboard[index_of("Cross")] == std::vector<int>{'Z' - 'A'});  // default "Z"
    CHECK((s.keyboard[index_of("Circle")] == std::vector<int>{100, 102}));
    CHECK(s.keyboard[index_of("L3")].empty());
    // scale, fullscreen, filter, volume, deadzone, keyboard.Cross; gamepad defaults the fake
    // resolver cannot resolve are silently unbound (they come from the defaults, not the file).
    CHECK(warnings.size() == 6);
    bool mentions_bad_key = false;
    for (const std::string& w : warnings) mentions_bad_key |= w.find("NotAKey") != std::string::npos;
    CHECK(mentions_bad_key);

    // Boundary values are accepted.
    const IniDocument edges = IniDocument::parse("[display]\nscale=8\nfullscreen=YES\n[audio]\nvolume=0\n");
    warnings.clear();
    const Settings e = parse_settings(edges, accepting_resolvers(), warnings);
    CHECK(warnings.empty());
    CHECK(e.display.scale == 8 && e.display.fullscreen && e.volume == 0);
}

void test_hotkeys() {
    // Defaults (a file without [hotkeys]) resolve silently; a bad value warns and keeps the default.
    std::vector<std::string> warnings;
    const BindingResolvers r{fake_resolve, fake_resolve};
    Settings s = parse_settings(IniDocument::parse(""), r, warnings);
    CHECK(warnings.empty());
    CHECK(s.frame_advance_keys == std::vector<int>{'N' - 'A'});
    CHECK(s.pause_keys.empty());  // "P, Pause": the fake resolver knows no "Pause", so the default is unbound

    s = parse_settings(IniDocument::parse("[hotkeys]\npause = Q\nframe_advance = Bogus Key\n"), r, warnings);
    CHECK(s.pause_keys == std::vector<int>{'Q' - 'A'});
    CHECK(s.frame_advance_keys == std::vector<int>{'N' - 'A'});
    CHECK(warnings.size() == 1 && warnings[0].find("frame_advance") != std::string::npos);
}

void test_binding_lists() {
    std::string bad;
    CHECK(parse_binding_list("Z", fake_resolve) == std::vector<int>{25});
    CHECK((parse_binding_list(" Up ,Return,  Right Shift ", fake_resolve) == std::vector<int>{100, 102, 103}));
    CHECK((parse_binding_list("A,,B,", fake_resolve) == std::vector<int>{0, 1}));  // stray commas ignored
    CHECK((parse_binding_list("A, a, A", fake_resolve, &bad) == std::nullopt));    // 'a' unknown to the fake
    CHECK(bad == "a");
    CHECK((parse_binding_list("A, A", fake_resolve) == std::vector<int>{0}));      // duplicates collapse
    CHECK(parse_binding_list("", fake_resolve)->empty());
    CHECK(parse_binding_list("   ", fake_resolve)->empty());
    CHECK(!parse_binding_list("Up, Bogus Key", fake_resolve, &bad).has_value());
    CHECK(bad == "Bogus Key");
    CHECK(!parse_binding_list("Z", NameResolver{}).has_value());  // no resolver: nothing resolves
}

#if defined(DCB_HAS_SDL3)
void test_sdl_names() {
    const BindingResolvers r = sdl3_binding_resolvers();
    // Keyboard: SDL scancode names, case-insensitive, with a Comma alias.
    CHECK(r.keyboard("Z") == SDL_SCANCODE_Z);
    CHECK(r.keyboard("return") == SDL_SCANCODE_RETURN);
    CHECK(r.keyboard("Right Shift") == SDL_SCANCODE_RSHIFT);
    CHECK(r.keyboard("Keypad 8") == SDL_SCANCODE_KP_8);
    CHECK(r.keyboard("Comma") == SDL_SCANCODE_COMMA);
    CHECK(!r.keyboard("NotAKey").has_value());
    CHECK(!r.keyboard("").has_value());
    // Every default keyboard name resolves, and matches SDL's own spelling round trip.
    for (size_t i = 0; i < kPadButtonCount; ++i) {
        const auto codes = parse_binding_list(kDefaultKeyboardBindings[i], r.keyboard);
        CHECK(codes.has_value());
        for (int c : *codes) CHECK(r.keyboard(SDL_GetScancodeName(static_cast<SDL_Scancode>(c))) == c);
    }
    const auto select = parse_binding_list(kDefaultKeyboardBindings[index_of("Select")], r.keyboard);
    CHECK((select == std::vector<int>{SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RSHIFT}));

    // Gamepad: positional aliases, SDL button names, triggers, signed stick halves.
    CHECK(r.gamepad("south") == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(r.gamepad("North") == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(r.gamepad("a") == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(r.gamepad("dpup") == SDL_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(r.gamepad("leftshoulder") == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    CHECK(r.gamepad("lefttrigger") == encode_gamepad_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, false));
    CHECK(r.gamepad("+righttrigger") == encode_gamepad_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, false));
    CHECK(!r.gamepad("-lefttrigger").has_value());
    CHECK(r.gamepad("-lefty") == encode_gamepad_axis(SDL_GAMEPAD_AXIS_LEFTY, true));
    CHECK(r.gamepad("+rightx") == encode_gamepad_axis(SDL_GAMEPAD_AXIS_RIGHTX, false));
    CHECK(!r.gamepad("leftx").has_value());  // stick axis without a direction
    CHECK(!r.gamepad("+bogus").has_value());
    CHECK(!r.gamepad("-").has_value());
    const int code = *r.gamepad("-lefty");
    CHECK(is_gamepad_axis(code) && gamepad_axis_of(code) == SDL_GAMEPAD_AXIS_LEFTY && gamepad_axis_negative(code));
    CHECK(!is_gamepad_axis(*r.gamepad("south")));
    CHECK(sdl3_axis_is_trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) && !sdl3_axis_is_trigger(SDL_GAMEPAD_AXIS_LEFTX));

    // The full default file parses warning-free with the real resolvers, with today's mapping.
    std::vector<std::string> warnings;
    const Settings s = parse_settings(default_settings_ini(), r, warnings);
    CHECK(warnings.empty());
    CHECK(s.keyboard[index_of("Cross")] == std::vector<int>{SDL_SCANCODE_Z});
    CHECK(s.keyboard[index_of("Start")] == std::vector<int>{SDL_SCANCODE_RETURN});
    CHECK(s.keyboard[index_of("R2")] == std::vector<int>{SDL_SCANCODE_2});
    CHECK(s.gamepad[index_of("Circle")] == std::vector<int>{SDL_GAMEPAD_BUTTON_EAST});
    CHECK(s.gamepad[index_of("L2")] == std::vector<int>{encode_gamepad_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, false)});
    CHECK((s.gamepad[index_of("Left")] ==
           std::vector<int>{SDL_GAMEPAD_BUTTON_DPAD_LEFT, encode_gamepad_axis(SDL_GAMEPAD_AXIS_LEFTX, true)}));

    // A user binding with an invalid name falls back to that button's default only.
    warnings.clear();
    const Settings u = parse_settings(
        IniDocument::parse("[keyboard]\nCross = Space, Kpad 5\nCircle = Space, Keypad 5\n[gamepad]\nStart = guide, -leftx\n"),
        r, warnings);
    CHECK(warnings.size() == 1);
    CHECK(u.keyboard[index_of("Cross")] == std::vector<int>{SDL_SCANCODE_Z});
    CHECK((u.keyboard[index_of("Circle")] == std::vector<int>{SDL_SCANCODE_SPACE, SDL_SCANCODE_KP_5}));
    CHECK((u.gamepad[index_of("Start")] ==
           std::vector<int>{SDL_GAMEPAD_BUTTON_GUIDE, encode_gamepad_axis(SDL_GAMEPAD_AXIS_LEFTX, true)}));
}
#endif

void test_axis_thresholds() {
    // Triggers: >50%, positive only.
    CHECK(!gamepad_axis_pressed(16384, false, true, 50));
    CHECK(gamepad_axis_pressed(16385, false, true, 50));
    CHECK(gamepad_axis_pressed(32767, false, true, 5));
    CHECK(!gamepad_axis_pressed(32767, true, true, 50));
    // Sticks: beyond the deadzone in the bound direction.
    CHECK(!gamepad_axis_pressed(16000, false, false, 50));
    CHECK(gamepad_axis_pressed(17000, false, false, 50));
    CHECK(!gamepad_axis_pressed(17000, true, false, 50));
    CHECK(gamepad_axis_pressed(-17000, true, false, 50));
    CHECK(gamepad_axis_pressed(-32768, true, false, 95));
    CHECK(!gamepad_axis_pressed(-30000, true, false, 95));
    CHECK(gamepad_axis_pressed(4000, false, false, 10));
    CHECK(!gamepad_axis_pressed(0, false, false, 0) && !gamepad_axis_pressed(0, true, false, 0));
}

void test_env_overrides() {
    Settings s;
    std::map<std::string, const char*> env;
    const EnvLookup lookup = [&](const char* name) -> const char* {
        const auto it = env.find(name);
        return it == env.end() ? nullptr : it->second;
    };
    apply_env_overrides(s, lookup);
    CHECK(s.display.filter == FilterMode::Nearest && s.display.scale_mode == ScaleMode::Integer);
    env["DCB_FILTER"] = "LINEAR";
    env["DCB_SCALE"] = "fit";
    apply_env_overrides(s, lookup);
    CHECK(s.display.filter == FilterMode::Linear && s.display.scale_mode == ScaleMode::Fit);
    env["DCB_FILTER"] = "nearest";
    env["DCB_SCALE"] = "bogus";  // unknown values leave the file's choice alone
    apply_env_overrides(s, lookup);
    CHECK(s.display.filter == FilterMode::Nearest && s.display.scale_mode == ScaleMode::Fit);
    apply_env_overrides(s, EnvLookup{});
}

void test_settings_path_resolution() {
    std::set<fs::path> existing;
    const auto exists = [&](const fs::path& p) { return existing.count(p) != 0; };

    SettingsLocations linux_where;
    linux_where.exe_dir = "/opt/dcb";
    linux_where.home = "/home/u";
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/home/u/.config/dcb-pc-port/settings.ini"));
    const fs::path xdg = fs::current_path().root_path() / "xdg";  // absolute on every host
    linux_where.xdg_config_home = xdg;
    CHECK(resolve_settings_path(linux_where, exists) == xdg / "dcb-pc-port" / "settings.ini");
    linux_where.xdg_config_home = "relative/xdg";  // ignored per the XDG spec
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/home/u/.config/dcb-pc-port/settings.ini"));

    // Portable mode: a settings.ini next to the executable wins over the per-user one.
    existing.insert(fs::path("/opt/dcb") / "settings.ini");
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/opt/dcb/settings.ini"));

    // A settings.ini in the current directory wins over the portable and per-user ones...
    linux_where.cwd = "/work/dcb";
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/opt/dcb/settings.ini"));  // absent: skipped
    existing.insert(fs::path("/work/dcb") / "settings.ini");
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/work/dcb/settings.ini"));

    // ...and an explicit override (DCB_SETTINGS) wins over everything.
    linux_where.override_path = "/tmp/custom.ini";
    CHECK(resolve_settings_path(linux_where, exists) == fs::path("/tmp/custom.ini"));

    // Windows: %APPDATA%\dcb-pc-port\settings.ini unless portable.
    existing.clear();
    SettingsLocations win;
    win.windows = true;
    win.exe_dir = "C:/Games/dcb";
    win.appdata = "C:/Users/u/AppData/Roaming";
    win.xdg_config_home = "/ignored";
    CHECK(resolve_settings_path(win, exists) ==
          fs::path("C:/Users/u/AppData/Roaming") / "dcb-pc-port" / "settings.ini");
    existing.insert(fs::path("C:/Games/dcb") / "settings.ini");
    CHECK(resolve_settings_path(win, exists) == fs::path("C:/Games/dcb") / "settings.ini");

    // No per-user directory at all: fall back to next to the executable, then the cwd.
    SettingsLocations bare;
    bare.exe_dir = "/opt/dcb";
    CHECK(resolve_settings_path(bare, exists) == fs::path("/opt/dcb/settings.ini"));
    CHECK(resolve_settings_path(SettingsLocations{}, exists) == fs::path("settings.ini"));
    CHECK(resolve_settings_path(bare, nullptr) == fs::path("/opt/dcb/settings.ini"));

    // The real process locations are at least well-formed.
    const SettingsLocations real = current_settings_locations();
    CHECK(!real.exe_dir.empty());
}

void test_settings_file(const char* argv0) {
    // A scratch directory under the build tree (next to the test binary).
    const fs::path dir = fs::absolute(fs::path(argv0)).parent_path() / "test_settings_tmp";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path path = dir / "nested" / "settings.ini";

    std::vector<std::string> warnings;
    SettingsFile created = SettingsFile::load_or_create(path, warnings);
    CHECK(warnings.empty());
    CHECK(fs::is_regular_file(path));  // first run writes the defaults, creating directories
    CHECK(read_text_file(path) == default_settings_ini().to_string());

    // User edits (comments, unknown keys) survive a programmatic change.
    std::string text = *read_text_file(path);
    text += "\n[extra]\n; my note\nfoo = bar\n";
    CHECK(write_text_file(path, text));
    SettingsFile loaded = SettingsFile::load_or_create(path, warnings);
    CHECK(warnings.empty());
    CHECK(loaded.set_and_save("display", "fullscreen", "true"));
    const std::string saved = *read_text_file(path);
    CHECK(saved.find("; my note\nfoo = bar\n") != std::string::npos);
    CHECK(IniDocument::parse(saved).get("display", "fullscreen") == "true");
    CHECK(!fs::exists(dir / "nested" / "settings.ini.tmp"));

    CHECK(!read_text_file(dir / "does-not-exist.ini").has_value());
    fs::remove_all(dir, ec);
}

void test_output_rect() {
    // Integer 4:3: largest whole multiple, centred.
    OutputRect r = compute_output_rect(1920, 1080, ScaleMode::Integer, AspectMode::Ratio4x3);
    CHECK(r.w == 1280 && r.h == 960 && r.x == 320 && r.y == 60);
    r = compute_output_rect(960, 720, ScaleMode::Integer, AspectMode::Ratio4x3);
    CHECK(r.w == 960 && r.h == 720 && r.x == 0 && r.y == 0);
    // Integer stretch: each axis independently.
    r = compute_output_rect(1920, 1080, ScaleMode::Integer, AspectMode::Stretch);
    CHECK(r.w == 1920 && r.h == 960 && r.x == 0 && r.y == 60);
    // Fit 4:3: pillarbox / letterbox.
    r = compute_output_rect(1920, 1080, ScaleMode::Fit, AspectMode::Ratio4x3);
    CHECK(r.w == 1440 && r.h == 1080 && r.x == 240 && r.y == 0);
    r = compute_output_rect(800, 1000, ScaleMode::Fit, AspectMode::Ratio4x3);
    CHECK(r.w == 800 && r.h == 600 && r.x == 0 && r.y == 200);
    // Fit stretch: the whole output.
    r = compute_output_rect(1234, 567, ScaleMode::Fit, AspectMode::Stretch);
    CHECK(r.w == 1234 && r.h == 567 && r.x == 0 && r.y == 0);
    // Smaller than one unit: integer mode degrades to fit.
    r = compute_output_rect(200, 150, ScaleMode::Integer, AspectMode::Ratio4x3);
    CHECK(r.w == 200 && r.h == 150);
    r = compute_output_rect(0, 100, ScaleMode::Fit, AspectMode::Ratio4x3);
    CHECK(r.w == 0 && r.h == 0);
}

void test_volume() {
    const int16_t in[] = {0, 1000, -1000, 32767, -32768, 99};
    int16_t out[6] = {};
    apply_volume(in, out, 6, 100);
    CHECK(std::memcmp(in, out, sizeof in) == 0);
    apply_volume(in, out, 6, 50);
    CHECK(out[0] == 0 && out[1] == 500 && out[2] == -500 && out[3] == 16383 && out[4] == -16384 && out[5] == 49);
    apply_volume(in, out, 6, 0);
    for (int16_t v : out) CHECK(v == 0);
    apply_volume(in, out, 6, -20);  // negative treated as silence
    for (int16_t v : out) CHECK(v == 0);
    // Gains above 100% saturate instead of wrapping.
    apply_volume(in, out, 6, 200);
    CHECK(out[1] == 2000 && out[2] == -2000 && out[3] == 32767 && out[4] == -32768 && out[5] == 198);
    // In-place operation.
    int16_t buf[] = {20000, -20000};
    apply_volume(buf, buf, 2, 200);
    CHECK(buf[0] == 32767 && buf[1] == -32768);
    apply_volume(nullptr, nullptr, 0, 50);  // zero samples: no access
}

}  // namespace

int main(int /*argc*/, char** argv) {
    test_ini_parse_and_round_trip();
    test_ini_edge_cases();
    test_defaults();
    test_invalid_values_fall_back();
    test_binding_lists();
    test_hotkeys();
#if defined(DCB_HAS_SDL3)
    test_sdl_names();
#endif
    test_axis_thresholds();
    test_env_overrides();
    test_settings_path_resolution();
    test_settings_file(argv[0]);
    test_output_rect();
    test_volume();
    std::puts("platform.settings: ok");
    return 0;
}
