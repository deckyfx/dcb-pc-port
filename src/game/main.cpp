// dcb — native entry point. Loads the PS-EXE image (for its data, not its code),
// installs the HLE layer, and calls the recompiled entry function.

#include "bios/bios.hpp"
#include "cdrom/disc.hpp"
#include "hw/mmio.hpp"
#include "system.hpp"

#include <psx/runtime.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/// DCB_WATCHDOG=<seconds>: abort after that long, so a debugger stops inside whatever loop the
/// game is spinning in (the call stack names the guest functions).
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

    const std::filesystem::path exe_path =
        argc > 1 ? argv[1] : std::filesystem::path("extracted") / DCB_GAME_ID / "exe" / "boot.exe";

    try {
        static psx::Machine machine;  // 2 MiB of guest RAM lives on the heap; the machine is long-lived
        hle::Bios bios;
        hle::Mmio mmio;
        hle::System system(machine.ctx(), mmio, bios);
        bios.attach(&system);
        bios.insert_cards(std::filesystem::path("saves") / DCB_GAME_ID);
        const auto disc_path = hle::Disc::locate(DCB_GAME_ID);
        mmio.insert_disc(std::make_unique<hle::Disc>(disc_path));
        std::printf("[dcb] disc %s\n", disc_path.string().c_str());
        mmio.attach(&system, machine.ctx());
        machine.set_bios_handler(&bios);
        machine.set_mmio_handler(&mmio);
        machine.set_poll_handler(&system);

        const psx::ExeInfo exe = machine.load_exe(exe_path);
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
