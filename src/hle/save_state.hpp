#pragma once
// Whole-machine save states within one run (docs/HOST_MAIN_LOOP.md, "Save states").
//
// A state is taken and loaded by the host between frames, while every game fiber is suspended.
// Layout: a header chunk ("DCBS": format version and this process's session id), then the CPU
// and RAM ("CPU "), the hardware ("MMIO" with GPU, SPU, CD-ROM, SIO0, MDEC inside), the BIOS
// ("BIOS" with the card file system), the tasks and their fibers ("SYS "), and a chunk the host
// adds for its own guest-visible values ("HOST"). States hold host stack addresses, so they are
// rejected by any other process.

#include <psx/runtime.hpp>
#include <psx/state.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hle {

class Bios;
class Mmio;
class System;

/// The objects a save state covers.
struct Guest {
    psx::Machine& machine;
    Mmio& mmio;
    Bios& bios;
    System& system;
};

/// Serialise the machine. `host` writes the caller's own chunk (it may be empty). Throws
/// psx::StateError when a state cannot be taken now (see System::can_save_state).
std::vector<uint8_t> save_guest(const Guest& guest, const std::function<void(psx::StateWriter&)>& host);

/// Load a state made by save_guest in this process. `host` reads the caller's chunk; it should
/// only parse (keep side effects until this returns). All or nothing: when the state is
/// rejected (psx::StateError, with the reason) the machine is put back as it was.
void load_guest(const Guest& guest, std::span<const uint8_t> state, const std::function<void(psx::StateReader&)>& host);

/// This process's session id, stamped into every state.
uint64_t state_session();

}  // namespace hle
