#pragma once
// Native replacement for the PS1 kernel's A0/B0/C0 function tables.
// No BIOS image is loaded at runtime: each call the game makes is implemented here.

#include <psx/runtime.hpp>

namespace hle {

class Bios final : public psx::BiosHandler {
public:
    void call(PsxContext& ctx, uint32_t table, uint32_t function) override;
};

}  // namespace hle
