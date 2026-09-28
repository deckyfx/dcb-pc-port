#include "gpu/gpu.hpp"

#include "gpu/hd_textures.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>

namespace hle {

namespace {

/// psx-spx "GPU Rendering Attributes / Dithering": offsets added to 8-bit colors before >>3.
constexpr int8_t kDither[4][4] = {
    {-4, +0, -3, +1},
    {+2, -2, +3, -1},
    {-3, +1, -4, +0},
    {+3, -1, +2, -2},
};

/// Sign-extend an 11-bit vertex coordinate (psx-spx "GPU Render Polygon Commands": vertices are
/// signed 11-bit, -1024..+1023).
constexpr int32_t sext11(uint32_t v) {
    return static_cast<int32_t>((v & 0x7FFu) ^ 0x400u) - 0x400;
}

/// 24-bit 0xBBGGRR command color -> 15-bit VRAM pixel (mask bit clear).
constexpr uint16_t to15(uint32_t c) {
    return static_cast<uint16_t>(((c >> 3) & 0x1Fu) | (((c >> 11) & 0x1Fu) << 5) | (((c >> 19) & 0x1Fu) << 10));
}

/// Round-to-nearest division for a positive divisor.
constexpr int64_t div_round(int64_t n, int64_t d) {
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

/// psx-spx "GPU Rendering Attributes / Semi Transparency": B = back (VRAM), F = front.
uint32_t blend(uint32_t back, uint32_t front, uint32_t mode) {
    uint32_t out = 0;
    for (uint32_t sh = 0; sh <= 10; sh += 5) {
        const int32_t bc = static_cast<int32_t>((back >> sh) & 31u);
        const int32_t fc = static_cast<int32_t>((front >> sh) & 31u);
        int32_t rc = 0;
        switch (mode) {
            case 0: rc = (bc + fc) >> 1; break;
            case 1: rc = std::min(bc + fc, 31); break;
            case 2: rc = std::max(bc - fc, 0); break;
            default: rc = std::min(bc + (fc >> 2), 31); break;
        }
        out |= static_cast<uint32_t>(rc) << sh;
    }
    return out;
}

/// Polygons and lines are skipped when any vertex pair is >= 1024 apart horizontally or
/// >= 512 vertically (psx-spx "GPU Render Polygon Commands", "GPU Render Line Commands").
template <typename V>
bool too_far(const V& a, const V& b) {
    return std::abs(a.x - b.x) > 1023 || std::abs(a.y - b.y) > 511;
}

constexpr int64_t kOne = int64_t{1} << 32;  // attribute interpolation: 32.32 fixed point
/// Small positive bias so values that are exact integers never floor to integer-1 because of
/// gradient rounding. Interpolation error stays < 2^-21 for 1024x512 spans, and the true
/// fractional distance to the next integer is >= 1/area >= 2^-20, so floor() stays exact.
constexpr int64_t kInterpBias = int64_t{1} << 11;


/// DCB_TRACE_PRIMS=1: print each distinct textured polygon/rectangle once (its raw GP0 words and
/// the decoded screen position, UV, texpage and CLUT), to find which draw shows which texture.
void trace_prim(const uint32_t* words, std::size_t count, uint32_t draw_mode, uint32_t packet) {
    static const bool on = [] {
        const char* e = std::getenv("DCB_TRACE_PRIMS");
        return e && *e && *e != '0';
    }();
    if (!on) return;
    static std::unordered_set<std::string> seen;
    std::string key(reinterpret_cast<const char*>(words), count * sizeof(uint32_t));
    if (!seen.insert(std::move(key)).second) return;
    std::fprintf(stderr, "[prim] @%06x mode=%04x", packet, draw_mode);
    for (std::size_t i = 0; i < count; ++i) std::fprintf(stderr, " %08x", words[i]);
    std::fputc('\n', stderr);
}

}  // namespace

std::vector<Gpu::SpriteScale> Gpu::parse_sprite_scales(const std::string& text) {
    std::vector<SpriteScale> rules;
    std::size_t pos = 0;
    for (int line_no = 1; pos < text.size(); ++line_no) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        SpriteScale s;
        const int n = std::sscanf(line.c_str(), "%d %d %d %d %d %d %d %d %d %d", &s.tex_x, &s.tex_y, &s.u, &s.v, &s.w,
                                  &s.h, &s.draw_w, &s.draw_h, &s.src_w, &s.src_h);
        if ((n != 8 && n != 10) || s.w <= 0 || s.h <= 0 || s.draw_w <= 0 || s.draw_h <= 0 || s.src_w < 0 ||
            s.src_h < 0 || s.u + (s.src_w ? s.src_w : s.w) > 256 || s.v + (s.src_h ? s.src_h : s.h) > 256) {
            std::fprintf(stderr, "[gpu] sprites.txt line %d: expected tex_x tex_y u v w h draw_w draw_h [src_w src_h]\n",
                         line_no);
            continue;
        }
        rules.push_back(s);
    }
    return rules;
}

Gpu::Gpu() : vram_(static_cast<std::size_t>(kVramWidth) * kVramHeight, 0) { reset(); }

Gpu::~Gpu() = default;

void Gpu::upload_rect(int x, int y, int w, int h, const uint16_t* pixels) {
    if (!pixels || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) at(x + col, y + row) = pixels[row * w + col];
}

// ---------------------------------------------------------------------------------------------
// GP0
// ---------------------------------------------------------------------------------------------

void Gpu::gp0_block(const uint32_t* words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) gp0(words[i]);
}

std::size_t Gpu::command_length(uint32_t cmd) {
    switch (cmd >> 5) {
        case 0: return cmd == 0x02 ? 3 : 1;  // 02h fill rectangle; others NOP / cache / IRQ
        case 1: {                            // polygon
            const std::size_t n = (cmd & 0x08u) ? 4 : 3;
            const std::size_t per_vertex = (cmd & 0x04u) ? 2 : 1;
            return 1 + n * per_vertex + ((cmd & 0x10u) ? n - 1 : 0);
        }
        case 2: return (cmd & 0x10u) ? 4 : 3;  // line / first segment of a polyline
        case 3: return 2 + ((cmd & 0x04u) ? 1 : 0) + (((cmd >> 3) & 3u) == 0 ? 1 : 0);  // rectangle
        case 4: return 4;  // VRAM->VRAM
        case 5: return 3;  // CPU->VRAM header
        case 6: return 3;  // VRAM->CPU header
        default: return 1; // environment
    }
}

void Gpu::gp0(uint32_t word) {
    switch (mode_) {
        case Mode::CpuToVram: write_transfer_word(word); return;
        case Mode::Polyline: polyline_word(word); return;
        case Mode::Command: break;
    }
    if (fifo_len_ == 0) fifo_need_ = command_length(word >> 24);
    fifo_[fifo_len_++] = word;
    if (fifo_len_ < fifo_need_) return;
    execute();
    fifo_len_ = 0;
}

void Gpu::execute() {
    const uint32_t cmd = fifo_[0] >> 24;
    switch (cmd >> 5) {
        case 0:
            if (cmd == 0x02) fill_rect();
            else if (cmd == 0x1F) irq_ = true;  // GP0(1Fh) interrupt request
            break;                              // 00h NOP, 01h clear cache, 03h-1Eh NOP
        case 1:
            if (cmd & 0x04u) trace_prim(fifo_.data(), fifo_need_, draw_mode_, packet_addr_);
            draw_polygon();
            break;
        case 2: draw_line_cmd(); break;
        case 3:
            if (cmd & 0x04u) trace_prim(fifo_.data(), fifo_need_, draw_mode_, packet_addr_);
            draw_rect();
            break;
        case 4: copy_vram(); break;
        case 5: begin_cpu_to_vram(); break;
        case 6: begin_vram_to_cpu(); break;
        default: environment(fifo_[0]); break;
    }
}

void Gpu::environment(uint32_t word) {
    // psx-spx "GPU Rendering Attributes" (E1h-E2h) and "GPU Other Commands" (E3h-E6h).
    switch (word >> 24) {
        case 0xE1: draw_mode_ = word & 0x3FFFu; break;
        case 0xE2: {
            tex_window_ = word & 0xFFFFFu;
            const uint32_t mask_x = word & 0x1Fu, mask_y = (word >> 5) & 0x1Fu;
            const uint32_t off_x = (word >> 10) & 0x1Fu, off_y = (word >> 15) & 0x1Fu;
            // texcoord = (texcoord AND NOT (mask*8)) OR ((offset AND mask)*8)
            tw_and_x_ = ~(mask_x * 8) & 0xFFu;
            tw_and_y_ = ~(mask_y * 8) & 0xFFu;
            tw_or_x_ = (off_x & mask_x) * 8;
            tw_or_y_ = (off_y & mask_y) * 8;
            break;
        }
        case 0xE3:
            area_tl_ = word & 0xFFFFFu;
            area_x1_ = static_cast<int32_t>(word & 0x3FFu);
            area_y1_ = std::min(static_cast<int32_t>((word >> 10) & 0x3FFu), kVramHeight - 1);
            break;
        case 0xE4:
            area_br_ = word & 0xFFFFFu;
            area_x2_ = static_cast<int32_t>(word & 0x3FFu);
            area_y2_ = std::min(static_cast<int32_t>((word >> 10) & 0x3FFu), kVramHeight - 1);
            break;
        case 0xE5:
            offset_raw_ = word & 0x3FFFFFu;
            offset_x_ = sext11(word);
            offset_y_ = sext11(word >> 11);
            break;
        case 0xE6:
            set_mask_ = (word & 1u) != 0;
            check_mask_ = (word & 2u) != 0;
            break;
        default: break;  // E0h, E7h-FFh: NOP
    }
}

Gpu::Vertex Gpu::decode_vertex(uint32_t xy, uint32_t color) const {
    Vertex v;
    v.x = sext11(xy) + offset_x_;
    v.y = sext11(xy >> 16) + offset_y_;
    v.r = static_cast<int32_t>(color & 0xFFu);
    v.g = static_cast<int32_t>((color >> 8) & 0xFFu);
    v.b = static_cast<int32_t>((color >> 16) & 0xFFu);
    return v;
}

Gpu::Prim Gpu::make_prim(uint32_t cmd, bool textured, uint32_t texpage, uint32_t clut) const {
    Prim p;
    // GP1(09h) allows E1h/texpage bit 11 to disable texturing.
    p.textured = textured && !(allow_tex_disable_ && (texpage & 0x800u));
    p.raw = (cmd & 0x01u) != 0;
    p.semi = (cmd & 0x02u) != 0;
    p.semi_mode = (texpage >> 5) & 3u;
    p.depth = (texpage >> 7) & 3u;
    p.tex_x = static_cast<int32_t>(texpage & 0xFu) * 64;
    p.tex_y = static_cast<int32_t>((texpage >> 4) & 1u) * 256;
    p.clut_x = static_cast<int32_t>(clut & 0x3Fu) * 16;
    p.clut_y = static_cast<int32_t>((clut >> 6) & 0x1FFu);
    return p;
}

void Gpu::draw_polygon() {
    // psx-spx "GPU Render Polygon Commands": cmd bit 4 gouraud, 3 quad, 2 textured, 1 semi, 0 raw.
    const uint32_t cmd = fifo_[0] >> 24;
    const bool gouraud = (cmd & 0x10u) != 0, textured = (cmd & 0x04u) != 0;
    const int n = (cmd & 0x08u) ? 4 : 3;
    Vertex v[4];
    uint32_t clut = 0, texpage = draw_mode_, color = fifo_[0];
    std::size_t i = 1;
    for (int k = 0; k < n; ++k) {
        if (gouraud && k > 0) color = fifo_[i++];
        v[k] = decode_vertex(fifo_[i++], color);
        if (textured) {
            const uint32_t t = fifo_[i++];
            v[k].u = static_cast<int32_t>(t & 0xFFu);
            v[k].v = static_cast<int32_t>((t >> 8) & 0xFFu);
            if (k == 0) clut = t >> 16;
            if (k == 1) texpage = t >> 16;
        }
    }
    if (textured) {
        // The texpage attribute replaces E1h bits 0-8 (and 11 when allowed), visible in GPUSTAT.
        draw_mode_ = (draw_mode_ & ~0x9FFu) | (texpage & 0x1FFu) | (allow_tex_disable_ ? texpage & 0x800u : 0u);
    }
    Prim p = make_prim(cmd, textured, texpage, clut);
    p.gouraud = gouraud;
    p.dither = (draw_mode_ & 0x200u) && (gouraud || (p.textured && !p.raw));
    draw_triangle(v[0], v[1], v[2], p);
    if (n == 4) draw_triangle(v[1], v[2], v[3], p);  // quads split into 1-2-3 and 2-3-4
}

void Gpu::draw_line_cmd() {
    // psx-spx "GPU Render Line Commands": bit 4 gouraud, bit 3 polyline, bit 1 semi-transparent.
    const uint32_t cmd = fifo_[0] >> 24;
    const bool gouraud = (cmd & 0x10u) != 0;
    Prim p = make_prim(cmd, false, draw_mode_, 0);
    p.gouraud = gouraud;
    p.dither = gouraud && (draw_mode_ & 0x200u);
    const Vertex a = decode_vertex(fifo_[1], fifo_[0]);
    const Vertex b = gouraud ? decode_vertex(fifo_[3], fifo_[2]) : decode_vertex(fifo_[2], fifo_[0]);
    draw_line(a, b, p);
    if (cmd & 0x08u) {
        mode_ = Mode::Polyline;
        poly_last_ = b;
        poly_color_ = fifo_[0];
        poly_gouraud_ = gouraud;
        poly_have_color_ = false;
        poly_prim_ = p;
    }
}

void Gpu::polyline_word(uint32_t word) {
    // Polylines end with a 5xxx5xxxh word, accepted in either the color or the vertex slot.
    if ((word & 0xF000F000u) == 0x50005000u) {
        mode_ = Mode::Command;
        return;
    }
    if (poly_gouraud_ && !poly_have_color_) {
        poly_color_ = word;
        poly_have_color_ = true;
        return;
    }
    const Vertex v = decode_vertex(word, poly_color_);
    draw_line(poly_last_, v, poly_prim_);
    poly_last_ = v;
    poly_have_color_ = false;
}

void Gpu::draw_rect() {
    // psx-spx "GPU Render Rectangle Commands": bits 3-4 size (variable, 1x1, 8x8, 16x16),
    // bit 2 textured, bit 1 semi, bit 0 raw. Texture page comes from E1h; never dithered.
    const uint32_t cmd = fifo_[0] >> 24;
    const bool textured = (cmd & 0x04u) != 0;
    std::size_t i = 2;
    const uint32_t tex = textured ? fifo_[i++] : 0;
    int32_t w = 0, h = 0;
    switch ((cmd >> 3) & 3u) {
        case 0:
            w = static_cast<int32_t>(fifo_[i] & 0x3FFu);
            h = static_cast<int32_t>((fifo_[i] >> 16) & 0x1FFu);
            break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        default: w = h = 16; break;
    }
    const Prim p = make_prim(cmd, textured, draw_mode_, tex >> 16);
    Vertex origin = decode_vertex(fifo_[1], fifo_[0]);
    origin.u = static_cast<int32_t>(tex & 0xFFu);
    origin.v = static_cast<int32_t>((tex >> 8) & 0xFFu);
    if (p.textured) {
        for (const SpriteScale& s : sprite_scales_) {
            if (s.tex_x != p.tex_x || s.tex_y != p.tex_y || s.u != origin.u || s.v != origin.v || s.w != w || s.h != h) continue;
            const int32_t x0 = origin.x + (w - s.draw_w) / 2, y0 = origin.y + (h - s.draw_h) / 2;
            const int32_t sw = s.src_w ? s.src_w : w, sh = s.src_h ? s.src_h : h;
            if (sw == s.draw_w && sh == s.draw_h) {
                // Same texel and pixel size: still a plain sprite, only larger.
                Vertex o = origin;
                o.x = x0;
                o.y = y0;
                draw_sprite(o, sw, sh, p);
                return;
            }
            // Two triangles covering the scaled rectangle; UVs span the texels exactly.
            Vertex q[4];
            for (int k = 0; k < 4; ++k) {
                q[k] = origin;
                q[k].x = x0 + ((k & 1) ? s.draw_w : 0);
                q[k].y = y0 + ((k & 2) ? s.draw_h : 0);
                q[k].u = origin.u + ((k & 1) ? sw : 0);
                q[k].v = origin.v + ((k & 2) ? sh : 0);
            }
            draw_triangle(q[0], q[1], q[2], p);  // sprites are never dithered or shaded: p as is
            draw_triangle(q[1], q[2], q[3], p);
            return;
        }
    }
    draw_sprite(origin, w, h, p);
}

void Gpu::fill_rect() {
    // psx-spx "GPU Memory Transfer Commands / GP0(02h)": X is 16-pixel aligned, width rounds up
    // to a multiple of 16; ignores draw area, draw offset and mask settings; wraps in VRAM.
    const uint16_t color = to15(fifo_[0]);
    const int32_t x = static_cast<int32_t>(fifo_[1] & 0x3F0u);
    const int32_t y = static_cast<int32_t>((fifo_[1] >> 16) & 0x1FFu);
    const int32_t w = static_cast<int32_t>(((fifo_[2] & 0x3FFu) + 0xFu) & ~0xFu);
    const int32_t h = static_cast<int32_t>((fifo_[2] >> 16) & 0x1FFu);
    for (int32_t row = 0; row < h; ++row) {
        const int32_t py = (y + row) & (kVramHeight - 1);
        if (x + w <= kVramWidth) {
            uint16_t* dst = &vram_[static_cast<std::size_t>(py * kVramWidth + x)];
            std::fill(dst, dst + w, color);
        } else {
            for (int32_t col = 0; col < w; ++col) at(x + col, py) = color;
        }
    }
}

void Gpu::put_masked(int32_t x, int32_t y, uint16_t pixel) {
    uint16_t& dst = at(x, y);
    if (check_mask_ && (dst & 0x8000u)) return;
    dst = static_cast<uint16_t>(pixel | (set_mask_ ? 0x8000u : 0u));
}

void Gpu::copy_vram() {
    // psx-spx "GPU Memory Transfer Commands / GP0(80h)": sizes 0 mean the maximum; coordinates
    // wrap; honours the E6h mask settings.
    const int32_t sx = static_cast<int32_t>(fifo_[1] & 0x3FFu), sy = static_cast<int32_t>((fifo_[1] >> 16) & 0x1FFu);
    const int32_t dx = static_cast<int32_t>(fifo_[2] & 0x3FFu), dy = static_cast<int32_t>((fifo_[2] >> 16) & 0x1FFu);
    const int32_t w = static_cast<int32_t>(((fifo_[3] & 0xFFFFu) - 1) & 0x3FFu) + 1;
    const int32_t h = static_cast<int32_t>((((fifo_[3] >> 16) & 0xFFFFu) - 1) & 0x1FFu) + 1;
    std::array<uint16_t, kVramWidth> line{};
    for (int32_t row = 0; row < h; ++row) {
        for (int32_t col = 0; col < w; ++col) line[static_cast<std::size_t>(col)] = at(sx + col, sy + row);
        for (int32_t col = 0; col < w; ++col) put_masked(dx + col, dy + row, line[static_cast<std::size_t>(col)]);
    }
}

HdTextures* Gpu::install_hd() {
    if (!hd_) hd_ = std::make_unique<HdTextures>();
    return hd_.get();
}

void Gpu::begin_cpu_to_vram() {
    // psx-spx "GP0(A0h)": destination, size (0 = max), then ceil(w*h/2) data words.
    write_.x = static_cast<int32_t>(fifo_[1] & 0x3FFu);
    write_.y = static_cast<int32_t>((fifo_[1] >> 16) & 0x1FFu);
    write_.w = static_cast<int32_t>(((fifo_[2] & 0xFFFFu) - 1) & 0x3FFu) + 1;
    write_.h = static_cast<int32_t>((((fifo_[2] >> 16) & 0xFFFFu) - 1) & 0x1FFu) + 1;
    write_.cx = write_.cy = 0;
    write_.remaining = static_cast<uint32_t>(write_.w * write_.h);
    mode_ = Mode::CpuToVram;
    // When HD replacement is armed, stage the words instead of writing through:
    // commit_staged_upload() hashes the full rect before anything hits VRAM.
    hd_staging_ = hd_ && hd_->enabled();
    if (hd_staging_) {
        const size_t pixels = static_cast<size_t>(write_.w) * static_cast<size_t>(write_.h);
        // Cap staging at 1M words (512x1024 max rect is 256K words); bigger rects
        // can only come from corrupt headers, so fall back to direct writes.
        if (pixels <= (1u << 20) * 2) {
            staged_.clear();
            staged_.reserve((pixels + 1) / 2);
        } else {
            hd_staging_ = false;
        }
    }
}

void Gpu::commit_staged_upload() {
    mode_ = Mode::Command;
    int out_w = write_.w, out_h = write_.h;  // a replacement may fill a larger slot
    const std::vector<uint16_t>* replacement =
        hd_ ? hd_->maybe_replace(write_.x, write_.y, write_.w, write_.h, staged_.data(), staged_.size(), &out_w, &out_h)
            : nullptr;
    if (replacement && replacement->size() == static_cast<size_t>(out_w) * static_cast<size_t>(out_h)) {
        // HD hit: commit the substitute pixels through the same masked path.
        size_t i = 0;
        for (int32_t row = 0; row < out_h; ++row) {
            for (int32_t col = 0; col < out_w; ++col) put_masked(write_.x + col, write_.y + row, (*replacement)[i++]);
        }
    } else {
        // Miss or disabled mid-transfer: replay the original words verbatim.
        // write_transfer_word() zeroed write_.remaining when staging completed,
        // so restore the cursor before replaying.
        write_.cx = write_.cy = 0;
        write_.remaining = static_cast<uint32_t>(static_cast<size_t>(write_.w) * static_cast<size_t>(write_.h));
        for (uint32_t word : staged_) write_transfer_word_direct(word);
    }
    staged_.clear();
    hd_staging_ = false;
}

void Gpu::write_transfer_word_direct(uint32_t word) {
    for (int half = 0; half < 2 && write_.remaining != 0; ++half) {
        put_masked(write_.x + write_.cx, write_.y + write_.cy, static_cast<uint16_t>(word >> (16 * half)));
        if (++write_.cx == write_.w) {
            write_.cx = 0;
            ++write_.cy;
        }
        --write_.remaining;
    }
    if (write_.remaining == 0) mode_ = Mode::Command;
}

void Gpu::write_transfer_word(uint32_t word) {
    if (hd_staging_) {
        staged_.push_back(word);
        // ceil(pixels/2) words complete the rect; odd rects pad the last half.
        const size_t pixels = static_cast<size_t>(write_.w) * static_cast<size_t>(write_.h);
        write_.remaining = pixels > staged_.size() * 2 ? static_cast<uint32_t>(pixels - staged_.size() * 2) : 0;
        if (write_.remaining == 0) commit_staged_upload();
        return;
    }
    write_transfer_word_direct(word);
}

void Gpu::begin_vram_to_cpu() {
    // psx-spx "GP0(C0h)": source, size, then the data is read through GPUREAD.
    read_.x = static_cast<int32_t>(fifo_[1] & 0x3FFu);
    read_.y = static_cast<int32_t>((fifo_[1] >> 16) & 0x1FFu);
    read_.w = static_cast<int32_t>(((fifo_[2] & 0xFFFFu) - 1) & 0x3FFu) + 1;
    read_.h = static_cast<int32_t>((((fifo_[2] >> 16) & 0xFFFFu) - 1) & 0x1FFu) + 1;
    read_.cx = read_.cy = 0;
    read_.remaining = static_cast<uint32_t>(read_.w * read_.h);
}

uint32_t Gpu::gpuread() {
    if (read_.remaining == 0) return gpuread_latch_;
    uint32_t word = 0;
    for (int half = 0; half < 2 && read_.remaining != 0; ++half) {
        word |= static_cast<uint32_t>(at(read_.x + read_.cx, read_.y + read_.cy)) << (16 * half);
        if (++read_.cx == read_.w) {
            read_.cx = 0;
            ++read_.cy;
        }
        --read_.remaining;
    }
    gpuread_latch_ = word;
    return word;
}

// ---------------------------------------------------------------------------------------------
// Pixel pipeline
// ---------------------------------------------------------------------------------------------

uint16_t Gpu::fetch_texel(const Prim& p, int32_t u, int32_t v) const {
    // psx-spx "GPU Rendering Attributes / Texture Window" and "Textures / CLUT".
    const int32_t tu = static_cast<int32_t>((static_cast<uint32_t>(u) & tw_and_x_) | tw_or_x_);
    const int32_t tv = static_cast<int32_t>((static_cast<uint32_t>(v) & tw_and_y_) | tw_or_y_);
    switch (p.depth) {
        case 0: {
            const uint32_t w = at(p.tex_x + (tu >> 2), p.tex_y + tv);
            return at(p.clut_x + static_cast<int32_t>((w >> ((tu & 3) * 4)) & 0xFu), p.clut_y);
        }
        case 1: {
            const uint32_t w = at(p.tex_x + (tu >> 1), p.tex_y + tv);
            return at(p.clut_x + static_cast<int32_t>((w >> ((tu & 1) * 8)) & 0xFFu), p.clut_y);
        }
        default: return at(p.tex_x + tu, p.tex_y + tv);  // 2 = 15-bit, 3 = reserved (acts as 15-bit)
    }
}

void Gpu::plot(int32_t x, int32_t y, int32_t r, int32_t g, int32_t b, int32_t u, int32_t v, const Prim& p) {
    uint16_t& dst = at(x, y);
    if (check_mask_ && (dst & 0x8000u)) return;
    uint32_t mask = set_mask_ ? 0x8000u : 0u;
    bool semi = p.semi;
    uint32_t color = 0;
    if (p.textured) {
        const uint32_t texel = fetch_texel(p, u, v);
        if (texel == 0) return;  // 0000h is fully transparent
        mask |= texel & 0x8000u;
        semi = semi && (texel & 0x8000u);  // only texels with bit 15 set are blended
        if (p.raw) {
            color = texel & 0x7FFFu;
        } else {
            // Modulation: texel * color / 80h, computed at 8-bit precision so it can be dithered.
            r = (static_cast<int32_t>(texel & 31u) * r) >> 4;
            g = (static_cast<int32_t>((texel >> 5) & 31u) * g) >> 4;
            b = (static_cast<int32_t>((texel >> 10) & 31u) * b) >> 4;
        }
    }
    if (!(p.textured && p.raw)) {
        if (p.dither) {
            const int32_t d = kDither[y & 3][x & 3];
            r += d;
            g += d;
            b += d;
        }
        color = static_cast<uint32_t>(std::clamp(r, 0, 255) >> 3) | static_cast<uint32_t>(std::clamp(g, 0, 255) >> 3) << 5 |
                static_cast<uint32_t>(std::clamp(b, 0, 255) >> 3) << 10;
    }
    if (semi) color = blend(dst, color, p.semi_mode);
    dst = static_cast<uint16_t>(color | mask);
}

// ---------------------------------------------------------------------------------------------
// Rasterizers
// ---------------------------------------------------------------------------------------------

void Gpu::draw_triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Prim& p) {
    if (too_far(v0, v1) || too_far(v1, v2) || too_far(v2, v0)) return;
    const Vertex* a = &v0;
    const Vertex* b = &v1;
    const Vertex* c = &v2;
    int64_t area = int64_t{b->x - a->x} * (c->y - a->y) - int64_t{b->y - a->y} * (c->x - a->x);
    if (area == 0) return;
    if (area < 0) {  // normalise winding so every edge function is >= 0 inside
        std::swap(b, c);
        area = -area;
    }

    const int32_t minx = std::max(std::min({a->x, b->x, c->x}), area_x1_);
    const int32_t maxx = std::min(std::max({a->x, b->x, c->x}), area_x2_);
    const int32_t miny = std::max(std::min({a->y, b->y, c->y}), area_y1_);
    const int32_t maxy = std::min(std::max({a->y, b->y, c->y}), area_y2_);
    if (minx > maxx || miny > maxy) return;

    // Edge functions sampled at integer pixel positions. Top-left fill rule: pixels exactly on a
    // top or left edge are drawn, on a right or bottom edge they are not (psx-spx notes that the
    // right/bottom edges of polygons are excluded).
    struct Edge {
        int32_t step_x, step_y, row;
    };
    auto make_edge = [&](const Vertex& s, const Vertex& t) {
        const int32_t dx = t.x - s.x, dy = t.y - s.y;
        const int32_t bias = (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
        return Edge{-dy, dx, dx * (miny - s.y) - dy * (minx - s.x) + bias};
    };
    Edge e0 = make_edge(*b, *c), e1 = make_edge(*c, *a), e2 = make_edge(*a, *b);

    // Attribute planes (r, g, b, u, v) in 32.32 fixed point, evaluated at (minx, row).
    const bool interp_color = p.gouraud && !(p.textured && p.raw);
    const bool interp_uv = p.textured;
    int64_t row_val[5] = {}, gx[5] = {}, gy[5] = {};
    auto setup = [&](int k, int32_t a0, int32_t a1, int32_t a2) {
        const int64_t d1 = a1 - a0, d2 = a2 - a0;
        const int64_t nx = d1 * (c->y - a->y) - d2 * (b->y - a->y);
        const int64_t ny = d2 * (b->x - a->x) - d1 * (c->x - a->x);
        gx[k] = div_round(nx * kOne, area);
        gy[k] = div_round(ny * kOne, area);
        row_val[k] = int64_t{a0} * kOne + gx[k] * (minx - a->x) + gy[k] * (miny - a->y) + kInterpBias;
    };
    if (interp_color) {
        setup(0, a->r, b->r, c->r);
        setup(1, a->g, b->g, c->g);
        setup(2, a->b, b->b, c->b);
    }
    if (interp_uv) {
        setup(3, a->u, b->u, c->u);
        setup(4, a->v, b->v, c->v);
    }

    int32_t r = a->r, g = a->g, bl = a->b, u = 0, v = 0;
    for (int32_t y = miny; y <= maxy; ++y) {
        int32_t w0 = e0.row, w1 = e1.row, w2 = e2.row;
        int64_t val[5];
        std::copy(std::begin(row_val), std::end(row_val), val);
        bool entered = false;
        for (int32_t x = minx; x <= maxx; ++x) {
            if ((w0 | w1 | w2) >= 0) {
                entered = true;
                if (interp_color) {
                    r = static_cast<int32_t>(val[0] >> 32);
                    g = static_cast<int32_t>(val[1] >> 32);
                    bl = static_cast<int32_t>(val[2] >> 32);
                }
                if (interp_uv) {
                    u = static_cast<int32_t>(val[3] >> 32);
                    v = static_cast<int32_t>(val[4] >> 32);
                }
                plot(x, y, r, g, bl, u, v, p);
            } else if (entered) {
                break;  // convex: once the span is left, the rest of the row is outside
            }
            w0 += e0.step_x;
            w1 += e1.step_x;
            w2 += e2.step_x;
            if (interp_color)
                for (int k = 0; k < 3; ++k) val[k] += gx[k];
            if (interp_uv)
                for (int k = 3; k < 5; ++k) val[k] += gx[k];
        }
        e0.row += e0.step_y;
        e1.row += e1.step_y;
        e2.row += e2.step_y;
        for (int k = 0; k < 5; ++k) row_val[k] += gy[k];
    }
}

void Gpu::draw_line(const Vertex& a, const Vertex& b, const Prim& p) {
    // DDA in 16.16 fixed point; both end points are drawn.
    if (too_far(a, b)) return;
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    const int32_t steps = std::max(std::abs(dx), std::abs(dy));
    constexpr int64_t kHalf = int64_t{1} << 15;
    int64_t x = int64_t{a.x} * 65536 + kHalf, y = int64_t{a.y} * 65536 + kHalf;
    int64_t r = int64_t{a.r} * 65536 + kHalf, g = int64_t{a.g} * 65536 + kHalf, bl = int64_t{a.b} * 65536 + kHalf;
    int64_t sx = 0, sy = 0, sr = 0, sg = 0, sb = 0;
    if (steps > 0) {
        sx = int64_t{dx} * 65536 / steps;
        sy = int64_t{dy} * 65536 / steps;
        if (p.gouraud) {
            sr = int64_t{b.r - a.r} * 65536 / steps;
            sg = int64_t{b.g - a.g} * 65536 / steps;
            sb = int64_t{b.b - a.b} * 65536 / steps;
        }
    }
    for (int32_t i = 0; i <= steps; ++i) {
        const auto px = static_cast<int32_t>(x >> 16), py = static_cast<int32_t>(y >> 16);
        if (px >= area_x1_ && px <= area_x2_ && py >= area_y1_ && py <= area_y2_)
            plot(px, py, static_cast<int32_t>(r >> 16), static_cast<int32_t>(g >> 16), static_cast<int32_t>(bl >> 16), 0, 0, p);
        x += sx;
        y += sy;
        r += sr;
        g += sg;
        bl += sb;
    }
}

void Gpu::draw_sprite(const Vertex& o, int32_t w, int32_t h, const Prim& p) {
    const int32_t x0 = std::max(o.x, area_x1_), x1 = std::min(o.x + w - 1, area_x2_);
    const int32_t y0 = std::max(o.y, area_y1_), y1 = std::min(o.y + h - 1, area_y2_);
    if (x0 > x1 || y0 > y1) return;

    if (!p.textured && !p.semi && !check_mask_) {  // plain opaque rectangle: straight row fills
        const uint16_t color = static_cast<uint16_t>(to15(static_cast<uint32_t>(o.r | o.g << 8 | o.b << 16)) |
                                                     (set_mask_ ? 0x8000u : 0u));
        for (int32_t y = y0; y <= y1; ++y) {
            uint16_t* row = &vram_[static_cast<std::size_t>(y * kVramWidth)];
            std::fill(row + x0, row + x1 + 1, color);
        }
        return;
    }

    // psx-spx "GP0(E1h)" bits 12/13: textured rectangle X/Y flip.
    const int32_t du = (draw_mode_ & 0x1000u) ? -1 : 1, dv = (draw_mode_ & 0x2000u) ? -1 : 1;
    for (int32_t y = y0; y <= y1; ++y) {
        const int32_t v = (o.v + dv * (y - o.y)) & 0xFF;
        int32_t u = (o.u + du * (x0 - o.x)) & 0xFF;
        for (int32_t x = x0; x <= x1; ++x) {
            plot(x, y, o.r, o.g, o.b, u, v, p);
            u = (u + du) & 0xFF;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// GP1 / status
// ---------------------------------------------------------------------------------------------

Gpu::UploadSnapshot Gpu::save_upload() const {
    UploadSnapshot snap;
    snap.x = write_.x;
    snap.y = write_.y;
    snap.w = write_.w;
    snap.h = write_.h;
    snap.cx = write_.cx;
    snap.cy = write_.cy;
    snap.remaining = write_.remaining;
    snap.staged = staged_;
    snap.hd_staging = hd_staging_;
    snap.active = mode_ == Mode::CpuToVram;
    return snap;
}

void Gpu::load_upload(const UploadSnapshot& snap) {
    write_.x = snap.x;
    write_.y = snap.y;
    write_.w = snap.w;
    write_.h = snap.h;
    write_.cx = snap.cx;
    write_.cy = snap.cy;
    write_.remaining = snap.remaining;
    staged_ = snap.staged;
    // A snapshot taken with HD armed but loaded without it (or vice versa)
    // still replays the same words; the flag only selects staged vs direct.
    hd_staging_ = snap.hd_staging && hd_ && hd_->enabled();
    mode_ = snap.active ? Mode::CpuToVram : Mode::Command;
    if (mode_ != Mode::CpuToVram) {
        staged_.clear();
        hd_staging_ = false;
    }
}

void Gpu::reset_command_buffer() {
    fifo_len_ = 0;
    mode_ = Mode::Command;
    write_.remaining = 0;
    read_.remaining = 0;
    staged_.clear();  // drop any half-staged HD upload (GP1(01h) mid-transfer)
    hd_staging_ = false;
}

void Gpu::reset() {
    // psx-spx "GP1(00h)": acts as GP1(01h), (02h), (03h,1), (04h,0), (05h,0), (06h,C00200h),
    // (07h,040010h), (08h,0) plus GP0(E1h..E6h) with all-zero parameters.
    reset_command_buffer();
    irq_ = false;
    display_disabled_ = true;
    dma_dir_ = 0;
    display_start_ = 0;
    hrange_ = 0xC00200u;
    vrange_ = 0x040010u;
    display_mode_ = 0;
    for (uint32_t cmd = 0xE1; cmd <= 0xE6; ++cmd) environment(cmd << 24);
}

void Gpu::gp1(uint32_t word) {
    // psx-spx "GPU Display Control Commands (GP1)". 40h-FFh mirror 00h-3Fh.
    const uint32_t cmd = (word >> 24) & 0x3Fu;
    switch (cmd) {
        case 0x00: reset(); break;
        case 0x01: reset_command_buffer(); break;
        case 0x02: irq_ = false; break;
        case 0x03: display_disabled_ = (word & 1u) != 0; break;
        case 0x04: dma_dir_ = word & 3u; break;
        case 0x05: display_start_ = word & 0x7FFFFu; break;
        case 0x06: hrange_ = word & 0xFFFFFFu; break;
        case 0x07: vrange_ = word & 0xFFFFFu; break;
        case 0x08: display_mode_ = word & 0xFFu; break;
        case 0x09: allow_tex_disable_ = (word & 1u) != 0; break;
        default:
            if (cmd >= 0x10 && cmd <= 0x1F) {
                // GPU info ("GP1(10h)"): unlisted indices leave GPUREAD unchanged.
                switch (word & 0xFu) {
                    case 2: gpuread_latch_ = tex_window_; break;
                    case 3: gpuread_latch_ = area_tl_; break;
                    case 4: gpuread_latch_ = area_br_; break;
                    case 5: gpuread_latch_ = offset_raw_; break;
                    case 7: gpuread_latch_ = 2; break;  // GPU type: 2 = CXD8561 "new" GPU
                    case 8: gpuread_latch_ = 0; break;
                    default: break;
                }
            }
            break;  // 0Ah-0Fh, 20h-3Fh: not modelled
    }
}

uint32_t Gpu::gpustat() const {
    // psx-spx "GPU Status Register (GPUSTAT)".
    const bool interlace = (display_mode_ & 0x20u) != 0;
    uint32_t s = draw_mode_ & 0x7FFu;                   // 0-10 from E1h
    if (set_mask_) s |= 1u << 11;
    if (check_mask_) s |= 1u << 12;
    if (!interlace || field_) s |= 1u << 13;            // interlace field (1 when progressive)
    if (display_mode_ & 0x80u) s |= 1u << 14;           // "reverseflag"
    if (draw_mode_ & 0x800u) s |= 1u << 15;             // texture disable
    s |= (display_mode_ & 0x40u) << 10;                 // 16: horizontal resolution 2
    s |= (display_mode_ & 0x3Fu) << 17;                 // 17-22: hres1, vres, video mode, depth, interlace
    if (display_disabled_) s |= 1u << 23;
    if (irq_) s |= 1u << 24;
    if (dma_dir_ != 0) s |= 1u << 25;                   // DMA request: always ready
    s |= 7u << 26;                                      // ready: command / VRAM->CPU / DMA block
    s |= dma_dir_ << 29;
    if (interlace && field_) s |= 1u << 31;             // odd line being drawn
    return s;
}

void Gpu::vblank() { field_ = !field_; }

Gpu::Display Gpu::display() const {
    static constexpr int kWidths[4] = {256, 320, 512, 640};
    Display d;
    d.x = static_cast<int>(display_start_ & 0x3FFu);
    d.y = static_cast<int>((display_start_ >> 10) & 0x1FFu);
    d.width = (display_mode_ & 0x40u) ? 368 : kWidths[display_mode_ & 3u];
    // Vertical resolution 480 requires interlace (GP1(08h) bits 2 and 5). The PAL/NTSC line count
    // is not reflected here; it comes from GP1(07h) if ever needed.
    d.height = ((display_mode_ & 0x04u) && (display_mode_ & 0x20u)) ? 480 : 240;
    d.rgb24 = (display_mode_ & 0x10u) != 0;
    d.interlaced = (display_mode_ & 0x20u) != 0;
    d.enabled = !display_disabled_;
    return d;
}

// ---------------------------------------------------------------------------------------------
// Save state
// ---------------------------------------------------------------------------------------------

void Gpu::save_state(psx::StateWriter& w) const {
    w.begin(psx::state_tag("GPU "), 2);
    w.vec(vram_);
    w.pod(mode_);
    w.pod(fifo_);
    w.size(fifo_len_);
    w.size(fifo_need_);
    w.pod(write_);
    w.pod(read_);
    w.pod(poly_last_);
    w.u32(poly_color_);
    w.boolean(poly_gouraud_);
    w.boolean(poly_have_color_);
    w.pod(poly_prim_);
    w.u32(draw_mode_);
    w.u32(tex_window_);
    w.u32(area_tl_);
    w.u32(area_br_);
    w.u32(offset_raw_);
    for (const int32_t v : {area_x1_, area_y1_, area_x2_, area_y2_, offset_x_, offset_y_}) w.pod(v);
    for (const uint32_t v : {tw_and_x_, tw_or_x_, tw_and_y_, tw_or_y_}) w.u32(v);
    w.boolean(set_mask_);
    w.boolean(check_mask_);
    w.u32(display_mode_);
    w.u32(display_start_);
    w.u32(hrange_);
    w.u32(vrange_);
    w.u32(dma_dir_);
    w.boolean(display_disabled_);
    w.boolean(irq_);
    w.boolean(allow_tex_disable_);
    w.boolean(field_);
    w.u32(gpuread_latch_);
    // HD texture replacement: an upload staged mid-transfer (a VBLANK yield can land between
    // its data words) and the replacer's palette sniffer; fitted art is re-derived, not saved.
    w.vec(staged_);
    w.boolean(hd_staging_);
    const bool hd = hd_ != nullptr;
    w.boolean(hd);
    if (hd) {
        const HdTextures::Snapshot snap = hd_->save();
        w.u64(snap.last_clut);
        w.boolean(snap.have_clut);
        w.size(snap.clut_lru.size());  // cache contents in eviction order (front first)
        for (const uint64_t hash : snap.clut_lru) {
            w.u64(hash);
            w.vec(snap.clut_cache.at(hash));
        }
        for (const uint64_t v : {snap.hits, snap.misses, snap.fit_hits, snap.miss_shape, snap.miss_no_palette,
                                 snap.miss_palette_not_live, snap.miss_palette_shape})
            w.u64(v);
    }
    w.end();
}

void Gpu::load_state(psx::StateReader& r) {
    r.begin(psx::state_tag("GPU "), 2);
    r.vec(vram_, vram_.size(), static_cast<std::size_t>(kVramWidth) * kVramHeight);
    r.pod(mode_);
    if (mode_ != Mode::Command && mode_ != Mode::CpuToVram && mode_ != Mode::Polyline) r.fail("bad GPU mode");
    r.pod(fifo_);
    fifo_len_ = r.size(fifo_.size(), 0);
    fifo_need_ = r.size(fifo_.size(), 0);
    r.pod(write_);
    r.pod(read_);
    r.pod(poly_last_);
    poly_color_ = r.u32();
    poly_gouraud_ = r.boolean();
    poly_have_color_ = r.boolean();
    r.pod(poly_prim_);
    draw_mode_ = r.u32();
    tex_window_ = r.u32();
    area_tl_ = r.u32();
    area_br_ = r.u32();
    offset_raw_ = r.u32();
    for (int32_t* v : {&area_x1_, &area_y1_, &area_x2_, &area_y2_, &offset_x_, &offset_y_}) r.pod(*v);
    for (uint32_t* v : {&tw_and_x_, &tw_or_x_, &tw_and_y_, &tw_or_y_}) *v = r.u32();
    set_mask_ = r.boolean();
    check_mask_ = r.boolean();
    display_mode_ = r.u32();
    display_start_ = r.u32();
    hrange_ = r.u32();
    vrange_ = r.u32();
    dma_dir_ = r.u32();
    display_disabled_ = r.boolean();
    irq_ = r.boolean();
    allow_tex_disable_ = r.boolean();
    field_ = r.boolean();
    gpuread_latch_ = r.u32();
    // Upload staging: at most a full-VRAM rect of words (see begin_cpu_to_vram).
    r.vec(staged_, static_cast<std::size_t>(kVramWidth) * kVramHeight);
    hd_staging_ = r.boolean();
    if (r.boolean()) {
        HdTextures::Snapshot snap;
        snap.last_clut = r.u64();
        snap.have_clut = r.boolean();
        const std::size_t cluts = r.size(1024);
        for (std::size_t i = 0; i < cluts; ++i) {
            const uint64_t hash = r.u64();
            std::vector<uint16_t> pal;
            r.vec(pal, 4096);
            snap.clut_lru.push_back(hash);
            snap.clut_cache[hash] = std::move(pal);
        }
        for (uint64_t* v : {&snap.hits, &snap.misses, &snap.fit_hits, &snap.miss_shape, &snap.miss_no_palette,
                            &snap.miss_palette_not_live, &snap.miss_palette_shape})
            *v = r.u64();
        if (hd_) hd_->load_snapshot(snap);  // same process: the replacer is configured as when saved
    } else if (hd_) {
        hd_->reset_runtime();
    }
    if (hd_staging_ && !(hd_ && hd_->enabled())) r.fail("state has an HD upload in flight but HD replacement is off");
    r.end();
}

}  // namespace hle
