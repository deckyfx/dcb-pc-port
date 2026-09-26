// Unit tests for the software GPU (src/hle/gpu). Run as `test_gpu <case>`; each case is its own
// ctest entry (gpu.<case>).

#include "gpu/gpu.hpp"
#include "gpu/hd_textures.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using hle::Gpu;

uint16_t px(const Gpu& gpu, int x, int y) { return gpu.vram()[y * Gpu::kVramWidth + x]; }

constexpr uint16_t rgb15(unsigned r, unsigned g, unsigned b) { return static_cast<uint16_t>(r | g << 5 | b << 10); }

constexpr uint32_t xy(int x, int y) {
    return (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) & 0xFFFFu) << 16;
}

/// A GPU with a full-VRAM draw area and zero offset (the reset state has a 1x1 area at 0,0).
std::unique_ptr<Gpu> make_gpu() {
    auto gpu = std::make_unique<Gpu>();
    gpu->gp1(0x00000000u);
    gpu->gp0(0xE3000000u);                               // draw area top-left (0,0)
    gpu->gp0(0xE4000000u | 1023u | (511u << 10));        // bottom-right (1023,511)
    gpu->gp0(0xE5000000u);                               // offset (0,0)
    return gpu;
}

void fill(Gpu& gpu, uint32_t color, int x, int y, int w, int h) {
    gpu.gp0(0x02000000u | color);
    gpu.gp0(xy(x, y));
    gpu.gp0(xy(w, h));
}

/// Upload w*h pixels to VRAM with GP0(A0h).
void upload(Gpu& gpu, int x, int y, int w, int h, const uint16_t* pixels) {
    gpu.gp0(0xA0000000u);
    gpu.gp0(xy(x, y));
    gpu.gp0(xy(w, h));
    const int n = w * h;
    for (int i = 0; i < n; i += 2) {
        const uint32_t lo = pixels[i], hi = i + 1 < n ? pixels[i + 1] : 0;
        gpu.gp0(lo | hi << 16);
    }
}

int count_nonzero(const Gpu& gpu, int x0, int y0, int w, int h) {
    int n = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) n += px(gpu, x, y) != 0;
    return n;
}

void test_fill_rect() {
    auto gpu = make_gpu();
    // Draw area / offset / mask must not affect GP0(02h).
    gpu->gp0(0xE3000000u);
    gpu->gp0(0xE4000000u);         // 1x1 draw area
    gpu->gp0(0xE5000000u | 100u);  // offset x=100
    gpu->gp0(0xE6000003u);         // set + check mask
    fill(*gpu, 0x00FF0000u, 0x13, 5, 5, 3);  // blue; x aligns down to 0x10, width rounds up to 16
    for (int y = 5; y < 8; ++y)
        for (int x = 0x10; x < 0x20; ++x) CHECK(px(*gpu, x, y) == rgb15(0, 0, 31));
    CHECK(px(*gpu, 0x0F, 5) == 0);
    CHECK(px(*gpu, 0x20, 5) == 0);
    CHECK(px(*gpu, 0x10, 4) == 0);
    CHECK(px(*gpu, 0x10, 8) == 0);
    // Fill wraps horizontally around VRAM.
    fill(*gpu, 0x000000FFu, 1008, 20, 32, 1);
    CHECK(px(*gpu, 1008, 20) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 1023, 20) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 0, 20) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 15, 20) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 16, 20) == 0);
}

void test_vram_transfer() {
    auto gpu = make_gpu();
    const uint16_t data[9] = {1, 2, 3, 4, 5, 6, 7, 8, 0x7FFF};
    upload(*gpu, 100, 50, 3, 3, data);  // odd pixel count: last word carries one pixel
    CHECK(!gpu->receiving_vram());
    CHECK(px(*gpu, 100, 50) == 1 && px(*gpu, 102, 50) == 3 && px(*gpu, 100, 51) == 4 && px(*gpu, 102, 52) == 0x7FFF);
    CHECK(px(*gpu, 103, 50) == 0);

    gpu->gp0(0xC0000000u);
    gpu->gp0(xy(100, 50));
    gpu->gp0(xy(3, 3));
    CHECK(gpu->sending_vram());
    CHECK((gpu->gpustat() & (1u << 27)) != 0);
    CHECK(gpu->gpuread() == (1u | 2u << 16));
    CHECK(gpu->gpuread() == (3u | 4u << 16));
    CHECK(gpu->gpuread() == (5u | 6u << 16));
    CHECK(gpu->gpuread() == (7u | 8u << 16));
    CHECK(gpu->gpuread() == 0x7FFFu);
    CHECK(!gpu->sending_vram());

    // Mask bit settings apply to CPU->VRAM uploads.
    gpu->gp0(0xE6000001u);  // set mask
    upload(*gpu, 0, 0, 2, 1, data);
    CHECK(px(*gpu, 0, 0) == 0x8001 && px(*gpu, 1, 0) == 0x8002);
    gpu->gp0(0xE6000002u);  // check mask only
    upload(*gpu, 0, 0, 2, 1, data + 4);
    CHECK(px(*gpu, 0, 0) == 0x8001);

    // VRAM->VRAM copy.
    gpu->gp0(0xE6000000u);
    gpu->gp0(0x80000000u);
    gpu->gp0(xy(100, 50));
    gpu->gp0(xy(200, 60));
    gpu->gp0(xy(3, 3));
    CHECK(px(*gpu, 200, 60) == 1 && px(*gpu, 202, 62) == 0x7FFF);
}

void test_triangle_fill_rule() {
    auto gpu = make_gpu();
    // Flat triangle (0,0) (4,0) (0,4): top-left rule keeps x+y<4 -> 10 pixels.
    gpu->gp0(0x200000FFu);
    gpu->gp0(xy(0, 0));
    gpu->gp0(xy(4, 0));
    gpu->gp0(xy(0, 4));
    CHECK(count_nonzero(*gpu, 0, 0, 8, 8) == 10);
    CHECK(px(*gpu, 0, 0) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 3, 0) != 0 && px(*gpu, 4, 0) == 0 && px(*gpu, 0, 3) != 0 && px(*gpu, 0, 4) == 0);
    CHECK(px(*gpu, 2, 2) == 0);

    // Opposite winding covers the same pixels.
    fill(*gpu, 0, 0, 0, 16, 16);
    gpu->gp0(0x200000FFu);
    gpu->gp0(xy(0, 0));
    gpu->gp0(xy(0, 4));
    gpu->gp0(xy(4, 0));
    CHECK(count_nonzero(*gpu, 0, 0, 8, 8) == 10);

    // Additive semi-transparent 4x4 quad at (20,20): exactly 16 pixels, each hit exactly once
    // (the shared diagonal is not drawn twice).
    gpu->gp0(0xE1000000u | (1u << 5));  // semi mode 1 (B+F)
    gpu->gp0(0x2A080808u);              // flat quad, semi-transparent, color 8 -> 1 in 5 bits
    gpu->gp0(xy(20, 20));
    gpu->gp0(xy(24, 20));
    gpu->gp0(xy(20, 24));
    gpu->gp0(xy(24, 24));
    CHECK(count_nonzero(*gpu, 16, 16, 16, 16) == 16);
    for (int y = 20; y < 24; ++y)
        for (int x = 20; x < 24; ++x) CHECK(px(*gpu, x, y) == rgb15(1, 1, 1));

    // Negative (sign-extended) coordinates are clipped by the draw area.
    fill(*gpu, 0, 0, 0, 32, 32);
    gpu->gp0(0x2000FF00u);
    gpu->gp0(xy(-4, -4));
    gpu->gp0(xy(8, -4));
    gpu->gp0(xy(-4, 8));
    CHECK(count_nonzero(*gpu, 0, 0, 16, 16) == 10);  // x+y<4 in the visible quadrant
    CHECK(px(*gpu, 0, 0) == rgb15(0, 31, 0));
    CHECK(px(*gpu, 1020, 508) == 0 && px(*gpu, 1023, 511) == 0);  // no wrap-around

    // Too-large polygons (width >= 1024) are skipped.
    fill(*gpu, 0, 0, 100, 16, 16);
    gpu->gp0(0x200000FFu);
    gpu->gp0(xy(-600, 100));
    gpu->gp0(xy(500, 100));
    gpu->gp0(xy(0, 110));
    CHECK(count_nonzero(*gpu, 0, 100, 16, 16) == 0);
}

void test_gouraud() {
    auto gpu = make_gpu();
    // Gouraud line: both end points drawn with exactly their vertex colors.
    gpu->gp0(0x500000FFu);  // red
    gpu->gp0(xy(0, 0));
    gpu->gp0(0x00FF0000u);  // blue
    gpu->gp0(xy(10, 0));
    CHECK(px(*gpu, 0, 0) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 10, 0) == rgb15(0, 0, 31));
    const uint16_t mid = px(*gpu, 5, 0);
    CHECK((mid & 31) >= 14 && (mid & 31) <= 17 && ((mid >> 10) & 31) >= 14 && ((mid >> 10) & 31) <= 17);

    // Gouraud triangle: the first vertex is exact, red falls off monotonically along x.
    gpu->gp0(0x300000F8u);
    gpu->gp0(xy(100, 100));
    gpu->gp0(0x00000000u);
    gpu->gp0(xy(132, 100));
    gpu->gp0(0x000000F8u);
    gpu->gp0(xy(100, 132));
    CHECK(px(*gpu, 100, 100) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 100, 131) == rgb15(31, 0, 0));  // along the constant-red edge
    int prev = 32;
    for (int x = 100; x < 131; ++x) {
        const int r = px(*gpu, x, 100) & 31;
        CHECK(r <= prev);
        prev = r;
    }
    CHECK(prev <= 1);
}

void test_textured_sprite() {
    auto gpu = make_gpu();
    // CLUT at (0,480): 0 transparent, 1 red, 2 blue with STP bit (semi-transparent).
    const uint16_t clut[16] = {0, rgb15(31, 0, 0), static_cast<uint16_t>(0x8000u | rgb15(0, 0, 16)), rgb15(0, 20, 0)};
    upload(*gpu, 0, 480, 16, 1, clut);
    // 4-bit texture in page 1 (x=64): one 16-texel row = 4 halfwords; pattern 0,1,2,3 repeating.
    const uint16_t row[4] = {0x3210, 0x3210, 0x3210, 0x3210};
    for (int v = 0; v < 16; ++v) upload(*gpu, 64, v, 4, 1, row);

    fill(*gpu, 0x00404040u, 192, 0, 32, 32);  // background (8,8,8)
    const uint16_t bg = rgb15(8, 8, 8);
    gpu->gp0(0xE1000001u);             // texpage x=64, 4-bit, semi mode 0
    gpu->gp0(0x7D808080u);             // 16x16 sprite, textured, raw
    gpu->gp0(xy(200, 8));
    gpu->gp0((480u << 6) << 16);      // clut (0,480), u=0 v=0
    CHECK(px(*gpu, 200, 8) == bg);                      // index 0 -> transparent
    CHECK(px(*gpu, 201, 8) == rgb15(31, 0, 0));
    CHECK(px(*gpu, 202, 8) == (0x8000u | rgb15(0, 0, 16)));  // opaque command: STP kept as mask bit
    CHECK(px(*gpu, 203, 23) == rgb15(0, 20, 0));
    CHECK(px(*gpu, 216, 8) == bg && px(*gpu, 200, 24) == bg);

    // Semi-transparent + modulated (color 80h = identity): only STP texels blend.
    fill(*gpu, 0x00404040u, 192, 0, 32, 32);
    gpu->gp0(0x7E808080u);  // 16x16, textured, semi, modulated
    gpu->gp0(xy(200, 8));
    gpu->gp0((480u << 6) << 16);
    CHECK(px(*gpu, 200, 8) == bg);
    CHECK(px(*gpu, 201, 8) == rgb15(31, 0, 0));                  // no STP: opaque
    CHECK(px(*gpu, 202, 8) == (0x8000u | rgb15(4, 4, 12)));      // (B+F)/2
    // Modulation by half intensity.
    gpu->gp0(0x7C404040u);
    gpu->gp0(xy(300, 8));
    gpu->gp0((480u << 6) << 16);
    CHECK(px(*gpu, 301, 8) == rgb15(15, 0, 0));

    // 8-bit CLUT and 15-bit direct textures with a texture window.
    const uint16_t idx8[2] = {0x0201, 0x0003};  // texels 1,2,3,0
    upload(*gpu, 128, 0, 2, 1, idx8);           // page 2 (x=128)
    gpu->gp0(0xE1000000u | 2u | (1u << 7));     // 8-bit
    gpu->gp0(0x65808080u);                      // variable-size raw textured rect
    gpu->gp0(xy(400, 0));
    gpu->gp0((480u << 6) << 16);
    gpu->gp0(xy(4, 1));
    CHECK(px(*gpu, 400, 0) == rgb15(31, 0, 0) && px(*gpu, 402, 0) == rgb15(0, 20, 0) && px(*gpu, 403, 0) == 0);
    const uint16_t direct[2] = {rgb15(1, 2, 3), rgb15(4, 5, 6)};
    upload(*gpu, 192, 0, 2, 1, direct);         // page 3 (x=192)
    gpu->gp0(0xE1000000u | 3u | (2u << 7));     // 15-bit
    gpu->gp0(0xE2000000u | 1u);                 // window: mask x = 8 texels -> u wraps every 8
    gpu->gp0(0x65808080u);
    gpu->gp0(xy(500, 0));
    gpu->gp0(8u);                               // u = 8 -> window maps to 0
    gpu->gp0(xy(2, 1));
    CHECK(px(*gpu, 500, 0) == rgb15(1, 2, 3) && px(*gpu, 501, 0) == rgb15(4, 5, 6));
}

void test_semi_transparency() {
    auto gpu = make_gpu();
    // Background (16,16,16), front (8,8,8) -> expected per mode.
    const unsigned expected[4] = {12, 24, 8, 18};
    for (unsigned mode = 0; mode < 4; ++mode) {
        const int x = static_cast<int>(mode) * 16;
        fill(*gpu, 0x00808080u, x, 0, 16, 1);
        gpu->gp0(0xE1000000u | mode << 5);
        gpu->gp0(0x62404040u);  // variable rect, semi, untextured
        gpu->gp0(xy(x, 0));
        gpu->gp0(xy(4, 1));
        const unsigned e = expected[mode];
        CHECK(px(*gpu, x, 0) == rgb15(e, e, e));
        CHECK(px(*gpu, x + 4, 0) == rgb15(16, 16, 16));
    }
    // Saturation: B+F clamps at 31.
    fill(*gpu, 0x00F8F8F8u, 0, 1, 16, 1);
    gpu->gp0(0xE1000000u | 1u << 5);
    gpu->gp0(0x62F8F8F8u);
    gpu->gp0(xy(0, 1));
    gpu->gp0(xy(1, 1));
    CHECK(px(*gpu, 0, 1) == rgb15(31, 31, 31));
}

void test_draw_area_clipping() {
    auto gpu = make_gpu();
    gpu->gp0(0xE3000000u | 10u | (10u << 10));
    gpu->gp0(0xE4000000u | 19u | (19u << 10));
    gpu->gp0(0x600000FFu);
    gpu->gp0(xy(0, 0));
    gpu->gp0(xy(64, 64));
    CHECK(count_nonzero(*gpu, 0, 0, 64, 64) == 100);
    CHECK(px(*gpu, 10, 10) != 0 && px(*gpu, 19, 19) != 0 && px(*gpu, 20, 19) == 0 && px(*gpu, 9, 10) == 0);
    // Triangles clip the same way.
    fill(*gpu, 0, 0, 0, 64, 64);
    gpu->gp0(0x200000FFu);
    gpu->gp0(xy(0, 0));
    gpu->gp0(xy(60, 0));
    gpu->gp0(xy(0, 60));
    CHECK(count_nonzero(*gpu, 0, 0, 64, 64) == 100);

    // Draw offset plus 11-bit sign extension: x = 0x7FF (-1) + 3 -> 2.
    auto g2 = make_gpu();
    g2->gp0(0xE5000000u | 3u | (5u << 11));
    g2->gp0(0x680000FFu);  // 1x1 rect
    g2->gp0(0x000007FFu);  // x = -1, y = 0
    CHECK(px(*g2, 2, 5) == rgb15(31, 0, 0));
    CHECK(count_nonzero(*g2, 0, 0, 16, 16) == 1);
    // Mask check protects pixels, set-mask marks new ones.
    g2->gp0(0xE6000001u);
    g2->gp0(0x6800FF00u);
    g2->gp0(xy(0, 0));
    CHECK(px(*g2, 3, 5) == (0x8000u | rgb15(0, 31, 0)));
    g2->gp0(0xE6000002u);
    g2->gp0(0x680000FFu);
    g2->gp0(xy(0, 0));
    CHECK(px(*g2, 3, 5) == (0x8000u | rgb15(0, 31, 0)));
}

void test_polyline() {
    auto gpu = make_gpu();
    gpu->gp0(0x480000FFu);  // flat polyline, red
    gpu->gp0(xy(0, 0));
    gpu->gp0(xy(10, 0));
    gpu->gp0(xy(10, 10));
    gpu->gp0(xy(0, 10));
    gpu->gp0(0x55555555u);  // terminator
    for (int i = 0; i <= 10; ++i) {
        CHECK(px(*gpu, i, 0) == rgb15(31, 0, 0));
        CHECK(px(*gpu, 10, i) == rgb15(31, 0, 0));
        CHECK(px(*gpu, i, 10) == rgb15(31, 0, 0));
    }
    CHECK(px(*gpu, 0, 5) == 0);  // not closed
    // The GPU is back in command mode: a fill executes normally.
    fill(*gpu, 0x0000FF00u, 32, 32, 16, 1);
    CHECK(px(*gpu, 32, 32) == rgb15(0, 31, 0));

    // Gouraud polyline terminated in the color slot, with a 5xxx5xxx pattern other than 55555555h.
    gpu->gp0(0x580000FFu);
    gpu->gp0(xy(0, 100));
    gpu->gp0(0x0000FF00u);
    gpu->gp0(xy(4, 100));
    gpu->gp0(0x00FF0000u);
    gpu->gp0(xy(4, 104));
    gpu->gp0(0x50005000u);
    CHECK(px(*gpu, 0, 100) == rgb15(31, 0, 0) && px(*gpu, 4, 100) == rgb15(0, 31, 0) && px(*gpu, 4, 104) == rgb15(0, 0, 31));
    fill(*gpu, 0x000000FFu, 32, 33, 16, 1);
    CHECK(px(*gpu, 32, 33) == rgb15(31, 0, 0));
}

void test_display() {
    Gpu gpu;
    CHECK(gpu.gpustat() == 0x1C802000u);  // after reset: display off, ready, not interlaced
    CHECK(!gpu.display().enabled);
    gpu.gp1(0x05000000u | 16u | (256u << 10));
    gpu.gp1(0x08000000u | 1u | 0x04u | 0x10u | 0x20u);  // 320 wide, 480 lines, 24-bit, interlaced
    gpu.gp1(0x03000000u);
    gpu.gp1(0x04000002u);
    Gpu::Display d = gpu.display();
    CHECK(d.x == 16 && d.y == 256 && d.width == 320 && d.height == 480);
    CHECK(d.rgb24 && d.interlaced && d.enabled);
    const uint32_t s = gpu.gpustat();
    CHECK(((s >> 17) & 3u) == 1 && (s & (1u << 19)) && (s & (1u << 21)) && (s & (1u << 22)));
    CHECK(!(s & (1u << 23)) && ((s >> 29) & 3u) == 2 && (s & (1u << 25)));
    gpu.gp1(0x08000040u);  // 368 wide, progressive
    d = gpu.display();
    CHECK(d.width == 368 && d.height == 240 && !d.rgb24 && !d.interlaced);
    CHECK(gpu.gpustat() & (1u << 16));

    // GP1(10h) info replies.
    gpu.gp1(0x10000007u);
    CHECK(gpu.gpuread() == 2u);
    gpu.gp0(0xE3000000u | 5u | (6u << 10));
    gpu.gp0(0xE5000000u | 0x7FFu);
    gpu.gp1(0x10000003u);
    CHECK(gpu.gpuread() == (5u | 6u << 10));
    gpu.gp1(0x10000005u);
    CHECK(gpu.gpuread() == 0x7FFu);
    // GP0(1Fh) IRQ and GP1(02h) acknowledge.
    gpu.gp0(0x1F000000u);
    CHECK(gpu.irq_pending() && (gpu.gpustat() & (1u << 24)));
    gpu.gp1(0x02000000u);
    CHECK(!gpu.irq_pending());
    // E1h lands in GPUSTAT bits 0-10; GP1(00h) resets it.
    gpu.gp0(0xE1000000u | 0x7FFu);
    CHECK((gpu.gpustat() & 0x7FFu) == 0x7FFu);
    gpu.gp1(0x00000000u);
    CHECK(gpu.gpustat() == 0x1C802000u);
}

void test_hd_upload() {
    // HD armed: a staged GP0(A0h) upload whose content hash is in the manifest is
    // substituted; unknown content commits verbatim; unarmed GPU is untouched.
    auto gpu = make_gpu();
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dcb_gpu_hd_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "art", ec);
    // 1x1 red PNG (same bytes as tests/vfs/test_vfs.cpp).
    const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
                           0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                           0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
                           0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
                           0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xFB, 0x52, 0x1D, 0x00, 0x00, 0x00,
                           0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    FILE* f = std::fopen((dir / "art" / "r.png").string().c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(png, 1, sizeof png, f) == sizeof png);
    std::fclose(f);
    // Image content = one 16-bit word 0x03E0 (green); manifest maps its hash.
    // The 1x1 PNG replaces 1:1. Odd-pixel uploads pad the word's high half.
    const uint8_t img_bytes[2] = {0xE0, 0x03};
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : img_bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    char manifest[256];
    std::snprintf(manifest, sizeof manifest,
                  "{\"version\":1,\"entries\":[{\"img\":\"%016llx\",\"w\":1,\"h\":1,\"bpp\":16,\"path\":\"r.png\"}]}",
                  (unsigned long long)h);
    const std::string man = (dir / "m.json").string();
    f = std::fopen(man.c_str(), "wb");
    CHECK(f != nullptr);
    std::fwrite(manifest, 1, std::strlen(manifest), f);
    std::fclose(f);

    CHECK(gpu->hd() == nullptr);
    CHECK(gpu->install_hd()->load(man, (dir / "art").string()));
    CHECK(gpu->hd()->enabled());

    // Known content: 1x1 green upload, replaced by the 1x1 red PNG -> 0x001F.
    const uint16_t known[1] = {0x03E0u};
    upload(*gpu, 10, 10, 1, 1, known);
    CHECK(!gpu->receiving_vram());
    CHECK(px(*gpu, 10, 10) == 0x001F);

    // Unknown content: bit-identical fallback.
    const uint16_t unknown[2] = {0x1234u, 0x5678u};
    upload(*gpu, 20, 10, 2, 1, unknown);
    CHECK(px(*gpu, 20, 10) == 0x1234 && px(*gpu, 21, 10) == 0x5678);
    fs::remove_all(dir, ec);
}

void test_hd_upload_snapshot() {
    // Mid-upload save/load with HD armed: feed half the words of a 2x2 upload
    // of unknown content, snapshot, restore into a fresh GPU (also HD-armed),
    // feed the rest — VRAM must equal the uninterrupted verbatim replay.
    auto gpu = make_gpu();
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dcb_gpu_hd_snap_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "art", ec);
    // One unrelated entry: HD arms (enabled) but our content never matches,
    // so every upload replays verbatim through the staged path.
    char manifest[256];
    std::snprintf(manifest, sizeof manifest,
                  "{\"version\":1,\"entries\":[{\"img\":\"ffffffffffffffff\",\"w\":1,\"h\":1,\"bpp\":16,"
                  "\"path\":\"nope.png\"}]}");
    const std::string man = (dir / "m.json").string();
    FILE* f = std::fopen(man.c_str(), "wb");
    CHECK(f != nullptr);
    std::fwrite(manifest, 1, std::strlen(manifest), f);
    std::fclose(f);
    CHECK(gpu->install_hd()->load(man, (dir / "art").string()));
    CHECK(gpu->hd()->enabled());
    gpu->gp0(0xA0000000u);
    gpu->gp0(xy(30, 40));
    gpu->gp0(xy(2, 2));
    gpu->gp0(0x22221111u);  // first two pixels
    CHECK(gpu->receiving_vram());
    Gpu::UploadSnapshot snap = gpu->save_upload();
    CHECK(snap.active && snap.staged.size() == 1 && snap.hd_staging);

    auto gpu2 = make_gpu();
    CHECK(gpu2->install_hd()->load(man, (dir / "art").string()));
    gpu2->load_upload(snap);
    CHECK(gpu2->receiving_vram());
    gpu2->gp0(0x44443333u);  // remaining two pixels
    CHECK(!gpu2->receiving_vram());
    CHECK(px(*gpu2, 30, 40) == 0x1111 && px(*gpu2, 31, 40) == 0x2222);
    CHECK(px(*gpu2, 30, 41) == 0x3333 && px(*gpu2, 31, 41) == 0x4444);

    // Inactive snapshot restores to idle.
    auto gpu3 = make_gpu();
    gpu3->load_upload(Gpu::UploadSnapshot{});
    CHECK(!gpu3->receiving_vram());
    fs::remove_all(dir, ec);
}

struct Case {
    const char* name;
    void (*fn)();
};
constexpr Case kCases[] = {
    {"hd_upload", test_hd_upload},
    {"hd_upload_snapshot", test_hd_upload_snapshot},
    {"fill_rect", test_fill_rect},
    {"vram_transfer", test_vram_transfer},
    {"triangle_fill_rule", test_triangle_fill_rule},
    {"gouraud", test_gouraud},
    {"textured_sprite", test_textured_sprite},
    {"semi_transparency", test_semi_transparency},
    {"draw_area_clipping", test_draw_area_clipping},
    {"polyline", test_polyline},
    {"display", test_display},
};

}  // namespace

int main(int argc, char** argv) {
    int ran = 0;
    for (const Case& c : kCases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        c.fn();
        std::printf("gpu.%s: ok\n", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "unknown test case\n");
        return 1;
    }
    return 0;
}
