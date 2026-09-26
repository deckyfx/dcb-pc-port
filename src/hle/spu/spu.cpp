#include "spu/spu.hpp"

namespace hle {

namespace {

constexpr uint32_t kTransferAddr = 0x1F801DA6;  // in 8-byte units
constexpr uint32_t kTransferFifo = 0x1F801DA8;
constexpr uint32_t kSpuCnt = 0x1F801DAA;
constexpr uint32_t kSpuStat = 0x1F801DAE;

}  // namespace

uint16_t Spu::read16(uint32_t phys) const {
    if (phys == kSpuStat) {
        // Bits 0-5 mirror SPUCNT (libspu waits for this), bit 7 mirrors the DMA request mode.
        // Transfers finish instantly, so the busy flag (bit 10) is never set.
        const uint16_t cnt = reg(kSpuCnt);
        return static_cast<uint16_t>((cnt & 0x3Fu) | ((cnt & 0x20u) << 2));
    }
    return reg(phys);
}

void Spu::write16(uint32_t phys, uint16_t value) {
    reg(phys) = value;
    if (phys == kTransferAddr) {
        transfer_addr_ = (static_cast<uint32_t>(value) * 8u) % kRamSize;
    } else if (phys == kTransferFifo) {  // manual (non-DMA) upload
        ram_[transfer_addr_] = static_cast<uint8_t>(value);
        ram_[(transfer_addr_ + 1) % kRamSize] = static_cast<uint8_t>(value >> 8);
        transfer_addr_ = (transfer_addr_ + 2) % kRamSize;
    }
}

void Spu::dma_write(const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        for (int b = 0; b < 4; ++b) {
            ram_[transfer_addr_] = static_cast<uint8_t>(words[i] >> (8 * b));
            transfer_addr_ = (transfer_addr_ + 1) % kRamSize;
        }
    }
}

void Spu::dma_read(uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            w |= static_cast<uint32_t>(ram_[transfer_addr_]) << (8 * b);
            transfer_addr_ = (transfer_addr_ + 1) % kRamSize;
        }
        words[i] = w;
    }
}

}  // namespace hle
