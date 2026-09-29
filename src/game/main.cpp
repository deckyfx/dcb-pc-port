// dcb — native entry point. Loads the PS-EXE image (for its data, not its code),
// installs the HLE layer, and calls the recompiled entry function.

#include "bios/bios.hpp"
#include "cdrom/disc.hpp"
#include "cdrom/load_log.hpp"
#include "gpu/hd_textures.hpp"
#include "hw/mmio.hpp"
#include "system.hpp"

#include "first_run.hpp"
#include "input_log.hpp"
#include "memcard.hpp"
#include "menu.hpp"
#include "cheat_presets.hpp"
#include "version.hpp"
#include "overrides/battle.hpp"
#include "overrides/fusion.hpp"
#include "overrides/movies.hpp"
#include "overrides/native_files.hpp"
#include "platform.hpp"
#include "save_states.hpp"
#include "settings.hpp"
#include "trainer.hpp"

#include <psx/coverage.h>
#include <psx/runtime.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dcb {
void register_code_names();  // code_names.cpp

/// The release version (CMake project VERSION, DCB_APP_VERSION); the window title uses it.
std::string_view app_version() { return DCB_APP_VERSION; }

/// "Digimon Digital Card Battle PC vx.y.z".
std::string window_title() { return std::string("Digimon Digital Card Battle PC v") + std::string(app_version()); }

}  // namespace dcb

namespace {

/// DCB_WATCHDOG=<seconds>: abort after that long, so a debugger stops inside whatever loop the
/// game is spinning in (the call stack names the guest functions).
/// DCB_SNAPSHOT=<dir>: save the displayed image every 30 frames as <dir>/frame_NNNNN.ppm.
/// DCB_PAD_SCRIPT="<from>-<to>:<Button>[+<Button>],...": hold pad buttons during those VBLANK
/// frames (names as in settings.ini), e.g. "600-610:Start" - scripted input for headless runs.
/// The pseudo-button "Any" stands for pressing some unbound key (sets `*any` on its first frame).
uint16_t scripted_pad(uint64_t frame, bool* any) {
    static const char* script = std::getenv("DCB_PAD_SCRIPT");
    if (!script) return 0xFFFF;
    uint16_t pad = 0xFFFF;
    const std::string text(script);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        unsigned long long from = 0, to = 0;
        const size_t colon = item.find(':');
        if (colon == std::string::npos || std::sscanf(item.c_str(), "%llu-%llu", &from, &to) != 2) continue;
        if (frame < from || frame > to) continue;
        size_t b = colon + 1;
        while (b < item.size()) {
            size_t e = item.find('+', b);
            if (e == std::string::npos) e = item.size();
            const std::string name = item.substr(b, e - b);
            if (name == "Any" && frame == from) *any = true;
            for (const platform::PadButtonInfo& info : platform::kPadButtons)
                if (name == info.name) pad = static_cast<uint16_t>(pad & ~info.bit);
            b = e + 1;
        }
    }
    return pad;
}

void snapshot(const uint16_t* vram, const platform::DisplayArea& area, uint64_t frame) {
    static const char* dir = std::getenv("DCB_SNAPSHOT");
    if (!dir || frame % 30 != 0 || area.width <= 0 || area.height <= 0) return;
    std::vector<uint32_t> rgba(static_cast<size_t>(area.width) * static_cast<size_t>(area.height));
    platform::convert_display(vram, area, rgba.data());
    char name[512];
    std::snprintf(name, sizeof name, "%s/frame_%05llu.ppm", dir, static_cast<unsigned long long>(frame));
    if (FILE* f = std::fopen(name, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", area.width, area.height);
        for (uint32_t px : rgba) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(px), static_cast<unsigned char>(px >> 8),
                                          static_cast<unsigned char>(px >> 16)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    // DCB_SNAPSHOT_VRAM=1: the whole 1024x512 VRAM as well (off-screen buffers, textures). 1.5 MB each.
    static const bool with_vram = std::getenv("DCB_SNAPSHOT_VRAM") != nullptr;
    std::snprintf(name, sizeof name, "%s/vram_%05llu.ppm", dir, static_cast<unsigned long long>(frame));
    if (FILE* f = with_vram ? std::fopen(name, "wb") : nullptr) {
        std::fprintf(f, "P6\n1024 512\n255\n");
        for (int i = 0; i < 1024 * 512; ++i) {
            const uint16_t p = vram[i];
            const unsigned char rgb[3] = {static_cast<unsigned char>((p & 31) << 3), static_cast<unsigned char>(((p >> 5) & 31) << 3),
                                          static_cast<unsigned char>(((p >> 10) & 31) << 3)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    std::fprintf(stderr, "[snap] frame %llu display x=%d y=%d %dx%d rgb24=%d enabled=%d\n",
                 static_cast<unsigned long long>(frame), area.x, area.y, area.width, area.height, area.rgb24, area.enabled);
}

/// Dump the coverage report if DCB_COVERAGE names a file. Free function so both
/// the clean-exit path and the catch block can call it.
void write_coverage_to(const char* path) {
    if (path && psx_coverage_count) {
        if (psx_coverage_write_json(path) == 0)
            std::printf("[dcb] coverage: %u functions -> %s\n", psx_coverage_count, path);
        else
            std::fprintf(stderr, "[dcb] coverage: cannot write %s\n", path);
    }
}

/// Controls page: the backend's binding list, or a headless placeholder.
std::vector<std::string> controls_lines(platform::Platform& host) {
    std::vector<std::string> lines = host.controls_lines();
    if (lines.empty()) lines.push_back("(no window: bindings unavailable)");
    return lines;
}

/// About page: version, build, credits. DCB_VERSION_* come from CMake (git
/// describe); third-party licences ship in the binary via their headers.
std::vector<std::string> about_lines() {
    std::vector<std::string> lines;
    lines.push_back(dcb::window_title() + " (" + DCB_VERSION_STRING + ")");
    lines.push_back(std::string("build ") + DCB_BUILD_TYPE + " " + DCB_PLATFORM_NAME);
    lines.push_back(std::string("game ") + DCB_GAME_ID);
    lines.push_back("");
    lines.push_back("Digimon Card Battle static recompilation.");
    lines.push_back("Needs the player's own disc dump; no game");
    lines.push_back("data is distributed with this program.");
    lines.push_back("");
    lines.push_back("Third party: SDL3 (zlib), stb (public domain / MIT),");
    lines.push_back("pl_mpeg (MIT). See third_party/ for licences.");
    return lines;
}

/// Refresh the menu's States page from SaveStates (occupancy + timestamps +
/// thumbnails).
void refresh_slots(menu::Menu& m, dcb::SaveStates& states) {
    menu::Menu::SlotInfo info[4];
    for (int i = 0; i < 4; ++i) {
        info[i].occupied = states.occupied(i);
        if (info[i].occupied) {
            const dcb::SaveStates::Thumbnail& thumb = states.thumbnail(i);
            const std::string& when = thumb.saved_at;
            info[i].label = when.empty() ? "saved" : when;
            info[i].thumb_w = thumb.width;
            info[i].thumb_h = thumb.height;
            info[i].thumb_rgb = thumb.rgb;
        }
    }
    m.set_slots(info);
}

/// Refresh the menu's Cards page from the save directory.
void refresh_cards(menu::Menu& m) {
    const std::vector<memcard::CardInfo> cards =
        memcard::list_cards(std::filesystem::path("saves") / DCB_GAME_ID);
    std::vector<std::string> names;
    int active = 0;
    for (size_t i = 0; i < cards.size(); ++i) {
        char entry[160];
        std::snprintf(entry, sizeof entry, "%s  (%llu KB)", cards[i].name.c_str(),
                      static_cast<unsigned long long>(cards[i].bytes / 1024));
        names.emplace_back(entry);
        if (!cards[i].is_backup) active = static_cast<int>(i);
    }
    m.set_cards(std::move(names), active);
}

/// Carry out one menu action at the frame boundary. Returns true when a state
/// was loaded (the caller re-reads the display). Save/load are safe at any
/// frame boundary; everything else touches host objects only.
bool handle_menu_action(menu::Action action, platform::Platform& host, menu::Menu& menu, dcb::SaveStates& states,
                        hle::Bios& bios, hle::Mmio& mmio, const platform::DisplayArea& area, trainer::Trainer& cheats) {
    switch (action) {
    case menu::Action::None:
    case menu::Action::Resume: menu.set_open(false); return false;
    case menu::Action::OpenStates: refresh_slots(menu, states); return false;
    case menu::Action::OpenSettings:
    case menu::Action::OpenControls:
    case menu::Action::OpenAbout:
    case menu::Action::OpenHotkeys: return false;  // navigation only
    case menu::Action::OpenTrainer:
        menu.set_open(false);
        cheats.set_open(true);
        return false;
    case menu::Action::OpenCards: refresh_cards(menu); return false;
    case menu::Action::Quit: host.request_quit(); return false;
    case menu::Action::Reset:
        menu.set_open(false);
        return states.reset();  // also during a movie (SaveStates::reset drops it)
    case menu::Action::SaveSlot: {
        const int slot = menu.slot();
        states.select_slot(slot);
        if (dcb::movie_host().active) {
            host.show_message("No save states during movies");
            return false;
        }
        if (states.save(slot)) {
            // Thumbnail: the display area downscaled to <= 96 px wide.
            dcb::SaveStates::Thumbnail thumb;
            const int w = std::clamp(area.width, 0, 1024);
            const int h = std::clamp(area.height, 0, 512);
            if (area.enabled && w > 0 && h > 0 && mmio.gpu().vram() != nullptr) {
                const int tw = std::min(w, 96);
                const int th = std::max(1, h * tw / w);
                std::vector<uint32_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h));
                platform::convert_display(mmio.gpu().vram(), area, rgba.data());
                thumb.width = tw;
                thumb.height = th;
                thumb.rgb.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 3);
                for (int y = 0; y < th; ++y) {
                    for (int x = 0; x < tw; ++x) {
                        const uint32_t px = rgba[(static_cast<size_t>(y) * h / th) * w +
                                                 (static_cast<size_t>(x) * w / tw)];
                        uint8_t* dst = &thumb.rgb[(static_cast<size_t>(y) * tw + x) * 3];
                        dst[0] = static_cast<uint8_t>(px);
                        dst[1] = static_cast<uint8_t>(px >> 8);
                        dst[2] = static_cast<uint8_t>(px >> 16);
                    }
                }
            }
            const std::time_t now = std::time(nullptr);
            char when[32];
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
            thumb.saved_at = when;
            states.set_thumbnail(slot, std::move(thumb));
        }
        refresh_slots(menu, states);
        return false;
    }
    case menu::Action::LoadSlot:
        states.select_slot(menu.slot());
        if (dcb::movie_host().active) {
            host.show_message("No save states during movies");
            return false;
        }
        return states.load(menu.slot());
    // Slot selection lives in the menu (Prev/Next already moved menu.slot());
    // sync SaveStates to it so hotkeys and the menu never disagree.
    case menu::Action::PrevSlot:
    case menu::Action::NextSlot: states.select_slot(menu.slot()); return false;
    case menu::Action::CycleResolution: {
        // 1x -> 2x -> 3x -> 4x -> 8x (3x is the default, so it must be reachable).
        static constexpr int kSteps[5] = {1, 2, 3, 4, 8};
        int cur = host.resolution();
        int next = kSteps[0];
        for (int s : kSteps) {
            if (s > cur) {
                next = s;
                break;
            }
        }
        host.set_resolution(next);
        menu.set_info(menu::Page::Settings, host.settings_lines());
        return false;
    }
    case menu::Action::CycleDisplay:
        // Menu rows: 1 = scale mode, 2 = filter/aspect.
        host.cycle_setting(menu.selection() == 1 ? 0 : 1);
        menu.set_info(menu::Page::Settings, host.settings_lines());
        return false;
    case menu::Action::CycleAudio:
        host.cycle_setting(2);
        menu.set_info(menu::Page::Settings, host.settings_lines());
        return false;
    case menu::Action::ToggleFullscreen:
        host.cycle_setting(3);
        menu.set_info(menu::Page::Settings, host.settings_lines());
        return false;
    case menu::Action::BackupCard: {
        const std::string name = memcard::backup_card(std::filesystem::path("saves") / DCB_GAME_ID);
        host.show_message(name.empty() ? "Backup failed" : "Backed up " + name);
        refresh_cards(menu);
        return false;
    }
    case menu::Action::UseCard: {
        const std::vector<memcard::CardInfo> cards =
            memcard::list_cards(std::filesystem::path("saves") / DCB_GAME_ID);
        const int sel = menu.card_sel();
        // Guards: the game must hold no open card files (a swap would desync
        // its view). Best done on the title screen: save data already loaded
        // into guest RAM stays stale until the game re-reads it — same as
        // swapping a physical card mid-game.
        if (bios.open_card_files() > 0) {
            host.show_message("Close the game's card screen first");
            return false;
        }
        if (sel >= 0 && sel < static_cast<int>(cards.size())) {
            const std::string error =
                memcard::use_card(std::filesystem::path("saves") / DCB_GAME_ID, cards[sel].name);
            if (error.empty()) {
                bios.reload_card(std::filesystem::path("saves") / DCB_GAME_ID);
                host.show_message("Using " + cards[sel].name);
            } else {
                host.show_message("Switch failed: " + error);
            }
            refresh_cards(menu);
        } else {
            host.show_message("Select a card file first");
        }
        return false;
    }
    case menu::Action::PrevCard:
    case menu::Action::NextCard: return false;  // selection only, handled in menu logic
    }
    return false;
}

void arm_watchdog() {
#ifndef _WIN32
    if (const char* s = std::getenv("DCB_WATCHDOG")) {
        std::signal(SIGALRM, [](int) {
            static const char msg[] = "[dcb] watchdog expired\n";
            (void)!write(2, msg, sizeof msg - 1);
            std::abort();
        });
        alarm(static_cast<unsigned>(std::atoi(s)));
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    // The runtime stops with abort() on anything unimplemented; never lose buffered log lines.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    arm_watchdog();
    dcb::register_code_names();  // names for guest call chains in the load / texture logs

    // dcb --import <disc.cue|disc.bin> [dest]: one-time import of the player's dump, then exit.
    if (argc > 1 && std::string(argv[1]) == "--import") return platform::import_command(argc, argv, DCB_GAME_ID);

    // Usage: dcb [extracted-dir|disc.cue|disc.bin]   (a PS-EXE path is also accepted, for development)
    std::filesystem::path disc_hint, exe_override;
    if (argc > 1) {
        const std::filesystem::path arg = argv[1];
        std::string ext = arg.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        (ext == ".exe" || arg.filename().string().find("SLPS_") == 0 ? exe_override : disc_hint) = arg;
    }

    try {
        static psx::Machine machine;  // 2 MiB of guest RAM lives on the heap; the machine is long-lived
        hle::Bios bios;
        hle::Mmio mmio;
        hle::System system(machine.ctx(), mmio, bios);
        bios.attach(&system);
        bios.insert_cards(std::filesystem::path("saves") / DCB_GAME_ID);
        // The shipped zips carry extracted/<id>/ next to the binary; accept it before
        // asking for the player's dump (the stock lookup is CWD-relative only, so a
        // Windows launch from Explorer would otherwise always prompt to import).
        std::filesystem::path exe_extracted =
            platform::current_settings_locations().exe_dir / "extracted" / DCB_GAME_ID;
        if (disc_hint.empty()) {
            std::error_code ex_ec;
            if (!std::filesystem::is_regular_file(exe_extracted / "layout.txt", ex_ec)) exe_extracted.clear();
        }
        // No game data yet: the SDL build asks for the player's dump and imports it (first run).
        const auto disc_path =
            platform::locate_or_import(DCB_GAME_ID, disc_hint.empty() ? exe_extracted : disc_hint,
                                       !std::getenv("DCB_HEADLESS"));
        // The boot executable's code is compiled in; its data comes from the disc, like everything else.
        auto disc = hle::Disc::open(disc_path);
        const std::vector<uint8_t> boot = exe_override.empty() ? disc->read_boot_exe() : std::vector<uint8_t>{};
        std::printf("[dcb] game data: %s\n", disc->describe().c_str());
        mmio.insert_disc(std::move(disc));
        // Named asset-load log for the RE loop (docs/RE_WORKFLOW.md): one line
        // per load with frame + guest cycle. Read-only over disc data, so
        // guest state and timing stay bit-identical.
        if (std::getenv("DCB_LOG_LOADS")) {
            hle::LoadLog::instance().set_enabled(true);
            hle::LoadLog::instance().set_data_dir(("extracted/" + std::string(DCB_GAME_ID)).c_str());
        }
        // Loose assets (en_font.bin, text/, files/): the shipped zips carry
        // assets/<id>/ next to the binary; accept it alongside the CWD one.
        std::string exe_assets;
        {
            const std::filesystem::path p =
                platform::current_settings_locations().exe_dir / "assets" / DCB_GAME_ID;
            std::error_code ea_ec;
            if (std::filesystem::is_directory(p, ea_ec)) exe_assets = p.string();
        }
        dcb::attach_native_files(mmio.disc(), DCB_GAME_ID, exe_assets);
        hle::HdTextures* hd_textures = nullptr;  // for the exit summary
        // Texture replacement (dcb_asset_ripper output). First found wins:
        //   DCB_HD_PACK=<.pak|folder> (+ DCB_HD_MANIFEST=<file> if the manifest lives elsewhere),
        //   <exe-dir>/assets/<id>.pak, assets/<id>.pak (self-contained: manifest inside),
        //   <exe-dir>/assets/converted/<id>/, assets/converted/<id>/ (loose files).
        // The exe-dir entries make the shipped zips portable: the game finds its PAK next
        // to the binary whatever the working directory is (e.g. launched from Explorer).
        // Without any, the game runs exactly as before (every upload commits verbatim).
        {
            const char* env_manifest = std::getenv("DCB_HD_MANIFEST");
            const char* env_pack = std::getenv("DCB_HD_PACK");
            const std::filesystem::path exe_assets =
                platform::current_settings_locations().exe_dir / "assets";
            const auto cand = [&](const std::filesystem::path& base) {
                return std::make_pair(base / (std::string(DCB_GAME_ID) + ".pak"), base / "converted" / DCB_GAME_ID);
            };
            const auto [exe_pak, exe_loose] = cand(exe_assets);
            const auto [cwd_pak, cwd_loose] = cand("assets");
            std::error_code hd_ec;
            std::string art;
            if (env_pack) art = env_pack;
            else if (std::filesystem::is_regular_file(exe_pak, hd_ec)) art = exe_pak.string();
            else if (std::filesystem::is_regular_file(cwd_pak, hd_ec)) art = cwd_pak.string();
            else if (std::filesystem::is_regular_file(exe_loose / hle::HdTextures::kManifestName, hd_ec))
                art = exe_loose.string();
            else if (std::filesystem::is_regular_file(cwd_loose / hle::HdTextures::kManifestName, hd_ec))
                art = cwd_loose.string();
            // Native movies (movie/movie<N>.mpg) come from the same places; a loose folder shadows the pack.
            dcb::attach_movies({exe_pak.string(), cwd_pak.string(), exe_loose.string(), cwd_loose.string(),
                                env_pack ? std::string(env_pack) : std::string()});
            if (!art.empty()) {
                hle::HdTextures* hd = mmio.gpu().install_hd();
                if (hd->load(env_manifest ? env_manifest : "", art)) {
                    std::printf("[dcb] replacement textures: %s\n", art.c_str());
                    hd_textures = hd;
                    // Sprites to draw at another size, for art whose layout differs (sprites.txt).
                    std::vector<uint8_t> text;
                    if (hd->read_art("sprites.txt", text)) {
                        auto rules = hle::Gpu::parse_sprite_scales(std::string(text.begin(), text.end()));
                        std::printf("[dcb] sprite sizes: %zu rule(s) from sprites.txt\n", rules.size());
                        mmio.gpu().set_sprite_scales(std::move(rules));
                    }
                } else {
                    std::printf("[dcb] replacement textures unavailable (continuing without them)\n");
                }
            }
        }
        mmio.attach(&system, machine.ctx());
        machine.set_bios_handler(&bios);
        machine.set_mmio_handler(&mmio);
        machine.set_poll_handler(&system);

        // Window, input and audio. DCB_HEADLESS=1 (or a build without SDL3) runs without a window.
        std::unique_ptr<platform::Platform> host;
#ifdef DCB_HAS_SDL3
        if (!std::getenv("DCB_HEADLESS")) {
            const std::string title = dcb::window_title();
            host = platform::make_sdl3(title.c_str());
        }
#endif
        if (!host) host = platform::make_headless();
        // DCB_RECORD / DCB_REPLAY: input record and replay (static: std::exit must close the log).
        static platform::InputLog input_log = platform::InputLog::from_env(DCB_GAME_ID);
        // Trainer: cheats/<serial>.txt (DCB_CHEATS) applied at each frame boundary, F4 panel.
        const std::unique_ptr<trainer::Trainer> cheats = trainer::make_trainer(machine.ctx().ram, DCB_GAME_ID);
        cheats->set_presets(dcb::cheat_presets());  // the General tab (cheat_presets.cpp)
        std::printf("[cheats] %zu built-in presets (%zu on)\n", cheats->presets().cheats().size(),
                    cheats->presets().enabled_count());
        host->attach_trainer(cheats.get());
        // Host work after each game frame (the game is suspended at its VBLANK): input for the
        // next frame, present, audio, overlay numbers, debug dumps.
        platform::DisplayArea area;
        const auto read_display = [&] {
            const hle::Gpu::Display d = mmio.gpu().display();
            area.x = d.x;
            area.y = d.y;
            area.width = d.width;
            area.height = d.height;
            area.rgb24 = d.rgb24;
            area.enabled = d.enabled;
        };
        dcb::HostFrameState frame_state;  // guest-visible host values: part of every save state
        // Function coverage for the RE loop (docs/RE_WORKFLOW.md): DCB_COVERAGE=<file>
        // writes per-function call counts at exit; DCB_TRACE_CALLS=<n> logs the first
        // n calls live. Sized from the generated tables; arming only appends to
        // host-side counters, so guest state and timing stay bit-identical.
        const char* coverage_path = std::getenv("DCB_COVERAGE");
        const bool coverage_armed = coverage_path != nullptr || std::getenv("DCB_TRACE_CALLS") != nullptr;
        if (coverage_armed) {
            // Sized from the recompiler-emitted name table (boot EXE + overlays);
            // stays disarmed when nothing was emitted.
            psx_coverage_init(psx_coverage_name_count);
            psx_coverage_armed = psx_coverage_count ? 1 : 0;
        }
        const auto guest_frame = [&] {
            // Input. While a movie plays (the MDEC is busy), any key or button skips it: the game
            // itself only accepts Start, so a press becomes a short Start tap.
            auto& [pad_frame, mdec_seen, movie_until, skip_until] = frame_state;
            bool any_press = host->take_any_press();
            uint16_t pad = static_cast<uint16_t>(host->pad_buttons(0) & scripted_pad(pad_frame, &any_press));
            if (mmio.mdec_transfers() != mdec_seen) {
                mdec_seen = mmio.mdec_transfers();
                movie_until = pad_frame + 15;
            }
            if (any_press && pad_frame < movie_until) skip_until = pad_frame + 6;
            if (pad_frame < skip_until) pad = static_cast<uint16_t>(pad & ~platform::Start);
            // A native movie is skipped by any key or Start, like the disc one.
            auto& movie = dcb::movie_host();
            if (movie.active && (any_press || (pad & platform::Start) == 0)) movie.skip = true;
            input_log.apply(pad_frame, pad, any_press);
            mmio.set_pad_buttons(0, pad);
            // DCB_TRACE_INPUT: what the game's pad library last read over the port.
            static const bool trace_input = std::getenv("DCB_TRACE_INPUT") != nullptr;
            static uint16_t sent_seen = 0xFFFF;
            if (trace_input && mmio.pad_sent() != sent_seen) {
                sent_seen = mmio.pad_sent();
                std::fprintf(stderr, "[input] frame %llu: game read pad %04X\n",
                             static_cast<unsigned long long>(pad_frame), sent_seen);
            }
            ++pad_frame;
            read_display();
            const auto present_start = std::chrono::steady_clock::now();
            if (movie.active) {
                movie.player.advance(1.0 / 59.94);  // one game frame of movie time (deterministic)
                const auto& rgb = movie.player.rgb();
                host->present_movie(rgb.empty() ? nullptr : rgb.data(), movie.player.width(), movie.player.height());
            } else {
                host->present(mmio.gpu().vram(), area);
            }
            // Performance overlay: once per second, turn the counters into rates and shares.
            static auto window_start = std::chrono::steady_clock::now();
            static uint64_t frames = 0, flips0 = mmio.display_flips(), gpu0 = mmio.gpu_ns(), sleep0 = system.sleep_ns();
            static uint64_t present_ns = 0;
            const auto now = std::chrono::steady_clock::now();
            present_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - present_start).count());
            ++frames;
            const double wall_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - window_start).count());
            if (wall_ns >= 1e9) {
                const double gpu_ns = static_cast<double>(mmio.gpu_ns() - gpu0);
                const double sleep_ns = static_cast<double>(system.sleep_ns() - sleep0);
                platform::FrameStats st;
                st.fps = static_cast<double>(frames) * 1e9 / wall_ns;
                st.game_fps = static_cast<double>(mmio.display_flips() - flips0) * 1e9 / wall_ns;
                st.gpu_pct = 100.0 * gpu_ns / wall_ns;
                st.cpu_pct = std::max(0.0, 100.0 * (wall_ns - sleep_ns - gpu_ns - static_cast<double>(present_ns)) / wall_ns);
                host->set_stats(st);
                window_start = now;
                frames = 0;
                present_ns = 0;
                flips0 = mmio.display_flips();
                gpu0 = mmio.gpu_ns();
                sleep0 = system.sleep_ns();
            }
            // While a native movie plays, its sound replaces the game's sound output.
            std::vector<int16_t> movie_audio;
            if (movie.active) movie_audio = movie.player.take_audio();
            // DCB_TRACE_MOVIE: per second of wall time, host frames vs movie output (sync debugging).
            static const bool trace_movie = std::getenv("DCB_TRACE_MOVIE") != nullptr;
            if (trace_movie && movie.active) {
                static auto t0 = std::chrono::steady_clock::now();
                static uint64_t frames_s = 0, samples_s = 0;
                ++frames_s;
                samples_s += movie_audio.size() / 2;
                const uint64_t video_s = movie.player.video_frames();
                if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(1)) {
                    std::fprintf(stderr, "[movie] 1s: %llu host frames, %llu audio samples, %llu video frames so far\n",
                                 static_cast<unsigned long long>(frames_s), static_cast<unsigned long long>(samples_s),
                                 static_cast<unsigned long long>(video_s));
                    t0 = std::chrono::steady_clock::now();
                    frames_s = samples_s = 0;
                }
            }
            const std::vector<int16_t>& game_audio = mmio.take_audio();
            const std::vector<int16_t>& audio = movie.active ? movie_audio : game_audio;
            if (!audio.empty()) host->queue_audio(audio.data(), audio.size() / 2);
            // DCB_AUDIO_DUMP=<file>: raw s16le stereo 44100 Hz of everything played (ffmpeg -f s16le -ar 44100 -ac 2).
            static FILE* dump = std::getenv("DCB_AUDIO_DUMP") ? std::fopen(std::getenv("DCB_AUDIO_DUMP"), "wb") : nullptr;
            if (dump && !audio.empty()) std::fwrite(audio.data(), sizeof(int16_t), audio.size(), dump);
            static uint64_t frame = 0;
            snapshot(mmio.gpu().vram(), area, frame);
            ++frame;
            // RE tracing stamps for the *next* frame's guest code (coverage +
            // load log share the host frame counter). One predictable branch
            // when everything is off.
            static const bool tracing =
                coverage_armed || hle::LoadLog::instance().enabled() || std::getenv("DCB_LOG_TEX") ||
                std::getenv("DCB_WATCH") || std::getenv("DCB_WATCH_BATTLE");
            if (tracing) {
                psx_coverage_frame = frame;
                psx_watch_frame = frame;
                hle::LoadLog::instance().set_frame(frame, machine.ctx().cycles);
                hle::LoadLog::instance().mdec_frame(mmio.mdec_transfers());
                hle::LoadLog::instance().flush();
            }
        };

        const psx::ExeInfo exe = exe_override.empty() ? machine.load_exe(boot) : machine.load_exe(exe_override);
        std::printf("[dcb] %s: text %08X+%X, entry %08X, %u recompiled functions\n", DCB_GAME_ID, exe.t_addr,
                    exe.t_size, exe.pc0, recomp_function_count);

        if (recomp_function_count == 0) {
            std::printf("[dcb] no recompiled code linked yet: run tools/recomp, then rebuild\n");
            return 0;
        }

        // The host loop: the game runs on its own fibers, one frame per resume_guest(), and this
        // thread owns everything in between (docs/HOST_MAIN_LOOP.md).
        system.start(exe.pc0);
        dcb::SaveStates states({machine, mmio, bios, system}, frame_state, input_log, *host);
        bool paused = false, frozen = false;  // frozen: the host held the game (pause, trainer panel)
        bool menu_was_open = false;  // edge-detect menu open for the slot sync below
        for (;;) {
            if (!host->pump_events()) {
                std::printf("[dcb] window closed\n");
                break;
            }
            uint32_t commands = host->take_commands();
            if (dcb::movie_host().active && (commands & (platform::kSaveState | platform::kLoadState))) {
                host->show_message("No save states during movies");  // the player's state is host-side
                commands &= ~(platform::kSaveState | platform::kLoadState);
            }
            if (const std::string notice = dcb::battle_hotkeys(machine.ctx(), commands, cheats->battle());
                !notice.empty())
                host->show_message(notice);  // F10-F12 in a card battle (overrides/battle.cpp)
            dcb::game_toggles_frame(machine.ctx(), cheats->toggles());  // overrides/fusion.cpp
            if (states.handle(commands)) {  // save states: between frames, also while paused
                read_display();
                host->present(mmio.gpu().vram(), area);
            }
            if (commands & platform::kTogglePause) {
                paused = !paused;
                host->set_paused(paused);
            }
            const bool menu_open = host->menu_open();
            // Menu actions run inside the frozen branch (so Save/Settings/cards
            // act while the menu is open) and also on the running path (an
            // action queued on the exact frame the menu closed).
            // Menu just opened this frame: adopt the hotkey-selected slot so
            // the menu and F5/F7 can never disagree (either direction).
            if (menu_open && !menu_was_open) {
                if (menu::Menu* menu = host->menu()) menu->set_slot(states.selected_slot());
            }
            menu_was_open = menu_open;
            const auto run_menu_actions = [&] {
                if (menu::Menu* menu = host->menu()) {
                    if (menu->is_open()) {
                        menu->set_info(menu::Page::Settings, host->settings_lines());
                        menu->set_info(menu::Page::Controls, controls_lines(*host));
                        menu->set_info(menu::Page::About, about_lines());
                        menu->set_info(menu::Page::Hotkeys, host->hotkey_lines());
                    }
                    bool reloaded = false;
                    for (menu::Action action = static_cast<menu::Action>(host->take_menu_action());
                         action != menu::Action::None;
                         action = static_cast<menu::Action>(host->take_menu_action())) {
                        if (handle_menu_action(action, *host, *menu, states, bios, mmio, area, *cheats)) reloaded = true;
                    }
                    if (reloaded) {
                        read_display();
                        host->present(mmio.gpu().vram(), area);
                    }
                }
            };
            if ((paused || cheats->is_open() || menu_open) && !(commands & platform::kFrameAdvance)) {
                run_menu_actions();
                const auto& still = dcb::movie_host().player.rgb();
                if (dcb::movie_host().active)  // the frozen picture (window resizes, overlay)
                    host->present_movie(still.empty() ? nullptr : still.data(), dcb::movie_host().player.width(),
                                        dcb::movie_host().player.height());
                else
                    host->present(mmio.gpu().vram(), area);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                frozen = true;
                continue;
            }
            run_menu_actions();
            cheats->apply_frame();
            if (!system.resume_guest()) {
                if (!system.error().empty()) throw std::runtime_error(system.error());
                std::printf("[dcb] the game's main program returned\n");
                break;
            }
            guest_frame();
            states.frame_done();
            if (states.exit_requested()) {
                std::printf("[dcb] DCB_EXIT_AT: stopping after %s frames\n", std::getenv("DCB_EXIT_AT"));
                break;
            }
            // After a hold or while unthrottled, line the clocks up instead of sleeping off the time
            // gained or rushing to make up the time lost.
            if (paused || frozen || host->fast_forward()) system.resync_pacing();
            else system.pace();
            frozen = false;
        }
        std::printf("[cd] sectors read through the CD drive: %llu data, %llu streamed (movie)\n",
                    static_cast<unsigned long long>(mmio.cd_sectors_read()),
                    static_cast<unsigned long long>(mmio.cd_sectors_streamed()));
        if (hd_textures)
            std::printf("[hd] %llu texture uploads replaced (%llu from the cache), %llu left as they were\n",
                        static_cast<unsigned long long>(hd_textures->hits()),
                        static_cast<unsigned long long>(hd_textures->fit_hits()),
                        static_cast<unsigned long long>(hd_textures->misses()));
        if (hd_textures && hd_textures->misses())
            std::printf("[hd] of those: %llu shape mismatch, %llu no palette, %llu palette not live, "
                        "%llu palette shape; the rest match no manifest entry (DCB_TRACE_HD=<n> lists them)\n",
                        static_cast<unsigned long long>(hd_textures->miss_shape()),
                        static_cast<unsigned long long>(hd_textures->miss_no_palette()),
                        static_cast<unsigned long long>(hd_textures->miss_palette_not_live()),
                        static_cast<unsigned long long>(hd_textures->miss_palette_shape()));
        write_coverage_to(coverage_path);  // clean exits (window close, program return, DCB_EXIT_AT)
    } catch (const std::exception& e) {
        // coverage_path is out of scope here; re-read the env (same value).
        write_coverage_to(std::getenv("DCB_COVERAGE"));  // fatal errors still dump what was recorded
        std::fprintf(stderr, "[dcb] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
