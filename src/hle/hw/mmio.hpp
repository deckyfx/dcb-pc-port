#pragma once
// Hardware-register fallback for code that pokes 0x1F801xxx directly instead of going through a
// Psy-Q function we have replaced. Holds register state so status polls and busy-waits resolve;
// device behaviour (GPU drawing, SPU, CD-ROM) plugs in here as those layers are built.
// Reference: psx-spx "I/O Map", "Interrupts", "DMA Channels", "Timers", "GPU I/O Ports".

#include "cdrom/cdrom.hpp"
#include "gpu/gpu.hpp"
#include "mdec/mdec.hpp"
#include "pad/sio.hpp"
#include "spu/spu.hpp"

#include <psx/runtime.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <utility>

namespace hle {

class System;

class Mmio final : public psx::MmioHandler {
public:
    Mmio();

    uint32_t read(uint32_t phys, unsigned width) override;
    void write(uint32_t phys, uint32_t value, unsigned width) override;

    /// Raise an interrupt line (0 = VBLANK, 1 = GPU, 2 = CDROM, 3 = DMA, 4-6 = timers, 7 = pads).
    void raise_irq(unsigned line) { i_stat_ |= 1u << line; }
    uint32_t pending_irqs() const { return i_stat_ & i_mask_; }

    /// Timers read the system clock, and status reads give the system a chance to deliver IRQs.
    void insert_disc(std::unique_ptr<Disc> disc) { cdrom_.insert(std::move(disc)); }
    void set_pad_buttons(unsigned port, uint16_t buttons) { sio_.set_buttons(port, buttons); }
    /// Advance time-driven devices (CD-ROM responses and sectors) to guest time `cycles`.
    void tick(uint64_t cycles) { cdrom_.tick(cycles); }
    /// Once per frame: the GPU's interlace field flips.
    void vblank() { gpu_.vblank(); }

    const Gpu& gpu() const { return gpu_; }

    void attach(System* system, PsxContext& ctx) {
        system_ = system;
        ctx_ = &ctx;
    }

private:
    // Interrupt controller
    uint32_t i_stat_ = 0, i_mask_ = 0;
    // DMA: 7 channels x {MADR, BCR, CHCR}, plus DPCR/DICR
    std::array<std::array<uint32_t, 3>, 7> dma_{};
    uint32_t dpcr_ = 0x07654321u, dicr_ = 0;
    // Timers: mode/target registers, and the clock value at the last counter reset
    struct Timer {
        uint32_t mode = 0, target = 0;
        uint64_t base = 0;
    };
    std::array<Timer, 3> timer_{};
    System* system_ = nullptr;
    PsxContext* ctx_ = nullptr;  ///< guest RAM for DMA
    Spu spu_;
    CdRom cdrom_;
    Sio0 sio_;
    Gpu gpu_;
    Mdec mdec_;
    // Everything else: plain storage so read-after-write works
    std::map<uint32_t, uint32_t> misc_;
    // Log each unhandled (address, direction) only a few times
    std::map<std::pair<uint32_t, bool>, unsigned> logged_;

    uint64_t timer_clock(unsigned index) const;
    uint32_t timer_counter(unsigned index) const;
    void gp0(uint32_t word);
    void dma_write(unsigned channel, unsigned reg, uint32_t value);
    void dma_run(unsigned channel);
    void dma_gpu_linked_list(uint32_t addr);
    void note(uint32_t phys, bool write, uint32_t value, unsigned width);
};

}  // namespace hle
