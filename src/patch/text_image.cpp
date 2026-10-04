// The raw disc image an imported dump was made from, for the community fixes (text_fixes.cpp):
// the .xdelta files patch the whole image, so it is rebuilt from layout.txt, iso_meta.bin and fs/
// (see src/hle/cdrom/importer.hpp, tools/disc/extract_disc.py write_layout). Unlike the game's
// ExtractedDisc, which leaves them zero, the EDC/ECC of every form-1 file sector is recomputed,
// so the image matches the original dump byte for byte (the patches may copy any of its bytes).

#include "text_internal.hpp"

#include <array>
#include <fstream>
#include <sstream>

namespace patch::text {

namespace {

constexpr size_t kRaw = 2352;

/// CD-ROM EDC / ECC (ECMA-130 annex A/B): the usual lookup-table formulation.
struct Ecc {
    std::array<uint8_t, 256> f{}, b{};
    std::array<uint32_t, 256> edc{};
    Ecc() {
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
            f[i] = static_cast<uint8_t>(j);
            b[i ^ (j & 0xFF)] = static_cast<uint8_t>(i);
            uint32_t e = i;
            for (int k = 0; k < 8; ++k) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
            edc[i] = e;
        }
    }
    uint32_t compute_edc(const uint8_t* p, size_t n) const {
        uint32_t e = 0;
        while (n--) e = (e >> 8) ^ edc[(e ^ *p++) & 0xFF];
        return e;
    }
    void block(const uint8_t* src, size_t major_count, size_t minor_count, size_t major_mult, size_t minor_inc,
               uint8_t* dest) const {
        const size_t size = major_count * minor_count;
        for (size_t major = 0; major < major_count; ++major) {
            size_t index = (major >> 1) * major_mult + (major & 1);
            uint8_t a = 0, c = 0;
            for (size_t minor = 0; minor < minor_count; ++minor) {
                const uint8_t t = src[index];
                index += minor_inc;
                if (index >= size) index -= size;
                a ^= t;
                c ^= t;
                a = f[a];
            }
            a = b[f[a] ^ c];
            dest[major] = a;
            dest[major + major_count] = a ^ c;
        }
    }
    /// EDC/ECC of a mode-2 sector (form 1: EDC + P/Q parity over a zeroed header; form 2: EDC).
    void seal(uint8_t* s) const {
        if (s[18] & 0x20) {  // form 2
            const uint32_t e = compute_edc(s + 16, 2332);
            for (int k = 0; k < 4; ++k) s[2348 + k] = static_cast<uint8_t>(e >> (8 * k));
            return;
        }
        const uint32_t e = compute_edc(s + 16, 2056);
        for (int k = 0; k < 4; ++k) s[2072 + k] = static_cast<uint8_t>(e >> (8 * k));
        uint8_t header[4];
        std::copy(s + 12, s + 16, header);
        std::fill(s + 12, s + 16, uint8_t{0});
        block(s + 12, 86, 24, 2, 86, s + 2076);   // P parity
        block(s + 12, 52, 43, 86, 88, s + 2248);  // Q parity
        std::copy(header, header + 4, s + 12);
    }
};

uint8_t bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

bool parse_hex4(const std::string& s, uint8_t out[4]) {
    if (s.size() != 8) return false;
    for (int i = 0; i < 4; ++i) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(s[2 * static_cast<size_t>(i)]), lo = nib(s[2 * static_cast<size_t>(i) + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

void read_exact(std::ifstream& in, uint8_t* out, size_t n, const std::filesystem::path& path) {
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    if (static_cast<size_t>(in.gcount()) != n) throw std::runtime_error("short read from " + path.string());
}

}  // namespace

Bytes rebuild_image(const std::filesystem::path& dump, std::vector<DiscFile>& files) {
    std::ifstream layout(dump / "layout.txt");
    if (!layout) throw std::runtime_error("no layout.txt in " + dump.string() + " (import the disc with dcb --import)");
    static const Ecc ecc;
    Bytes image;
    std::string line;
    const auto sector = [&](uint64_t lba) {
        if ((lba + 1) * kRaw > image.size()) throw std::runtime_error("layout.txt: sector past the end");
        return image.data() + lba * kRaw;
    };
    while (std::getline(layout, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "sectors") {
            uint64_t n = 0;
            in >> n;
            image.assign(n * kRaw, 0);
        } else if (kind == "meta") {
            uint64_t lba = 0, count = 0, index = 0;
            in >> lba >> count >> index;
            std::ifstream meta(dump / "iso_meta.bin", std::ios::binary);
            meta.seekg(static_cast<std::streamoff>(index * kRaw));
            if (count) sector(lba + count - 1);
            read_exact(meta, sector(lba), count * kRaw, dump / "iso_meta.bin");
        } else if (kind == "file") {
            uint64_t lba = 0, count = 0, bytes = 0;
            std::string storage, first, last, rel;
            in >> lba >> count >> bytes >> storage >> first >> last;
            std::getline(in >> std::ws, rel);  // the path may contain spaces
            uint8_t first_sh[4], last_sh[4];
            if (rel.empty() || !parse_hex4(first, first_sh) || !parse_hex4(last, last_sh) || count == 0)
                throw std::runtime_error("bad line in layout.txt: " + line);
            sector(lba + count - 1);  // in range
            std::ifstream f(dump / rel, std::ios::binary);
            if (!f) throw std::runtime_error("cannot read " + (dump / rel).string());
            if (storage == "raw2352") {
                read_exact(f, sector(lba), count * kRaw, dump / rel);
                continue;
            }
            files.push_back({rel.rfind("fs/", 0) == 0 ? rel.substr(3) : rel, static_cast<uint32_t>(lba),
                             static_cast<uint32_t>(count), static_cast<uint32_t>(bytes)});
            static constexpr uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
            for (uint64_t k = 0; k < count; ++k) {
                uint8_t* s = sector(lba + k);
                std::copy(kSync, kSync + 12, s);
                const uint64_t abs = lba + k + 150;
                s[12] = bcd(static_cast<uint32_t>(abs / 4500));
                s[13] = bcd(static_cast<uint32_t>(abs / 75 % 60));
                s[14] = bcd(static_cast<uint32_t>(abs % 75));
                s[15] = 2;
                const uint8_t* sh = k + 1 == count ? last_sh : first_sh;
                std::copy(sh, sh + 4, s + 16);
                std::copy(sh, sh + 4, s + 20);
                const uint64_t off = k * 2048;
                if (off < bytes) read_exact(f, s + 24, static_cast<size_t>(std::min<uint64_t>(2048, bytes - off)), dump / rel);
                ecc.seal(s);
            }
        }
    }
    if (image.empty()) throw std::runtime_error("empty layout.txt in " + dump.string());
    return image;
}

}  // namespace patch::text
