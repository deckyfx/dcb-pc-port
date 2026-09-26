#pragma once
// SPU register file and sound RAM. Audio output comes later; for now this holds the state libspu
// programs (so status polls and transfers behave) and receives DMA uploads of samples.
// Reference: psx-spx "Sound Processing Unit (SPU)".

#include <array>
#include <cstdint>
#include <vector>

namespace hle {

class Spu {
public:
    static constexpr uint32_t kBase = 0x1F801C00, kEnd = 0x1F802000;
    static constexpr uint32_t kRamSize = 512 * 1024;

    Spu() : ram_(kRamSize, 0) {}

    uint16_t read16(uint32_t phys) const;
    void write16(uint32_t phys, uint16_t value);

    /// DMA channel 4, RAM -> SPU: append words at the current transfer address.
    void dma_write(const uint32_t* words, uint32_t count);
    /// DMA channel 4, SPU -> RAM.
    void dma_read(uint32_t* words, uint32_t count);

private:
    std::array<uint16_t, (kEnd - kBase) / 2> regs_{};
    std::vector<uint8_t> ram_;
    uint32_t transfer_addr_ = 0;  ///< byte address in sound RAM

    uint16_t& reg(uint32_t phys) { return regs_[(phys - kBase) / 2]; }
    uint16_t reg(uint32_t phys) const { return regs_[(phys - kBase) / 2]; }
};

}  // namespace hle
