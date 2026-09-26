#include "program.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace recomp {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

json read_json(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return json::parse(in);
}

uint32_t hex(const json& value) { return static_cast<uint32_t>(std::stoul(value.get<std::string>(), nullptr, 16)); }

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

}  // namespace

std::optional<uint32_t> Segment::word(uint32_t addr) const {
    if (addr < base || addr + 4 > end() || (addr & 3)) return std::nullopt;
    uint32_t v;
    std::memcpy(&v, bytes.data() + (addr - base), 4);
    return v;
}

Instr Segment::instr(uint32_t addr) const {
    const auto w = word(addr);
    return w ? decode(*w, addr) : Instr{0, addr, Op::Invalid, 0, 0, 0, 0, 0};
}

Program load_program(const fs::path& root, const std::string& game_id, uint32_t text_begin, uint32_t text_end) {
    Program prog;
    prog.game_id = game_id;

    // Boot EXE: header + load image.
    const auto exe = read_file(root / "extracted" / game_id / "exe" / "boot.exe");
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error("boot.exe is not a PS-X EXE");
    Segment main;
    main.name = "main";
    prog.entry = le32(exe, 0x10);
    main.base = le32(exe, 0x18);
    const uint32_t t_size = le32(exe, 0x1C);
    main.bytes.assign(exe.begin() + 0x800, exe.begin() + 0x800 + t_size);
    main.code_begin = text_begin;
    main.code_end = text_end;
    prog.segments.push_back(std::move(main));

    // Overlays sharing one load window.
    const fs::path ov_path = root / "config" / game_id / "overlays.json";
    if (fs::exists(ov_path)) {
        const json ov = read_json(ov_path);
        const auto container = read_file(root / "extracted" / game_id / "fs" / ov.at("container").get<std::string>());
        const uint32_t load = hex(ov.at("load_address"));
        prog.overlay_lo = load;
        prog.overlay_hi = load;
        for (const auto& s : ov.at("segments")) {
            Segment seg;
            seg.name = s.at("name").get<std::string>();
            seg.overlay = true;
            seg.base = load;
            const uint32_t off = hex(s.at("file_offset"));
            const uint32_t size = hex(s.at("size"));
            if (off + size > container.size()) throw std::runtime_error("overlay " + seg.name + " exceeds container");
            seg.bytes.assign(container.begin() + off, container.begin() + off + size);
            seg.code_begin = seg.base;
            seg.code_end = seg.end();
            prog.overlay_hi = std::max(prog.overlay_hi, seg.end());
            prog.segments.push_back(std::move(seg));
        }
    }

    // Ghidra's function list for the boot EXE.
    const fs::path fn_path = root / "config" / game_id / "functions.json";
    if (fs::exists(fn_path)) {
        const json functions = read_json(fn_path);  // must outlive the loop (range-for temporary)
        for (const auto& f : functions.at("functions")) {
            const uint32_t e = hex(f.at("entry"));
            if (prog.main().in_code(e)) prog.known_functions.push_back(e);
        }
    }
    return prog;
}

}  // namespace recomp
