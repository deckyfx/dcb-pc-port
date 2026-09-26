// dcb — native entry point. Loads the PS-EXE image (for its data, not its code),
// installs the HLE layer, and calls the recompiled entry function.

#include "bios/bios.hpp"
#include "cdrom/disc.hpp"
#include "hw/mmio.hpp"
#include "system.hpp"

#include "platform.hpp"

#include <psx/runtime.hpp>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/// DCB_WATCHDOG=<seconds>: abort after that long, so a debugger stops inside whatever loop the
/// game is spinning in (the call stack names the guest functions).
/// DCB_SNAPSHOT=<dir>: save the displayed image every 30 frames as <dir>/frame_NNNNN.ppm.
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

    // Usage: dcb [disc.cue|disc.bin]   (a PS-EXE path is also accepted, for development)
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
        const auto disc_path = hle::Disc::locate(DCB_GAME_ID, disc_hint);
        // The boot executable's code is compiled in; its data comes from the disc, like everything else.
        const std::vector<uint8_t> boot = exe_override.empty() ? hle::Disc(disc_path).read_boot_exe() : std::vector<uint8_t>{};
        mmio.insert_disc(std::make_unique<hle::Disc>(disc_path));
        std::printf("[dcb] disc %s\n", disc_path.string().c_str());
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
        system.on_vblank([&] {
            if (!host->pump_events()) {
                std::printf("[dcb] window closed\n");
                std::exit(0);
            }
            mmio.set_pad_buttons(0, host->pad_buttons(0));
            const hle::Gpu::Display d = mmio.gpu().display();
            platform::DisplayArea area;
            area.x = d.x;
            area.y = d.y;
            area.width = d.width;
            area.height = d.height;
            area.rgb24 = d.rgb24;
            area.enabled = d.enabled;
            host->present(mmio.gpu().vram(), area);
            const std::vector<int16_t>& audio = mmio.take_audio();
            if (!audio.empty()) host->queue_audio(audio.data(), audio.size() / 2);
            // DCB_AUDIO_DUMP=<file>: raw s16le stereo 44100 Hz of everything played (ffmpeg -f s16le -ar 44100 -ac 2).
            static FILE* dump = std::getenv("DCB_AUDIO_DUMP") ? std::fopen(std::getenv("DCB_AUDIO_DUMP"), "wb") : nullptr;
            if (dump && !audio.empty()) std::fwrite(audio.data(), sizeof(int16_t), audio.size(), dump);
            static uint64_t frame = 0;
            snapshot(mmio.gpu().vram(), area, frame++);
        });

        const psx::ExeInfo exe = exe_override.empty() ? machine.load_exe(boot) : machine.load_exe(exe_override);
        std::printf("[dcb] %s: text %08X+%X, entry %08X, %u recompiled functions\n", DCB_GAME_ID, exe.t_addr,
                    exe.t_size, exe.pc0, recomp_function_count);

        if (recomp_function_count == 0) {
            std::printf("[dcb] no recompiled code linked yet: run tools/recomp, then rebuild\n");
            return 0;
        }
        psx_dispatch(&machine.ctx(), exe.pc0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[dcb] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
