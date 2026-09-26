// Unit tests for dcb_vfs (src/vfs) and the HD texture index (src/hle/gpu).
// Run as `test_vfs <case>`; each case is its own ctest entry (vfs.<case>).

#include "gpu/hd_textures.hpp"
#include "vfs/hash.hpp"
#include "vfs/image.hpp"
#include "vfs/pak.hpp"
#include "vfs/tim.hpp"
#include "vfs/vab.hpp"
#include "vfs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

namespace fs = std::filesystem;

// --- TIM -------------------------------------------------------------------

// Minimal 4-bit TIM: 16-entry CLUT (entry 1 = red), 8x2 pixels, all index 1.
std::vector<uint8_t> make_tim4() {
    std::vector<uint8_t> t;
    auto u32 = [&](uint32_t v) {
        t.push_back(static_cast<uint8_t>(v));
        t.push_back(static_cast<uint8_t>(v >> 8));
        t.push_back(static_cast<uint8_t>(v >> 16));
        t.push_back(static_cast<uint8_t>(v >> 24));
    };
    auto u16 = [&](uint16_t v) {
        t.push_back(static_cast<uint8_t>(v));
        t.push_back(static_cast<uint8_t>(v >> 8));
    };
    u32(0x10);
    u32(0x08);              // 4-bit + CLUT
    u32(12 + 16 * 2);       // clut length
    u16(0);
    u16(240);               // clut y
    u16(16);
    u16(1);                 // 16 entries, 1 palette
    u16(0x0000);
    u16(0x001F);            // red
    for (int i = 2; i < 16; ++i) u16(0x7FFF);
    u32(12 + 2 * 2 * 2);    // image length: w=2 (8px), h=2
    u16(640);
    u16(0);
    u16(2);
    u16(2);
    for (int i = 0; i < 8; ++i) t.push_back(0x11);  // all index 1
    return t;
}

void test_tim4() {
    const std::vector<uint8_t> blob = make_tim4();
    vfs::Tim tim;
    CHECK(vfs::parse_tim(blob.data(), blob.size(), tim) == blob.size());
    CHECK(tim.bpp == 4 && tim.has_clut);
    CHECK(tim.pixel_width() == 8 && tim.pixel_height() == 2);
    std::vector<uint8_t> rgba;
    CHECK(vfs::tim_to_rgba(tim, 0, rgba));
    CHECK(rgba.size() == 8 * 2 * 4);
    // Red, opaque: expand5(31) == 255.
    CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
    // Index 0 -> transparent black.
    vfs::Tim zero = tim;
    std::fill(zero.pixels.begin(), zero.pixels.end(), 0x00);
    CHECK(vfs::tim_to_rgba(zero, 0, rgba));
    CHECK(rgba[3] == 0);
    // Bad palette / truncated input rejected.
    CHECK(!vfs::tim_to_rgba(tim, 1, rgba));
    CHECK(vfs::parse_tim(blob.data(), blob.size() - 1, tim) == 0);
    uint8_t bad_magic[8] = {};
    CHECK(vfs::parse_tim(bad_magic, sizeof bad_magic, tim) == 0);
    // Transparent CLUT entry stays transparent.
    CHECK(vfs::rgba_to_psx15(1, 2, 3, 0) == 0);
    CHECK(vfs::rgba_to_psx15(255, 0, 0, 255) == 0x001F);
}

void test_tim_scan() {
    const std::vector<uint8_t> one = make_tim4();
    std::vector<uint8_t> blob = {0xAA, 0x10, 0x00};  // junk prefix incl. partial magic
    blob.insert(blob.end(), one.begin(), one.end());
    blob.push_back(0x10);  // trailing junk byte
    blob.insert(blob.end(), one.begin(), one.end());
    const auto hits = vfs::scan_tims(blob.data(), blob.size());
    CHECK(hits.size() == 2);
    CHECK(hits[0].first == 3 && hits[0].second == one.size());
    CHECK(hits[1].first == 3 + one.size() + 1);
}

// --- hash ------------------------------------------------------------------

void test_fnv() {
    CHECK(vfs::fnv1a64(nullptr, 0) == vfs::kFnvOffsetBasis);
    const char* s = "hello";
    const uint64_t h = vfs::fnv1a64(s, 5);
    CHECK(h == 0xa430d84680aabd0bull);  // canonical FNV-1a 64 test vector
    CHECK(vfs::to_hex16(h) == "a430d84680aabd0b");
    uint64_t back = 0;
    CHECK(vfs::from_hex16("a430d84680aabd0b", back) && back == h);
    CHECK(!vfs::from_hex16("xyz", back));
}

// --- pak + vfs -------------------------------------------------------------

fs::path scratch_dir() {
    fs::path dir = fs::temp_directory_path() / "dcb_vfs_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void test_pak_roundtrip() {
    const fs::path dir = scratch_dir();
    vfs::PakWriter w;
    const std::vector<uint8_t> a = {1, 2, 3, 250};
    const std::vector<uint8_t> empty;
    CHECK(w.add("textures/a.bin", a));
    CHECK(w.add("empty.bin", empty));
    CHECK(!w.add("textures/a.bin", a));  // duplicate rejected
    CHECK(!w.add("../evil", a));         // traversal rejected
    const std::string pak = (dir / "t.pak").string();
    CHECK(w.write(pak));

    vfs::PakReader r;
    CHECK(r.open(pak));
    CHECK(r.entries().size() == 2);
    std::vector<uint8_t> out;
    CHECK(r.read("textures/a.bin", out) && out == a);
    CHECK(r.read("empty.bin", out) && out.empty());
    CHECK(!r.read("missing", out));
    CHECK(!r.open(dir / "nope.pak"));
}

void test_vfs_mounts() {
    const fs::path dir = scratch_dir();
    fs::create_directories(dir / "loose" / "sub");
    FILE* f = std::fopen((dir / "loose" / "sub" / "f.txt").string().c_str(), "wb");
    CHECK(f);
    std::fwrite("hi", 1, 2, f);
    std::fclose(f);

    vfs::PakWriter w;
    const std::vector<uint8_t> pak_data = {'p', 'a', 'k'};
    CHECK(w.add("sub/f.txt", pak_data));
    const std::string pak = (dir / "t.pak").string();
    CHECK(w.write(pak));

    vfs::Vfs vfs;
    CHECK(vfs.mount(pak));
    CHECK(vfs.mount(dir / "loose"));  // later mount shadows the pak
    std::vector<uint8_t> out;
    CHECK(vfs.read("sub/f.txt", out));
    CHECK(out == std::vector<uint8_t>({'h', 'i'}));
    CHECK(vfs.exists("sub/f.txt"));
    CHECK(!vfs.exists("../evil"));
    CHECK(!vfs.read("nope", out));
}

// --- HD index ----------------------------------------------------------------
// Drives maybe_replace() with a hand-built manifest + 1x1 red PNG in a temp dir.

void test_hd_replace() {
    const fs::path dir = scratch_dir();
    // 1x1 red PNG: 4-bit TIM pixels {red} replaced by RGBA(255,0,0,255) — same color,
    // proving the plumbing, not the art.
    const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
                           0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                           0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
                           0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
                           0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xFB, 0x52, 0x1D, 0x00, 0x00, 0x00,
                           0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    fs::create_directories(dir / "art");
    FILE* f = std::fopen((dir / "art" / "r.png").string().c_str(), "wb");
    CHECK(f);
    CHECK(std::fwrite(png, 1, sizeof png, f) == sizeof png);
    std::fclose(f);

    // Manifest for a 1x1 16-bit upload of PSX15 red (bytes 1F 00).
    const uint8_t upload[2] = {0x1F, 0x00};
    const uint64_t img = vfs::fnv1a64(upload, 2);
    char manifest[256];
    std::snprintf(
        manifest, sizeof manifest,
        "{\"version\":1,\"game\":\"TEST\",\"entries\":[{\"img\":\"%s\",\"w\":1,\"h\":1,\"bpp\":16,\"path\":\"r.png\"}]}",
        vfs::to_hex16(img).c_str());
    const std::string man_path = (dir / "m.json").string();
    f = std::fopen(man_path.c_str(), "wb");
    CHECK(f);
    std::fwrite(manifest, 1, std::strlen(manifest), f);
    std::fclose(f);

    hle::HdTextures hd;
    CHECK(hd.load(man_path, (dir / "art").string()));
    CHECK(hd.enabled() && hd.entry_count() == 1);
    // Whatever the embedded 1x1 PNG decodes to, the replacement must equal the
    // same RGBA pushed through rgba_to_psx15.
    vfs::Vfs probe;
    CHECK(probe.mount(dir / "art"));
    int pw = 0, ph = 0;
    std::vector<uint8_t> png_rgba;
    CHECK(vfs::load_rgba(probe, "r.png", pw, ph, png_rgba));
    CHECK(pw == 1 && ph == 1);
    const uint16_t want = vfs::rgba_to_psx15(png_rgba[0], png_rgba[1], png_rgba[2], png_rgba[3]);
    const uint32_t staged[1] = {0x0000001Fu};  // low half = 0x001F, padding half ignored
    const std::vector<uint16_t>* hit = hd.maybe_replace(0, 0, 1, 1, staged, 1);
    CHECK(hit && hit->size() == 1 && (*hit)[0] == want);
    CHECK(hd.hits() == 1);
    // Wrong content misses; wrong size misses.
    const uint32_t other[1] = {0x00007C00u};
    CHECK(hd.maybe_replace(0, 0, 1, 1, other, 1) == nullptr);
    CHECK(hd.maybe_replace(0, 0, 2, 1, staged, 1) == nullptr);
    CHECK(hd.misses() == 2);
    // Bad manifest / missing art never replaces.
    hle::HdTextures bad;
    CHECK(!bad.load((dir / "missing.json").string(), ""));
    CHECK(!bad.enabled());
}

// --- VAB / BRR ---------------------------------------------------------------
// Hand-computed vectors for the SPU-ADPCM formula (spu-adpcm, psx-spx "SPU
// ADPCM Samples"): sample = (nibble<<12 >> shift) + ((old*pos + older*neg + 32) >> 6).

void test_brr_block() {
    // Silence block: shift 0, filter 0, all nibbles 0 -> 28 zeros.
    const uint8_t silence[16] = {};
    int16_t out[28];
    int16_t old = 0, older = 0;
    vfs::decode_brr_block(silence, out, old, older);
    for (int s : out) CHECK(s == 0);

    // Nibble 1 (+0x1000), shift 12, filter 0: 0x1000>>12 = 1 per sample.
    uint8_t ones[16] = {0x0C, 0x00};
    for (int i = 2; i < 16; ++i) ones[i] = 0x11;
    old = older = 0;
    vfs::decode_brr_block(ones, out, old, older);
    for (int s : out) CHECK(s == 1);

    // Filter history carries across blocks: filter 1 (pos 60) with zero input
    // decays old by 60/64 per sample.
    uint8_t filt[16] = {0x1C, 0x00};  // filter 1, shift 12, zero nibbles
    for (int i = 2; i < 16; ++i) filt[i] = 0x00;
    old = 1024;
    older = 0;
    vfs::decode_brr_block(filt, out, old, older);
    CHECK(out[0] == (1024 * 60 + 32) >> 6);  // 960
    CHECK(old == out[27]);                    // history updated
}

// Minimal VAB: header + one genuine tone (first-8-nonzero + reserved tail,
// vag 1) followed by unrelated rows that must NOT parse as tones.
std::vector<uint8_t> make_vab() {
    std::vector<uint8_t> v(32 + 2048 + 4 * 32 + 64, 0);
    auto u32 = [&](size_t o, uint32_t x) {
        v[o] = static_cast<uint8_t>(x);
        v[o + 1] = static_cast<uint8_t>(x >> 8);
        v[o + 2] = static_cast<uint8_t>(x >> 16);
        v[o + 3] = static_cast<uint8_t>(x >> 24);
    };
    auto u16 = [&](size_t o, uint16_t x) {
        v[o] = static_cast<uint8_t>(x);
        v[o + 1] = static_cast<uint8_t>(x >> 8);
    };
    v[0] = 'p';
    v[1] = 'B';
    v[2] = 'A';
    v[3] = 'V';
    u32(4, 7);
    u32(12, static_cast<uint32_t>(v.size()));
    u16(16, 0xEEEE);
    u16(18, 1);
    u16(20, 1);
    u16(22, 1);
    // tone 0 genuine: vol nonzero + reserved tail, vag 1, center 60.
    const size_t t0 = 32 + 2048;
    v[t0 + 0] = 0x7F;
    v[t0 + 2] = 60;
    u16(t0 + 22, 1);
    const uint8_t tail[8] = {0xC0, 0x00, 0xC1, 0x00, 0xC2, 0x00, 0xC3, 0x00};
    for (int i = 0; i < 8; ++i) v[t0 + 24 + i] = tail[i];
    // tone 1: first-8-nonzero but NO tail -> garbage, must be skipped.
    v[t0 + 32 + 0] = 0x7F;
    return v;
}

void test_vab_parse() {
    const std::vector<uint8_t> blob = make_vab();
    vfs::Vab vab;
    CHECK(vfs::parse_vab(blob.data(), blob.size(), vab) == blob.size());
    CHECK(vab.version == 7 && vab.vags == 1 && vab.total_size == blob.size());
    CHECK(vab.active_tones.size() == 1);  // the tail-less row is not a tone
    CHECK(vab.active_tones[0].vag == 1 && vab.active_tones[0].center == 60);
    // Truncated / bad magic rejected.
    CHECK(vfs::parse_vab(blob.data(), blob.size() - 1, vab) == 0);
    uint8_t bad[80] = {};
    CHECK(vfs::parse_vab(bad, sizeof bad, vab) == 0);
    // Scan finds it inside junk.
    std::vector<uint8_t> hay = {0xAA, 0x70};
    hay.insert(hay.end(), blob.begin(), blob.end());
    const auto hits = vfs::scan_vabs(hay.data(), hay.size());
    CHECK(hits.size() == 1 && hits[0].first == 2 && hits[0].second == blob.size());
}

void test_vab_decode() {
    // decode_vag walks an explicit wave area: block 0 plain, block 1 LoopEnd.
    uint8_t waves[32] = {0x0C, 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x11, 0x11, 0x11, 0x11, 0x11, 0x0C, 0x01, 0x22, 0x22, 0x22, 0x22,
                         0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22};
    std::vector<int16_t> pcm;
    CHECK(vfs::decode_vag(waves, sizeof waves, 0, pcm));
    CHECK(pcm.size() == 56);  // two blocks
    for (int i = 0; i < 28; ++i) CHECK(pcm[i] == 1);
    CHECK(!vfs::decode_vag(waves, sizeof waves, 2, pcm));  // OOB start
    uint8_t noend[16] = {};
    CHECK(!vfs::decode_vag(noend, sizeof noend, 0, pcm));  // no LoopEnd
    // WAV round-trips the samples.
    std::vector<uint8_t> wav;
    CHECK(vfs::write_wav(22050, pcm.data(), pcm.size(), wav));
    CHECK(wav.size() == 44 + pcm.size() * 2);
    CHECK(wav[0] == 'R' && wav[22] == 1);  // RIFF, mono
    const uint32_t rate = static_cast<uint32_t>(wav[24]) | (static_cast<uint32_t>(wav[25]) << 8) |
                          (static_cast<uint32_t>(wav[26]) << 16) | (static_cast<uint32_t>(wav[27]) << 24);
    CHECK(rate == 22050);
}

struct Case {
    const char* name;
    void (*fn)();
};
constexpr Case kCases[] = {
    {"tim4", test_tim4},
    {"tim_scan", test_tim_scan},
    {"fnv", test_fnv},
    {"pak_roundtrip", test_pak_roundtrip},
    {"vfs_mounts", test_vfs_mounts},
    {"hd_replace", test_hd_replace},
    {"brr_block", test_brr_block},
    {"vab_parse", test_vab_parse},
    {"vab_decode", test_vab_decode},
};

}  // namespace

int main(int argc, char** argv) {
    int ran = 0;
    for (const Case& c : kCases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        c.fn();
        std::printf("vfs.%s: ok\n", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "unknown test case\n");
        return 1;
    }
    return 0;
}
