#pragma once
// Host-side runtime API: owns guest memory, loads the PS-EXE, and lets the HLE layer
// plug in behind MMIO and BIOS calls without the runtime depending on it.

#include <psx/recomp.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace psx {

/// Memory-mapped I/O handler (0x1F801000-0x1F802FFF). Installed by the HLE layer.
struct MmioHandler {
    virtual ~MmioHandler() = default;
    virtual uint32_t read(uint32_t phys, unsigned width) = 0;
    virtual void write(uint32_t phys, uint32_t value, unsigned width) = 0;
};

/// BIOS function-table handler for jumps to 0xA0 / 0xB0 / 0xC0 (function number in $t1).
struct BiosHandler {
    virtual ~BiosHandler() = default;
    virtual void call(PsxContext& ctx, uint32_t table, uint32_t function) = 0;
};

/// Periodic hook for timing and interrupt delivery (called from generated loops and I/O polls).
struct PollHandler {
    virtual ~PollHandler() = default;
    virtual void poll(PsxContext& ctx) = 0;
};

/// Parsed PS-EXE header fields the runtime needs to start the game.
struct ExeInfo {
    uint32_t pc0 = 0, gp0 = 0, t_addr = 0, t_size = 0, b_addr = 0, b_size = 0, s_addr = 0, s_size = 0;
};

/// Guest machine: CPU context plus the memory it points into.
class Machine {
public:
    Machine();
    Machine(const Machine&) = delete;             // ctx_.host points at this object
    Machine& operator=(const Machine&) = delete;

    PsxContext& ctx() { return ctx_; }

    /// Copy a PS-EXE's load image into RAM, clear BSS, and seed pc/gp/sp. Throws on malformed input.
    ExeInfo load_exe(const std::filesystem::path& path);

    void set_mmio_handler(MmioHandler* handler) { mmio_ = handler; }
    void set_bios_handler(BiosHandler* handler) { bios_ = handler; }
    void set_poll_handler(PollHandler* handler) { poll_ = handler; }
    PollHandler* poll_handler() const { return poll_; }
    MmioHandler* mmio() const { return mmio_; }
    BiosHandler* bios() const { return bios_; }

    /// The machine the C entry points (psx_slow_*, psx_dispatch) operate on.
    static Machine& from(PsxContext* ctx);

private:
    PsxContext ctx_{};
    std::unique_ptr<std::array<uint8_t, PSX_RAM_SIZE>> ram_;
    std::array<uint8_t, PSX_SCRATCH_SIZE> scratch_{};
    MmioHandler* mmio_ = nullptr;
    BiosHandler* bios_ = nullptr;
    PollHandler* poll_ = nullptr;
};

/// Look up a recompiled function by guest address (binary search over the generated table).
RecompFunc find_function(uint32_t addr);

/// Call guest code from native code the way the kernel does (event callbacks, interrupt
/// handlers): all CPU registers are preserved around the call; returns the guest's $v0.
uint32_t call_guest(PsxContext& ctx, uint32_t addr, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
                    uint32_t a3 = 0);

}  // namespace psx
