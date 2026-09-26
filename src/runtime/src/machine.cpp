#include <psx/runtime.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace psx {

namespace {

constexpr size_t kExeHeaderSize = 0x800;

uint32_t le32(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
           static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

}  // namespace

Machine::Machine() : ram_(std::make_unique<std::array<uint8_t, PSX_RAM_SIZE>>()) {
    ram_->fill(0);
    ctx_.ram = ram_->data();
    ctx_.scratch = scratch_.data();
    ctx_.host = this;
}

Machine& Machine::from(PsxContext* ctx) {
    return *static_cast<Machine*>(ctx->host);
}

ExeInfo Machine::load_exe(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return load_exe(std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}});
}

ExeInfo Machine::load_exe(const std::vector<uint8_t>& data) {
    if (data.size() < kExeHeaderSize || std::memcmp(data.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error("not a PS-X EXE");

    ExeInfo exe{le32(data, 0x10), le32(data, 0x14), le32(data, 0x18), le32(data, 0x1C),
                le32(data, 0x28), le32(data, 0x2C), le32(data, 0x30), le32(data, 0x34)};

    const int32_t text = psx_ram_offset(exe.t_addr);
    if (text < 0 || static_cast<size_t>(text) + exe.t_size > PSX_RAM_SIZE ||
        data.size() < kExeHeaderSize + exe.t_size)
        throw std::runtime_error("PS-EXE text segment does not fit in RAM");
    std::memcpy(ctx_.ram + text, data.data() + kExeHeaderSize, exe.t_size);

    if (exe.b_size) {
        const int32_t bss = psx_ram_offset(exe.b_addr);
        if (bss < 0 || static_cast<size_t>(bss) + exe.b_size > PSX_RAM_SIZE)
            throw std::runtime_error("PS-EXE BSS does not fit in RAM");
        std::memset(ctx_.ram + bss, 0, exe.b_size);
    }

    // Mirror what the BIOS does before jumping to the executable.
    ctx_.pc = exe.pc0;
    ctx_.r[28] = exe.gp0;
    const uint32_t sp = exe.s_addr ? exe.s_addr + exe.s_size : 0x801FFF00u;
    ctx_.r[29] = sp;
    ctx_.r[30] = sp;
    return exe;
}

}  // namespace psx
