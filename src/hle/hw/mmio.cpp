#include "hw/mmio.hpp"

#include <cstdio>

namespace hle {

uint32_t Mmio::read(uint32_t phys, unsigned width) {
    std::fprintf(stderr, "[mmio] read%u  %08X\n", width * 8, phys);
    return 0;
}

void Mmio::write(uint32_t phys, uint32_t value, unsigned width) {
    std::fprintf(stderr, "[mmio] write%u %08X = %08X\n", width * 8, phys, value);
}

}  // namespace hle
