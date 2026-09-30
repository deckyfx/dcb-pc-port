// DRV / PAK / TIM / ARC containers (see containers.hpp).

#include "containers.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace patch {

namespace {

constexpr uint32_t kSector = 0x800;
constexpr size_t kEntry = 32;

/// Python's `data[a:b]` on a span: clamped, never throws.
View slice(View b, size_t a, size_t e) {
    a = std::min(a, b.size());
    e = std::clamp(e, a, b.size());
    return b.subspan(a, e - a);
}

/// Bytes up to the first NUL (the field's text).
std::string field(View b, size_t off, size_t len) {
    View f = slice(b, off, off + len);
    const auto nul = std::find(f.begin(), f.end(), uint8_t{0});
    return std::string(f.begin(), nul);
}

}  // namespace

uint16_t rd16(View b, size_t off) {
    if (off + 2 > b.size()) throw std::runtime_error("read past the end of the data");
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t rd32(View b, size_t off) {
    if (off + 4 > b.size()) throw std::runtime_error("read past the end of the data");
    return uint32_t{b[off]} | uint32_t{b[off + 1]} << 8 | uint32_t{b[off + 2]} << 16 | uint32_t{b[off + 3]} << 24;
}

void wr16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void wr32(Bytes& b, uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(static_cast<uint8_t>(v >> s));
}

Bytes read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- DRV

Drv::Drv(View data) : data_(data) { walk(0, "", 0); }

void Drv::walk(uint32_t sector, const std::string& prefix, int depth) {
    if (depth > 8) throw std::runtime_error("DRV: TOC nesting too deep; not a DRV?");
    for (size_t off = size_t{sector} * kSector; off + kEntry <= data_.size(); off += kEntry) {
        const uint8_t kind = data_[off];
        if (kind == 0) break;
        const std::string ext = field(data_, off + 1, 3);
        const uint32_t start = rd32(data_, off + 4), size = rd32(data_, off + 8);
        const std::string name = field(data_, off + 16, 16);
        if (kind == 0x80) {
            walk(start, prefix + name + "/", depth + 1);
        } else if (kind == 0x01) {
            // A record past the end (US B.DRV's never-written "copy of CARD2.CDD") is skipped.
            if (uint64_t{start} * kSector + size <= data_.size())
                entries_.push_back({prefix + name + "." + ext, start * kSector, size});
        } else {
            throw std::runtime_error("DRV: unknown TOC entry type");
        }
    }
}

View Drv::file(std::string_view path) const {
    for (const Entry& e : entries_)
        if (e.path == path) return data_.subspan(e.offset, e.size);
    throw std::runtime_error(std::string(path) + " not in DRV");
}

std::optional<View> Drv::find_last(std::string_view path) const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->path == path) return data_.subspan(it->offset, it->size);
    return std::nullopt;
}

// ---------------------------------------------------------------- PAK

std::vector<PakChunk> read_pak(View data) {
    std::vector<PakChunk> out;
    size_t off = 0;
    while (off + 4 <= data.size()) {
        if (rd32(data, off) == 0xFFFFFFFFu) return out;  // terminator
        PakChunk c;
        c.kind = rd16(data, off);
        c.id = rd16(data, off + 2);
        const uint32_t size = rd32(data, off + 4);
        if (off + 8 + size > data.size()) throw std::runtime_error("PAK: a chunk overruns the file");
        c.data.assign(data.begin() + static_cast<std::ptrdiff_t>(off + 8),
                      data.begin() + static_cast<std::ptrdiff_t>(off + 8 + size));
        out.push_back(std::move(c));
        off += 8 + size;
    }
    if (off < data.size() && std::any_of(data.begin() + static_cast<std::ptrdiff_t>(off), data.end(),
                                         [](uint8_t b) { return b != 0; }))
        throw std::runtime_error("PAK: trailing bytes");
    return out;
}

Bytes write_pak(const std::vector<PakChunk>& chunks) {
    Bytes out;
    for (const PakChunk& c : chunks) {
        wr16(out, c.kind);
        wr16(out, c.id);
        wr32(out, static_cast<uint32_t>(c.data.size()));
        out.insert(out.end(), c.data.begin(), c.data.end());
    }
    wr32(out, 0xFFFFFFFFu);
    return out;
}

// ---------------------------------------------------------------- TIM / ARC

std::vector<TimBlock> tim_blocks(View tim) {
    if (tim.size() < 8 || rd32(tim, 0) != 0x10) throw std::runtime_error("not a TIM");
    std::vector<TimBlock> out;
    size_t off = 8;
    const auto block = [&] {
        TimBlock b;
        const uint32_t len = rd32(tim, off);
        b.offset = off;
        b.x = rd16(tim, off + 4);
        b.y = rd16(tim, off + 6);
        b.w = rd16(tim, off + 8);
        b.h = rd16(tim, off + 10);
        b.payload = slice(tim, off + 12, off + len);
        out.push_back(b);
        off += len;
    };
    if (rd32(tim, 4) & 8) block();
    block();
    return out;
}

std::optional<std::vector<View>> arc_entries(View arc) {
    if (arc.size() < 4) return std::nullopt;
    const uint32_t first = rd32(arc, 0);
    if (first % 4 != 0 || first < 8 || first >= arc.size()) return std::nullopt;
    std::vector<uint32_t> offs(first / 4);
    for (size_t i = 0; i < offs.size(); ++i) offs[i] = rd32(arc, 4 * i);
    if (offs.back() != arc.size() || !std::is_sorted(offs.begin(), offs.end())) return std::nullopt;
    std::vector<View> out;
    for (size_t i = 0; i + 1 < offs.size(); ++i) out.push_back(arc.subspan(offs[i], offs[i + 1] - offs[i]));
    return out;
}

Bytes write_arc(const std::vector<View>& entries) {
    Bytes out;
    uint32_t pos = static_cast<uint32_t>(4 * (entries.size() + 1));
    for (View e : entries) {
        wr32(out, pos);
        pos += static_cast<uint32_t>(e.size());
    }
    wr32(out, pos);
    for (View e : entries) out.insert(out.end(), e.begin(), e.end());
    return out;
}

}  // namespace patch
