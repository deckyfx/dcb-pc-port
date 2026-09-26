#pragma once
// Hardware-register fallback for code that pokes 0x1F801xxx directly instead of going
// through a Psy-Q library function we have replaced. Routes to the GPU/SPU/CD-ROM/pad models.

#include <psx/runtime.hpp>

namespace hle {

class Mmio final : public psx::MmioHandler {
public:
    uint32_t read(uint32_t phys, unsigned width) override;
    void write(uint32_t phys, uint32_t value, unsigned width) override;
};

}  // namespace hle
