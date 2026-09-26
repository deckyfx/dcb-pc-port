// dcb — native entry point. Loads the PS-EXE image (for its data, not its code),
// installs the HLE layer, and calls the recompiled entry function.

#include "bios/bios.hpp"
#include "hw/mmio.hpp"

#include <psx/runtime.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>

int main(int argc, char** argv) {
    const std::filesystem::path exe_path =
        argc > 1 ? argv[1] : std::filesystem::path("extracted") / DCB_GAME_ID / "exe" / "boot.exe";

    try {
        static psx::Machine machine;  // 2 MiB of guest RAM lives on the heap; the machine is long-lived
        hle::Bios bios;
        hle::Mmio mmio;
        machine.set_bios_handler(&bios);
        machine.set_mmio_handler(&mmio);

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
