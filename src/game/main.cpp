// dcb — native entry point. Loads the PS-EXE image (for its data, not its code),
// installs the HLE layer, and calls the recompiled entry function.

#include "bios/bios.hpp"
#include "cdrom/disc.hpp"
#include "hw/mmio.hpp"
#include "system.hpp"

#include "first_run.hpp"
#include "input_log.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include "trainer.hpp"

#include <psx/runtime.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

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
        // No game data yet: the SDL build asks for the player's dump and imports it (first run).
        const auto disc_path = platform::locate_or_import(DCB_GAME_ID, disc_hint, !std::getenv("DCB_HEADLESS"));
        // The boot executable's code is compiled in; its data comes from the disc, like everything else.
        auto disc = hle::Disc::open(disc_path);
        const std::vector<uint8_t> boot = exe_override.empty() ? disc->read_boot_exe() : std::vector<uint8_t>{};
        std::printf("[dcb] game data: %s\n", disc->describe().c_str());
        mmio.insert_disc(std::move(disc));
        mmio.attach(&system, machine.ctx());
        machine.set_bios_handler(&bios);
        machine.set_mmio_handler(&mmio);
        machine.set_poll_handler(&system);

        // Window, input and audio. DCB_HEADLESS=1 (or a build without SDL3) runs without a window.
        std::unique_ptr<platform::Platform> host;
#ifdef DCB_HAS_SDL3
        if (!std::getenv("DCB_HEADLESS")) host = platform::make_sdl3("Digimon World: Digital Card Arena (PC Port)");
#endif
        if (!host) host = platform::make_headless();
        // DCB_RECORD / DCB_REPLAY: input record and replay (static: std::exit must close the log).
        static platform::InputLog input_log = platform::InputLog::from_env(DCB_GAME_ID);
        // Trainer: cheats/<serial>.txt (DCB_CHEATS) applied at each frame boundary, F4 panel.
        const std::unique_ptr<trainer::Trainer> cheats = trainer::make_trainer(machine.ctx().ram, DCB_GAME_ID);
        host->attach_trainer(cheats.get());
        // Host work after each game frame (the game is suspended at its VBLANK): input for the
        // next frame, present, audio, overlay numbers, debug dumps.
        platform::DisplayArea area;
        const auto guest_frame = [&] {
            // Input. While a movie plays (the MDEC is busy), any key or button skips it: the game
            // itself only accepts Start, so a press becomes a short Start tap.
            static uint64_t pad_frame = 0, mdec_seen = 0, movie_until = 0, skip_until = 0;
            bool any_press = host->take_any_press();
            uint16_t pad = static_cast<uint16_t>(host->pad_buttons(0) & scripted_pad(pad_frame, &any_press));
            if (mmio.mdec_transfers() != mdec_seen) {
                mdec_seen = mmio.mdec_transfers();
                movie_until = pad_frame + 15;
            }
            if (any_press && pad_frame < movie_until) skip_until = pad_frame + 6;
            if (pad_frame < skip_until) pad = static_cast<uint16_t>(pad & ~platform::Start);
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
            const hle::Gpu::Display d = mmio.gpu().display();
            area.x = d.x;
            area.y = d.y;
            area.width = d.width;
            area.height = d.height;
            area.rgb24 = d.rgb24;
            area.enabled = d.enabled;
            const auto present_start = std::chrono::steady_clock::now();
            host->present(mmio.gpu().vram(), area);
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
            const std::vector<int16_t>& audio = mmio.take_audio();
            if (!audio.empty()) host->queue_audio(audio.data(), audio.size() / 2);
            // DCB_AUDIO_DUMP=<file>: raw s16le stereo 44100 Hz of everything played (ffmpeg -f s16le -ar 44100 -ac 2).
            static FILE* dump = std::getenv("DCB_AUDIO_DUMP") ? std::fopen(std::getenv("DCB_AUDIO_DUMP"), "wb") : nullptr;
            if (dump && !audio.empty()) std::fwrite(audio.data(), sizeof(int16_t), audio.size(), dump);
            static uint64_t frame = 0;
            snapshot(mmio.gpu().vram(), area, frame++);
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
        bool paused = false;
        for (;;) {
            if (!host->pump_events()) {
                std::printf("[dcb] window closed\n");
                break;
            }
            const uint32_t commands = host->take_commands();
            if (commands & platform::kTogglePause) {
                paused = !paused;
                host->set_paused(paused);
                if (!paused) system.resync_pacing();  // don't rush to make up the paused time
            }
            if ((paused || cheats->is_open()) && !(commands & platform::kFrameAdvance)) {
                host->present(mmio.gpu().vram(), area);  // the frozen picture (window resizes, overlay)
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            cheats->apply_frame();
            if (!system.resume_guest()) {
                if (!system.error().empty()) throw std::runtime_error(system.error());
                std::printf("[dcb] the game's main program returned\n");
                break;
            }
            guest_frame();
            if (paused || host->fast_forward()) system.resync_pacing();  // unthrottled; no catch-up later
            else system.pace();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[dcb] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
